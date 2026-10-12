# my_Thesis

This repository contains the firmware, backend services, and experimental evaluation scripts for the diploma thesis titled **"Early detection of wildfires in remote forest areas using Multidimensional gas signatures and low-power communication"**. 

The project was conducted at the **Department of Information and Electronic Systems Engineering**, School of Engineering, **International Hellenic University (IHU)**, by **George Kounatidis**, under the supervision of Professor **Dr. Periklis Chatzimisios**

## System Overview

The system is an end-to-end, cost-effective, and energy-efficient Internet of Things (IoT) solution designed for the early detection of biomass burning in remote Mediterranean forest areas.
Rather than relying on simple static temperature thresholds, the architecture leverages **multidimensional gas signatures** (10 sequential heating steps of a Metal-Oxide / MOX sensor) combined with environmental telemetry and **Machine Learning** classifiers to minimize False Positives (FPs).

The system architecture is structured across three interconnected layers:
* **Sensing / Perception Layer:** Distributed, battery-powered sensor nodes deployed in hexagonal monitoring cells, collecting *in situ* environmental data and multidimensional gas signatures.
* **Network Layer:** Long-range, low-power wireless transmission using the **LoRaWAN** protocol (EU868 band) to an outdoor gateway, managed by **The Things Stack (TTS)**.
* **Application Layer:** Asynchronous ingestion via **MQTT** in Python, fire probability classification via trained ML models, time-series storage in **InfluxDB**, and real-time visualization and alerting via **Grafana**.

## Hardware Architecture & Custom PCB

The sensor node was engineered from the ground up for minimal quiescent current draw and resilience in outdoor forest conditions:
* **Microcontroller:** **Waveshare ESP32-C3 Zero** board (single-core 32-bit RISC-V SoC, 400 KB SRAM, 384 KB ROM, 3.3V logic). Onboard Wi-Fi and Bluetooth subsystems are explicitly disabled at boot to prevent unnecessary power draw.
* **Environmental & Gas Sensor:** **Adafruit BME688** over SPI. Measures ambient temperature, relative humidity, and barometric pressure in *forced mode*, followed by the **Standard Heating Profile 354** (10 sequential temperature steps across **100°C**, **200°C**, and **320°C**) to extract 10 gas-sensitive resistance values.
* **LoRa Transceiver:** **HopeRF RFM95W** (Semtech SX1276) mounted on an Elecrow breakout board with an SMA antenna connector, interfaced over the shared SPI bus.
* **Power Supply & Energy Management:** Single 18650 Li-ion cell (**Samsung SDI INR18650-35E**, 3.6V nominal, 3350 mAh capacity) regulated down to 3.3V by a low-dropout (LDO) linear regulator (**Microchip MCP1700**). The onboard LDO was bypassed to achieve an ultra-low quiescent current. Battery voltage is monitored via a high-impedance voltage divider (**1 MΩ** / **330 kΩ**) and an RC filter (**100 nF**, cut-off frequency **6.4 Hz**) connected to ADC1_CH1 (`GPIO1`).
* **Enclosure & Shielding:** Custom 2-layer PCB (**36.1 mm × 52.7 mm**, panelized layout) designed in **Altium Designer**. The BME688 is housed inside a custom ABS **Stevenson screen** to protect it from direct solar radiation and rain while maintaining natural ventilation.
* **LoRaWAN Gateway:** **RAK5146 PiHAT** (Semtech SX1303 baseband processor, 8 multi-SF LoRa channels, SMA 3 dBi antenna) coupled with a **Raspberry Pi 4 Model B** running RAKPiOS and UDP Packet Forwarder.

## Software, Network & Machine Learning

* **Node Firmware (`ED1_Final_Firmare_1_3b_pcb`):** Built in Arduino IDE using `Bosch-BME68x-Library` and `MCCI LoRaWAN LMIC`. The node authenticates via **OTAA** (AES-128 encryption), bit-packs sensor readings (`pack_bits()`), and transmits unconfirmed uplinks on **EU868** using **SF8**, **125 kHz** bandwidth, **4/5** coding rate, and **12 dBm** transmission power (Time on Air: **164.35 ms**). To eliminate energy-draining OTAA re-joins upon waking, session keys are persisted in **RTC memory** before entering deep sleep for **500 s**.
* **Backend Bridge (`python_bridge`):** Python daemon subscribing to The Things Stack broker via **Paho MQTT** (`v3/+/devices/+/up`). It decodes the JSON payload, retrieves geographical coordinates from the TTS Device Registry (cached for **900 s** to avoid unnecessary API requests), and invokes the classification pipeline.
* **Machine Learning Pipeline:** The 13-dimensional feature vector `[T, RH, P, Gas_0, Gas_1, ..., Gas_9]` is scaled using `StandardScaler` (z-score) and fed into binary classifiers (`0: FIRE`, `1: NO FIRE`). Evaluated models include **Support Vector Machines (SVM with RBF kernel)**, **AdaBoost**, and **XGBoost**. The probability of fire is retrieved via `predict_proba()`.
* **Storage & Visualization:** Formatted data records (environmental telemetry, fire probability, battery percentage, RSSI, SNR, and node coordinates) are stored in **InfluxDB**. The **Grafana** dashboard displays node status on an interactive map, tracks metrics over time, and triggers automated alerts (**Probable Wildfire** at **>67%** threshold, and **Low Battery** at **<20%**) via Email.
