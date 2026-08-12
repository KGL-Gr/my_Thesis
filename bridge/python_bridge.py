import json
import logging
import signal
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional, Tuple
from datetime import datetime, timezone
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

import joblib
import pandas as pd
import paho.mqtt.client as mqtt
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS

TTN_BROKER = ""
TTN_PORT = 1883
TTN_USERNAME = ""          
TTN_API_KEY = ""
TTN_APP_ID = ""
TTN_MQTT_TOPIC = ""

INFLUX_URL = ""
INFLUX_TOKEN = ""
INFLUX_ORG = ""
INFLUX_BUCKET = ""

MODEL_PATH = "name.joblib"

# My models use:
# 0 = FIRE
# 1 = NO FIRE
FIRE_CLASS_LABEL = 0

# Optional location lookup
TTN_LOCATIONS_ENABLED = True
TTN_LOCATIONS_TTL_SECONDS = 900

LOG_LEVEL = "INFO"


logging.basicConfig(
    level=LOG_LEVEL.upper(),
    format="%(asctime)s %(levelname)s %(message)s",
)
LOGGER = logging.getLogger("wildfire-bridge")

@dataclass(frozen=True)
class Config:
    ttn_broker: str
    ttn_port: int
    ttn_username: str
    ttn_api_key: str
    ttn_app_id: str
    mqtt_topic: str

    influx_url: str
    influx_token: str
    influx_org: str
    influx_bucket: str

    model_path: Path
    fire_class_label: int

    ttn_locations_enabled: bool
    ttn_locations_ttl_seconds: int

def load_config() -> Config:
    return Config(
        ttn_broker=TTN_BROKER,
        ttn_port=TTN_PORT,
        ttn_username=TTN_USERNAME,
        ttn_api_key=TTN_API_KEY,
        ttn_app_id=TTN_APP_ID,
        mqtt_topic=TTN_MQTT_TOPIC,
        influx_url=INFLUX_URL,
        influx_token=INFLUX_TOKEN,
        influx_org=INFLUX_ORG,
        influx_bucket=INFLUX_BUCKET,
        model_path=Path(MODEL_PATH),
        fire_class_label=FIRE_CLASS_LABEL,
        ttn_locations_enabled=TTN_LOCATIONS_ENABLED,
        ttn_locations_ttl_seconds=TTN_LOCATIONS_TTL_SECONDS,
    )

class LocationResolver:
    def __init__(self, config: Config):
        self.config = config
        self.cache: Dict[str, Tuple[float, Tuple[Optional[float], Optional[float]]]] = {}

    def resolve(self, payload: Dict[str, Any]) -> Tuple[Optional[float], Optional[float]]:
        device_id = payload.get("end_device_ids", {}).get("device_id")
        if not device_id:
            return None, None

        cached = self.cache.get(device_id)
        now = time.time()

        if cached and now - cached[0] < self.config.ttn_locations_ttl_seconds:
            return cached[1]

        location = self._extract_from_uplink(payload)
        if location == (None, None) and self.config.ttn_locations_enabled:
            location = self._fetch_from_registry(device_id)

        self.cache[device_id] = (now, location)
        return location


    def _extract_from_uplink(self, payload: Dict[str, Any]) -> Tuple[Optional[float], Optional[float]]:
        uplink = payload.get("uplink_message", {})

        # Prefer the end-device location set in The Things Stack
        uplink_locations = uplink.get("locations", {})
        if isinstance(uplink_locations, dict):
            user_location = uplink_locations.get("user")
            if isinstance(user_location, dict):
                latitude = user_location.get("latitude")
                longitude = user_location.get("longitude")
                if latitude is not None and longitude is not None:
                    return float(latitude), float(longitude)

            # Fallback: any other end-device location inside uplink_message.locations
            for candidate in uplink_locations.values():
                if not isinstance(candidate, dict):
                    continue

                latitude = candidate.get("latitude")
                longitude = candidate.get("longitude")
                if latitude is not None and longitude is not None:
                    return float(latitude), float(longitude)

        return None, None

    def _fetch_from_registry(self, device_id: str) -> Tuple[Optional[float], Optional[float]]:
        url = f"https://{self.config.ttn_broker}/api/v3/as/applications/{self.config.ttn_app_id}/devices/{device_id}"
        request = Request(
            url,
            headers={
                "Authorization": f"Bearer {self.config.ttn_api_key}",
                "Accept": "application/json",
            },
        )

        try:
            with urlopen(request, timeout=10) as response:
                body = json.loads(response.read().decode("utf-8"))
        except HTTPError as exc:
            LOGGER.warning("Location lookup failed for %s: HTTP %s", device_id, exc.code)
            return None, None
        except URLError as exc:
            LOGGER.warning("Location lookup failed for %s: %s", device_id, exc.reason)
            return None, None
        except Exception as exc:
            LOGGER.warning("Location lookup failed for %s: %s", device_id, exc)
            return None, None

        locations = body.get("locations", {})
        if not isinstance(locations, dict):
            return None, None

        for candidate in locations.values():
            latitude = candidate.get("latitude")
            longitude = candidate.get("longitude")
            if latitude is not None and longitude is not None:
                return float(latitude), float(longitude)

        return None, None

class FireModel:

    DEFAULT_FEATURES = [
        "Temperature",
        "Humidity",
        "Pressure",
        "Gas_0",
        "Gas_1",
        "Gas_2",
        "Gas_3",
        "Gas_4",
        "Gas_5",
        "Gas_6",
        "Gas_7",
        "Gas_8",
        "Gas_9",
    ]

    def __init__(self, model_path: Path, fire_class_label: int):
        self.model = joblib.load(model_path)
        self.fire_class_label = fire_class_label

        self.feature_names = list(getattr(self.model, "feature_names_in_", []))
        if not self.feature_names:
            self.feature_names = self.DEFAULT_FEATURES.copy()

        self.classes_ = list(getattr(self.model, "classes_", []))

        LOGGER.info("Loaded model from %s", model_path)
        LOGGER.info("Model feature names: %s", self.feature_names)
        LOGGER.info("Model classes: %s", self.classes_)

    def build_frame(self, decoded: Dict[str, Any]) -> pd.DataFrame:
        row = {
            "Temperature": float(decoded.get("temperature", 0.0) or 0.0),
            "Humidity": float(decoded.get("humidity", 0.0) or 0.0),
            "Pressure": float(decoded.get("pressure", 0.0) or 0.0),
            "Gas_0": float(decoded.get("gas_0", 0.0) or 0.0),
            "Gas_1": float(decoded.get("gas_1", 0.0) or 0.0),
            "Gas_2": float(decoded.get("gas_2", 0.0) or 0.0),
            "Gas_3": float(decoded.get("gas_3", 0.0) or 0.0),
            "Gas_4": float(decoded.get("gas_4", 0.0) or 0.0),
            "Gas_5": float(decoded.get("gas_5", 0.0) or 0.0),
            "Gas_6": float(decoded.get("gas_6", 0.0) or 0.0),
            "Gas_7": float(decoded.get("gas_7", 0.0) or 0.0),
            "Gas_8": float(decoded.get("gas_8", 0.0) or 0.0),
            "Gas_9": float(decoded.get("gas_9", 0.0) or 0.0),
        }

        return pd.DataFrame([row], columns=self.feature_names)

    def predict(self, decoded: Dict[str, Any]) -> Tuple[float, Optional[int]]:
        frame = self.build_frame(decoded)

        predicted_label: Optional[int] = None
        if hasattr(self.model, "predict"):
            raw_pred = self.model.predict(frame)[0]
            try:
                predicted_label = int(raw_pred)
            except (TypeError, ValueError):
                predicted_label = None

        p_fire = self._predict_fire_probability(frame, predicted_label)
        return p_fire, predicted_label

    def _predict_fire_probability(self, frame: pd.DataFrame, predicted_label: Optional[int]) -> float:
        if hasattr(self.model, "predict_proba"):
            probabilities = self.model.predict_proba(frame)[0]

            if self.classes_:
                if self.fire_class_label not in self.classes_:
                    raise RuntimeError(
                        f"fire_class_label={self.fire_class_label} not found in model.classes_={self.classes_}"
                    )
                fire_index = self.classes_.index(self.fire_class_label)
                return float(probabilities[fire_index])

            raise RuntimeError("Model has predict_proba but no classes_ attribute")

        if predicted_label is None:
            return 0.0

        return 1.0 if predicted_label == self.fire_class_label else 0.0

class WildfireBridge:
    def __init__(self, config: Config):
        self.config = config
        self.model = FireModel(config.model_path, config.fire_class_label)
        self.location_resolver = LocationResolver(config)

        self.influx_client = InfluxDBClient(
            url=config.influx_url,
            token=config.influx_token,
            org=config.influx_org,
        )
        self.write_api = self.influx_client.write_api(write_options=SYNCHRONOUS)

        self.mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
        self.mqtt_client.username_pw_set(config.ttn_username, config.ttn_api_key)
        self.mqtt_client.on_connect = self.on_connect
        self.mqtt_client.on_disconnect = self.on_disconnect
        self.mqtt_client.on_message = self.on_message

    def start(self) -> None:
        LOGGER.info("Connecting to MQTT broker %s:%s", self.config.ttn_broker, self.config.ttn_port)
        self.mqtt_client.connect(self.config.ttn_broker, self.config.ttn_port, 60)
        self.mqtt_client.loop_forever()

    def stop(self) -> None:
        try:
            self.mqtt_client.disconnect()
        finally:
            self.write_api.flush()
            self.influx_client.close()

    def on_connect(self, client: mqtt.Client, userdata: Any, flags: Dict[str, Any], rc: int) -> None:
        if rc != 0:
            LOGGER.error("MQTT connect failed with code %s", rc)
            return

        LOGGER.info("Connected to TTN MQTT broker")
        client.subscribe(self.config.mqtt_topic)
        LOGGER.info("Subscribed to %s", self.config.mqtt_topic)

    def on_disconnect(self, client: mqtt.Client, userdata: Any, rc: int) -> None:
        if rc != 0:
            LOGGER.warning("Unexpected MQTT disconnect: %s", rc)

    def on_message(self, client: mqtt.Client, userdata: Any, msg: mqtt.MQTTMessage) -> None:
        try:
            payload_json = json.loads(msg.payload.decode("utf-8"))
        except Exception:
            LOGGER.exception("Failed to parse MQTT JSON on topic %s", msg.topic)
            return

        try:
            uplink = payload_json.get("uplink_message")
            if not isinstance(uplink, dict):
                LOGGER.warning("Skipping message without uplink_message on topic %s", msg.topic)
                return

            decoded = uplink.get("decoded_payload")
            if not isinstance(decoded, dict):
                LOGGER.warning(
                    "Skipping message without decoded_payload device=%s f_port=%s frm_payload=%s",
                    payload_json.get("end_device_ids", {}).get("device_id"),
                    uplink.get("f_port"),
                    uplink.get("frm_payload"),
                )
                return

            best_gateway = max(
                uplink.get("rx_metadata", []),
                key=lambda metadata: metadata.get("rssi", -1000),
                default={},
            )
            rssi = best_gateway.get("rssi")
            snr = best_gateway.get("snr")
            latitude, longitude = self.location_resolver.resolve(payload_json)

            p_fire, predicted_label = self.model.predict(decoded)

            self._write_point(
                payload_json=payload_json,
                decoded=decoded,
                rssi=rssi,
                snr=snr,
                latitude=latitude,
                longitude=longitude,
                p_fire=p_fire,
                predicted_label=predicted_label,
            )

            LOGGER.info(
                "Stored uplink device=%s seq=%s p_fire=%.4f pred=%s rssi=%s snr=%s lat=%s lon=%s",
                payload_json.get("end_device_ids", {}).get("device_id"),
                decoded.get("seqNum"),
                p_fire,
                predicted_label,
                rssi,
                snr,
                latitude,
                longitude,
            )
        except Exception:
            LOGGER.exception("Error processing message on topic %s", msg.topic)

    def _write_point(
        self,
        payload_json: Dict[str, Any],
        decoded: Dict[str, Any],
        rssi: Optional[float],
        snr: Optional[float],
        latitude: Optional[float],
        longitude: Optional[float],
        p_fire: float,
        predicted_label: Optional[int],
    ) -> None:
        received_at = payload_json.get("received_at")
        device_id = payload_json.get("end_device_ids", {}).get("device_id", "unknown")

        point = (
            Point("environment_metrics")
            .tag("device_id", str(device_id))
            .field("NodeID", float(decoded.get("nodeID", 0.0) or 0.0))
            .field("SeqNum", float(decoded.get("seqNum", 0.0) or 0.0))
            .field("Temperature", float(decoded.get("temperature", 0.0) or 0.0))
            .field("Humidity", float(decoded.get("humidity", 0.0) or 0.0))
            .field("Pressure", float(decoded.get("pressure", 0.0) or 0.0))
            .field("Battery", float(decoded.get("battery", 0.0) or 0.0))
            .field("fire_probability", float(p_fire))
        )

        if predicted_label is not None:
            point = point.field("predicted_label", int(predicted_label))
            point = point.field(
                "predicted_fire",
                1 if int(predicted_label) == self.config.fire_class_label else 0
            )

        if rssi is not None:
            point = point.field("RSSI", float(rssi))
        if snr is not None:
            point = point.field("SNR", float(snr))
        if latitude is not None:
            point = point.field("Latitude", float(latitude))
        if longitude is not None:
            point = point.field("Longitude", float(longitude))

        for index in range(10):
            point = point.field(f"Gas_{index}", float(decoded.get(f"gas_{index}", 0.0) or 0.0))

        if received_at:
            point = point.time(received_at)

        self.write_api.write(
            bucket=self.config.influx_bucket,
            org=self.config.influx_org,
            record=point,
        )

def main() -> int:
    config = load_config()

    if not config.model_path.exists():
        LOGGER.error("Model file not found: %s", config.model_path)
        return 1

    bridge = WildfireBridge(config)

    def handle_signal(signum: int, frame: Any) -> None:
        LOGGER.info("Received signal %s, shutting down", signum)
        bridge.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    try:
        bridge.start()
    except KeyboardInterrupt:
        LOGGER.info("Interrupted by user")
    finally:
        bridge.stop()

    return 0

if __name__ == "__main__":
    raise SystemExit(main())