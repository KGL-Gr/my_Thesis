#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <cmath>
#include <WiFi.h>
#include "esp_wifi.h"
#include <esp_bt.h>
#include "bme68xLibrary.h"

#include <driver/adc.h>
#include <esp_adc_cal.h>

// Should be configured before installation. Alonia 170.0 m, Thessaloni 40.0 m
const float ALTITUDE_S = 40.0; 

const adc1_channel_t ADC_CHANNEL = ADC1_CHANNEL_1;      
const adc_atten_t ADC_ATTEN = ADC_ATTEN_DB_11;
const adc_bits_width_t ADC_BITS = ADC_WIDTH_BIT_12;

const float VOLTAGE_MULTIPLIER = 4.0304;
const float BATTERY_EMPTY_VOLTAGE = 3.35f;  // device cutoff = 0%
const float BATTERY_FULL_VOLTAGE  = 4.25f;  // Battery full charge = 100%

esp_adc_cal_characteristics_t adc_chars;

const unsigned TX_INTERVAL_SECONDS = 500; // Profile will last ~10.8s, so the tx will comence in >510s.
const int PAYLOAD_SIZE = 33;

#define uS_TO_S_FACTOR 1000000ULL

// TX watchdog: if LMIC gets stuck after queuing, sleep and retry next cycle
const unsigned long TX_WATCHDOG_MS = 180000UL;

// LMIC wants LSB first
static const u1_t PROGMEM APPEUI[8] = {
};
void os_getArtEui(u1_t* buf) { memcpy_P(buf, APPEUI, 8); }

// LMIC wants LSB first
static const u1_t PROGMEM DEVEUI[8] = {
};
void os_getDevEui(u1_t* buf) { memcpy_P(buf, DEVEUI, 8); }

// LMIC wants MSB first
static const u1_t PROGMEM APPKEY[16] = {
};
void os_getDevKey(u1_t* buf) { memcpy_P(buf, APPKEY, 16); }

const lmic_pinmap lmic_pins = {
  .nss = 7,
  .rxtx = LMIC_UNUSED_PIN,
  .rst = 0,
  .dio = {2, 3, LMIC_UNUSED_PIN},
};

const uint32_t RTC_MAGIC_VALUE = 0xED10BEEF;

RTC_DATA_ATTR uint32_t RTC_MAGIC = 0;
RTC_DATA_ATTR bool RTC_SESSION_VALID = false;

RTC_DATA_ATTR u4_t RTC_NETID = 0;
RTC_DATA_ATTR devaddr_t RTC_DEVADDR = 0;
RTC_DATA_ATTR u1_t RTC_NWKSKEY[16];
RTC_DATA_ATTR u1_t RTC_APPSKEY[16];

RTC_DATA_ATTR u4_t RTC_FCNT_UP = 0;
RTC_DATA_ATTR u4_t RTC_FCNT_DOWN = 0;
RTC_DATA_ATTR uint8_t RTC_RX_DELAY = 5;

RTC_DATA_ATTR uint32_t RTC_APP_SEQNUM = 0;

static osjob_t sendjob;
static uint8_t txPacket[PAYLOAD_SIZE];

volatile bool txComplete = false;
volatile bool txStarted = false;

bool packetQueued = false;
unsigned long packetQueuedAtMs = 0;

uint8_t nodeID = 0;
uint32_t seqNum = 0;
uint8_t battery_state = 0;

#define NEW_GAS_MEAS (BME68X_GASM_VALID_MSK | BME68X_HEAT_STAB_MSK | BME68X_NEW_DATA_MSK)

const uint8_t PIN_CS = 8;
const uint8_t Battery_Pin = 1;

Bme68x bme;

struct CycleData {
  float temp;
  float humi;
  float press;
  float gas[10];
  bool gasValid[10];
};

CycleData cycleData;

void do_send(osjob_t* j);
void goToDeepSleep();

float adc_to_voltage(uint16_t adc_reading) {
  uint32_t voltage_mv = esp_adc_cal_raw_to_voltage(adc_reading, &adc_chars);
  return voltage_mv / 1000.0f;
}

float get_battery_percent(float voltage) {
  if (voltage <= BATTERY_EMPTY_VOLTAGE) return 0.0f;
  if (voltage >= BATTERY_FULL_VOLTAGE)  return 100.0f;

  return ((voltage - BATTERY_EMPTY_VOLTAGE) / (BATTERY_FULL_VOLTAGE - BATTERY_EMPTY_VOLTAGE)) * 100.0f;
}

void get_battery_voltage() {
  adc1_config_width(ADC_BITS);
  adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN);

  esp_adc_cal_characterize(
    ADC_UNIT_1,
    ADC_ATTEN,
    ADC_BITS,
    1100,
    &adc_chars
  );

  uint32_t raw = 0;

  for (int i = 0; i < 4; i++) {
    raw += adc1_get_raw(ADC_CHANNEL);
    delayMicroseconds(10);
  }

  raw /= 4;

  float battery_v = adc_to_voltage(raw) * VOLTAGE_MULTIPLIER;
  battery_state = (uint8_t)round(get_battery_percent(battery_v));

  Serial.print(F("Battery voltage: "));
  Serial.print(battery_v, 3);
  Serial.print(F(" V, "));
  Serial.print(battery_state);
  Serial.println(F("%"));
}

void resetCycleData() {
  cycleData.temp = NAN;
  cycleData.humi = NAN;
  cycleData.press = NAN;

  for (int i = 0; i < 10; i++) {
    cycleData.gas[i] = NAN;
    cycleData.gasValid[i] = false;
  }
}

bool allGasValuesCollected() {
  for (int i = 0; i < 10; i++) {
    if (!cycleData.gasValid[i]) return false;
  }
  return true;
}

bool takeAmbientTPH() {
  bme68xData data;
  uint8_t nFieldsLeft = 0;

  bme.setOpMode(BME68X_SLEEP_MODE);
  delay(50);

  bme.setOpMode(BME68X_FORCED_MODE);
  delay(200);

  if (bme.fetchData()) {
    do {
      nFieldsLeft = bme.getData(data);

      cycleData.temp = data.temperature;
      cycleData.humi = data.humidity;
      cycleData.press = ((data.pressure/100.0)/(pow(1.0-(ALTITUDE_S/44330.0), 5.255)))*100.0;

      Serial.print(F("Ambient T/H/P captured: "));
      Serial.print(cycleData.temp);
      Serial.print(F(" C, "));
      Serial.print(cycleData.humi);
      Serial.print(F(" %, "));
      Serial.print(cycleData.press);
      Serial.println(F(" Pa"));

      return true;

    } while (nFieldsLeft);
  }

  Serial.println(F("Failed to capture ambient T/H/P."));
  return false;
}

void takeGasMeasurements() {
  uint16_t tempProf[10] = {
    320, 100, 100, 100, 200,
    200, 200, 320, 320, 320
  };

  uint16_t durProf[10] = {
    700, 280, 1400, 4200, 700,
    700, 700, 700, 700, 700
  };

  uint32_t profileDurationMs = 0;

  for (int i = 0; i < 10; i++) {
    profileDurationMs += durProf[i];
  }

  bme.setOpMode(BME68X_SLEEP_MODE);
  delay(10);

  bme.setSeqSleep(BME68X_ODR_0_59_MS);
  bme.setHeaterProf(tempProf, durProf, 10);
  bme.setOpMode(BME68X_SEQUENTIAL_MODE);

  Serial.println(F("Gas profile started."));

  bme68xData data;
  uint8_t nFieldsLeft = 0;

  unsigned long startMs = millis();
  unsigned long timeoutMs = profileDurationMs + 1500;

  while ((millis() - startMs) < timeoutMs) {
    if (bme.fetchData()) {
      do {
        nFieldsLeft = bme.getData(data);

        Serial.print(F("t+"));
        Serial.print(millis() - startMs);
        Serial.print(F(" ms, Gas index "));
        Serial.print(data.gas_index);
        Serial.print(F(", status 0x"));
        Serial.print(data.status, HEX);
        Serial.print(F(", gas "));
        Serial.println(data.gas_resistance);

        if ((data.status & NEW_GAS_MEAS) == NEW_GAS_MEAS) {
          if (data.gas_index < 10 && !cycleData.gasValid[data.gas_index]) {
            cycleData.gas[data.gas_index] = data.gas_resistance;
            cycleData.gasValid[data.gas_index] = true;

            Serial.print(F("Captured Gas_"));
            Serial.print(data.gas_index);
            Serial.print(F(" = "));
            Serial.println(data.gas_resistance);
          }

          if (allGasValuesCollected()) {
            Serial.println(F("All 10 gas values collected."));
            bme.setOpMode(BME68X_SLEEP_MODE);
            return;
          }
        }

      } while (nFieldsLeft);
    }

    delay(10);
  }

  Serial.println(F("Gas profile timeout. Stopping BME688."));

  int count = 0;
  for (int i = 0; i < 10; i++) {
    if (cycleData.gasValid[i]) count++;
  }

  Serial.print(F("Gas values collected: "));
  Serial.print(count);
  Serial.println(F("/10"));

  bme.setOpMode(BME68X_SLEEP_MODE);
}

void pack_bits(uint8_t* buffer, uint16_t bitPos, uint32_t value, uint8_t bits) {
  for (uint8_t i = 0; i < bits; i++) {
    uint16_t byteIndex = (bitPos + i) / 8;
    uint8_t bitIndex = 7 - ((bitPos + i) % 8);
    uint8_t bit = (value >> (bits - 1 - i)) & 0x01;

    if (bit) {
      buffer[byteIndex] |= (1 << bitIndex);
    } else {
      buffer[byteIndex] &= ~(1 << bitIndex);
    }
  }
}

void buildTxPacket() {
  memset(txPacket, 0, PAYLOAD_SIZE);

  uint16_t bitPos = 0;

  pack_bits(txPacket, bitPos, nodeID, 8);
  bitPos += 8;

  pack_bits(txPacket, bitPos, seqNum & 0xFFFFF, 20);
  bitPos += 20;

  int16_t temp_int = 0;
  if (!isnan(cycleData.temp)) {
    temp_int = (int16_t)round(cycleData.temp * 100.0f);
  }

  pack_bits(txPacket, bitPos, (uint16_t)temp_int, 16);
  bitPos += 16;

  uint16_t humi_int = 0;
  if (!isnan(cycleData.humi)) {
    humi_int = (uint16_t)round(cycleData.humi * 100.0f);
  }

  pack_bits(txPacket, bitPos, humi_int, 16);
  bitPos += 16;

  uint32_t press_int = 0;
  if (!isnan(cycleData.press)) {
    press_int = (uint32_t)round(cycleData.press * 100.0f);
  }

  if (press_int > 0xFFFFFF) {
    press_int = 0xFFFFFF;
  }

  pack_bits(txPacket, bitPos, press_int, 24);
  bitPos += 24;

  for (int i = 0; i < 10; i++) {
    uint32_t gas_int = 0;

    if (cycleData.gasValid[i] && !isnan(cycleData.gas[i])) {
      float scaled = cycleData.gas[i] / 1000.0f;
      if (scaled < 0) scaled = 0;

      gas_int = (uint32_t)round(scaled);

      if (gas_int > 0x1FFFF) {
        gas_int = 0x1FFFF;
      }
    }

    pack_bits(txPacket, bitPos, gas_int, 17);
    bitPos += 17;
  }

  pack_bits(txPacket, bitPos, battery_state, 8);
  bitPos += 8;

  pack_bits(txPacket, bitPos, 0, 2);
  bitPos += 2;
}

void configureLmicCommon() {
  LMIC_setClockError(MAX_CLOCK_ERROR * 5 / 100);
  LMIC_setLinkCheckMode(0);
  LMIC_setAdrMode(0);
  LMIC_setDrTxpow(DR_SF8, 12);
}

void saveJoinedSessionFromLmic() {
  u4_t netid = 0;
  devaddr_t devaddr = 0;
  u1_t nwkKey[16];
  u1_t artKey[16];

  LMIC_getSessionKeys(&netid, &devaddr, nwkKey, artKey);

  RTC_MAGIC = RTC_MAGIC_VALUE;
  RTC_SESSION_VALID = true;

  RTC_NETID = netid;
  RTC_DEVADDR = devaddr;

  memcpy(RTC_NWKSKEY, nwkKey, 16);
  memcpy(RTC_APPSKEY, artKey, 16);

  RTC_FCNT_UP = LMIC.seqnoUp;
  RTC_FCNT_DOWN = LMIC.seqnoDn;
  RTC_RX_DELAY = LMIC.rxDelay;

  RTC_APP_SEQNUM = seqNum;

  Serial.println(F("LoRaWAN session stored in RTC :)"));
  Serial.print(F("RTC DevAddr: "));   Serial.println(RTC_DEVADDR, HEX);
  Serial.print(F("RTC FCntUp: "));    Serial.println(RTC_FCNT_UP);
}

void saveSessionBeforeSleep() {
  RTC_MAGIC = RTC_MAGIC_VALUE;
  RTC_APP_SEQNUM = seqNum;

  if (RTC_SESSION_VALID) {
    RTC_FCNT_UP = LMIC.seqnoUp;
    RTC_FCNT_DOWN = LMIC.seqnoDn;
    RTC_RX_DELAY = LMIC.rxDelay;

    Serial.print(F("RTC session updated before sleep. FCntUp=")); Serial.println(RTC_FCNT_UP);
  } else {
    Serial.println(F("There is no valid session saved before sleep :("));
  }
}

bool restoreSessionFromRtc() {
  seqNum = RTC_APP_SEQNUM;

  Serial.print(F("RTC_MAGIC: 0x"));   Serial.println(RTC_MAGIC, HEX);

  if (RTC_MAGIC != RTC_MAGIC_VALUE || !RTC_SESSION_VALID) {
    Serial.println(F("No saved LoRaWAN session. OTAA join required."));
    return false;
  }

  LMIC_setSession(RTC_NETID, RTC_DEVADDR, RTC_NWKSKEY, RTC_APPSKEY);

  LMIC.seqnoUp = RTC_FCNT_UP;
  LMIC.seqnoDn = RTC_FCNT_DOWN;
  LMIC.rxDelay = RTC_RX_DELAY;

  configureLmicCommon();

  Serial.println(F("LoRaWAN session restored from RTC."));
  Serial.print(F("Restored DevAddr: "));      Serial.println(RTC_DEVADDR, HEX);
  Serial.print(F("Restored FCntUp: "));       Serial.println(LMIC.seqnoUp);
  Serial.print(F("Restored app seqNum: "));   Serial.println(seqNum);

  return true;
}

void clearRtcSession() {
  RTC_MAGIC = 0;
  RTC_SESSION_VALID = false;
  RTC_NETID = 0;
  RTC_DEVADDR = 0;
  RTC_FCNT_UP = 0;
  RTC_FCNT_DOWN = 0;
  RTC_RX_DELAY = 5;
  RTC_APP_SEQNUM = seqNum;

  memset(RTC_NWKSKEY, 0, sizeof(RTC_NWKSKEY));
  memset(RTC_APPSKEY, 0, sizeof(RTC_APPSKEY));

  Serial.println(F("RTC LoRaWAN session cleared."));
}

void goToDeepSleep() {
  saveSessionBeforeSleep();

  Serial.print(F("Entering deep sleep for "));  Serial.print(TX_INTERVAL_SECONDS);
  Serial.println(F(" seconds."));

  bme.setOpMode(BME68X_SLEEP_MODE);

  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  esp_bt_controller_disable();

  Serial.flush();

  esp_sleep_enable_timer_wakeup(
    (uint64_t)TX_INTERVAL_SECONDS * uS_TO_S_FACTOR
  );

  esp_deep_sleep_start();
}

void onEvent(ev_t ev) {
  Serial.print(os_getTime());
  Serial.print(F(": "));

  switch (ev) {
    case EV_SCAN_TIMEOUT:
      Serial.println(F("EV_SCAN_TIMEOUT"));
      break;

    case EV_BEACON_FOUND:
      Serial.println(F("EV_BEACON_FOUND"));
      break;

    case EV_BEACON_MISSED:
      Serial.println(F("EV_BEACON_MISSED"));
      break;

    case EV_BEACON_TRACKED:
      Serial.println(F("EV_BEACON_TRACKED"));
      break;

    case EV_JOINING:
      Serial.println(F("EV_JOINING"));
      break;

    case EV_JOINED: {
      Serial.println(F("EV_JOINED"));

      u4_t netid = 0;
      devaddr_t devaddr = 0;
      u1_t nwkKey[16];
      u1_t artKey[16];

      LMIC_getSessionKeys(&netid, &devaddr, nwkKey, artKey);

      Serial.print(F("netid: "));
      Serial.println(netid, DEC);

      Serial.print(F("devaddr: "));
      Serial.println(devaddr, HEX);

      configureLmicCommon();

      saveJoinedSessionFromLmic();

      break;
    }

    case EV_JOIN_FAILED:
      Serial.println(F("EV_JOIN_FAILED"));
      Serial.print(F(" :("));
      clearRtcSession();
      goToDeepSleep();
      break;

    case EV_REJOIN_FAILED:
      Serial.println(F("EV_REJOIN_FAILED"));
      Serial.print(F(" :("));
      clearRtcSession();
      goToDeepSleep();
      break;

    case EV_TXSTART:
      txStarted = true;
      Serial.println(F("EV_TXSTART!"));
      Serial.print(F(" :)"));
      break;

    case EV_JOIN_TXCOMPLETE:
      Serial.println(F("EV_JOIN_TXCOMPLETE!"));
      break;

    case EV_TXCOMPLETE:
      Serial.println(F("EV_TXCOMPLETE, including RX windows."));

      if (LMIC.txrxFlags & TXRX_ACK) {
        Serial.println(F("Received ACK!"));
      }
      Serial.print(F("LMIC FCntUp after TX: "));
      Serial.println(LMIC.seqnoUp);

      txComplete = true;
      packetQueued = false;
      break;

    case EV_LOST_TSYNC:
      Serial.println(F("EV_LOST_TSYNC"));
      break;

    case EV_RESET:
      Serial.println(F("EV_RESET"));
      break;

    case EV_RXCOMPLETE:
      Serial.println(F("EV_RXCOMPLETE"));
      break;

    case EV_LINK_DEAD:
      Serial.println(F("EV_LINK_DEAD x_x"));
      break;

    case EV_LINK_ALIVE:
      Serial.println(F("EV_LINK_ALIVE"));
      break;

    default:
      Serial.print(F("Unknown event: ")); Serial.println((unsigned)ev);
      Serial.print(F(" :O"));
      break;
  }
}

void do_send(osjob_t* j) {
  Serial.print(F("Before send: opmode=0x"));  Serial.print(LMIC.opmode, HEX);
  Serial.print(F(" os_getTime="));            Serial.print(os_getTime());
  Serial.print(F(" LMIC.seqnoUp="));          Serial.println(LMIC.seqnoUp);

  if (LMIC.opmode & OP_TXRXPEND) {
    Serial.println(F("OP_TXRXPEND, not sending."));
    return;
  }

  seqNum++;
  buildTxPacket();
  LMIC_setTxData2(1, txPacket, 33, 0);

  packetQueued = true;
  txStarted = false;
}

void setup() {
  Serial.begin(115200);
  delay(10);

  Serial.println(F("\n=== System Booting ==="));

  Serial.print(F("Wake cause: "));  Serial.println(esp_sleep_get_wakeup_cause());

  Serial.print(F("\nStarting ED")); Serial.print(nodeID);
  Serial.print(F(" node.."));

  // Disable the onboard network stack
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  esp_bt_controller_disable();

  // De-select SPI devices
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);

  pinMode(lmic_pins.nss, OUTPUT);
  digitalWrite(lmic_pins.nss, HIGH);

  SPI.begin(5, 4, 6, -1);
  Serial.println(F("SPI lines OK"));

  os_init();
  Serial.println(F("LMIC os_init OK"));

  LMIC_reset();
  Serial.println(F("LMIC reset OK"));

  configureLmicCommon();
  bool restored = restoreSessionFromRtc();

  if (restored) {
    Serial.println(F("Using restored session."));
  } else {
    Serial.println(F("Using OTAA join."));
  }

  bme.begin(PIN_CS, SPI);

  if (bme.checkStatus()) {  
    if (bme.checkStatus() == BME68X_ERROR) {                  // Restart
      Serial.println("Sensor error: " + bme.statusString());
      return;
    } else if (bme.checkStatus() == BME68X_WARNING) {
      Serial.println("Sensor warning: " + bme.statusString());// Proceed
    }
  }

  bme.setTPH();

  resetCycleData();

  Serial.println(F("BME init complete."));

  if (takeAmbientTPH()) {
    Serial.println(F("Ambient TPH done."));
  }

  takeGasMeasurements();

  get_battery_voltage();

  do_send(&sendjob);
}

void loop() {
  os_runloop_once();

  if (txComplete) {
    txComplete = false;
    resetCycleData();
    goToDeepSleep();
  }

  if (packetQueued && (millis() - packetQueuedAtMs > TX_WATCHDOG_MS)) {
    Serial.println(F("TX watchdog timeout."));
    Serial.print(F("txStarted="));
    Serial.println(txStarted ? F("true") : F("false"));
    Serial.print(F("LMIC opmode=0x"));
    Serial.println(LMIC.opmode, HEX);
    Serial.print(F("LMIC seqnoUp="));
    Serial.println(LMIC.seqnoUp);

    // Next wake will retry with the saved session.
    packetQueued = false;
    goToDeepSleep();
  }
}
