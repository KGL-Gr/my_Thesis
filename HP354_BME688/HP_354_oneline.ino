/**
 * Copyright (C) 2021 Bosch Sensortec GmbH
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Arduino.h"
#include "bme68xLibrary.h"

#include <FS.h>
#include <LittleFS.h>

#define NEW_GAS_MEAS (BME68X_GASM_VALID_MSK | BME68X_HEAT_STAB_MSK | BME68X_NEW_DATA_MSK)
#define MEAS_DUR 140
#define PIN_CS 5

Bme68x bme;

bool isCooling = false;
unsigned long cooldownStartTime = 0;
uint32_t sampleCount = 0;

// Stores one full record
struct CycleData {
  uint32_t timeSec;
  uint32_t sample;
  float temp;
  float humi;
  float press;
  float gas[10];
  bool gasValid[10];
};

CycleData cycleData;

// Resets the process
void resetCycleData() {
  cycleData.timeSec = 0;
  cycleData.sample = 0;
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
    if (!cycleData.gasValid[i]) {
      return false;
    }
  }
  return true;
}

// Function to print data from the file
void readFile(const char *path) {

  File file = LittleFS.open(path, "r");
  if (!file) {
    Serial.println("Failed to open file for reading :'(");
    return;
  }

  Serial.print("Read from file: ");
  while (file.available()) {
    Serial.write(file.read());
  }
  file.close();
}

void writeFile(const char *path, const char *message) {

  File file = LittleFS.open(path, "w");
  if (!file) {
    Serial.println("Failed to open file for writing :'(");
    return;
  }

  if (file.print(message)) {
    Serial.println("File written");
  } else {
    Serial.println("Write failed :(");
  }

  delay(1000);
  file.close();
}

void appendFile(const char *path, const char *message) {

  File file = LittleFS.open(path, "a");
  if (!file) {
    Serial.println("Failed to open file for appending :'(");
    return;
  }
  if (!file.print(message)) {
    Serial.println("Append failed :(");
  }

  file.close();
}

void writeCycleToFile() {
  char line[320];

  snprintf(
    line,
    sizeof(line),
    "%lu,%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
    cycleData.timeSec,
    cycleData.sample,
    cycleData.temp,
    cycleData.humi,
    cycleData.press,
    cycleData.gas[0],
    cycleData.gas[1],
    cycleData.gas[2],
    cycleData.gas[3],
    cycleData.gas[4],
    cycleData.gas[5],
    cycleData.gas[6],
    cycleData.gas[7],
    cycleData.gas[8],
    cycleData.gas[9]
  );

  appendFile("/data.txt", line);
  Serial.println(line);
}

bool takeAmbientTPH() {
  bme68xData data;
  uint8_t nFieldsLeft = 0;

  // Put sensor to sleep first
  bme.setOpMode(BME68X_SLEEP_MODE);
  delay(50);

  bme.setOpMode(BME68X_FORCED_MODE);

  delay(200);

  if (bme.fetchData()) {
    do {
      nFieldsLeft = bme.getData(data);

      // TPH measurement before the heater profile starts
      cycleData.temp = data.temperature;
      cycleData.press = data.pressure;
      cycleData.humi = data.humidity;
      cycleData.timeSec = millis() / 1000;
      cycleData.sample = sampleCount;

      Serial.print("Ambient T/H/P captured: ");
      Serial.print(cycleData.temp); Serial.print(" C, ");
      Serial.print(cycleData.humi); Serial.print(" %, ");
      Serial.print(cycleData.press); Serial.println(" Pa");

      return true;

    } while (nFieldsLeft);
  }

  Serial.println("Failed to capture ambient TPH.");
  return false;
}

void startGasProfile() {
  uint16_t tempProf[10] = {320, 100, 100, 100, 200, 200, 200, 320, 320, 320};
  uint16_t mulProf[10]  = {5, 2, 10, 30, 5, 5, 5, 5, 5, 5};

  uint16_t sharedHeatrDur = MEAS_DUR - (bme.getMeasDur(BME68X_PARALLEL_MODE) / 1000);

  bme.setHeaterProf(tempProf, mulProf, sharedHeatrDur, 10);
  bme.setOpMode(BME68X_PARALLEL_MODE);

  Serial.println("Gas profile started.");
}

void setup(void) {
  SPI.begin();
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.begin(115200);
  Serial.flush();

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed :'(");
    return;
  } else {
    Serial.println("Little FS Done");
  }

  bool fileexists = LittleFS.exists("/data.txt");

  if (!fileexists) {
    Serial.println("File doesnt exist");
    Serial.println("Creating file...");
    writeFile(
      "/data.txt",
      "Time_sec,Sample,Temp,Humi,Pressure,Gas_0,Gas_1,Gas_2,Gas_3,Gas_4,Gas_5,Gas_6,Gas_7,Gas_8,Gas_9\n"
    );
  } else {
    Serial.println("File already exists");
  }

  readFile("/data.txt");

  bme.begin(PIN_CS, SPI);

  if (bme.checkStatus()) {
    if (bme.checkStatus() == BME68X_ERROR) {
      Serial.println("Sensor error:" + bme.statusString());

      for (int i = 0; i < 7; i++) {
        digitalWrite(LED_BUILTIN, HIGH); delay(330);
        digitalWrite(LED_BUILTIN, LOW);  delay(500);
      }
      return;
    } else if (bme.checkStatus() == BME68X_WARNING) {
      Serial.println("Sensor Warning:" + bme.statusString());
    }
  }

  bme.setTPH();

  resetCycleData();

  Serial.println("Ready.");
}

void loop() {
  // Cooldown state
  if (isCooling) {
    if (millis() - cooldownStartTime >= 60000) {
      Serial.println("Cooldown complete.");
      isCooling = false;
    }
    return;
  }

  // Start a new cycle only if none is in progress
  if (allGasValuesCollected() || isnan(cycleData.temp)) {
    resetCycleData();

    if (!takeAmbientTPH()) {
      delay(1000);
      return;
    }

    startGasProfile();
  }

  bme68xData data;
  uint8_t nFieldsLeft = 0;

  delay(MEAS_DUR);

  if (bme.fetchData()) {
    do {
      nFieldsLeft = bme.getData(data);

      if (data.status & NEW_GAS_MEAS) {
        if (data.gas_index < 10 && !cycleData.gasValid[data.gas_index]) {
          cycleData.gas[data.gas_index] = data.gas_resistance;
          cycleData.gasValid[data.gas_index] = true;

          Serial.print("Captured Gas_");
          Serial.print(data.gas_index);
          Serial.print(" = ");
          Serial.println(data.gas_resistance);
        }

        if (allGasValuesCollected()) {
          writeCycleToFile();

          sampleCount++;

          Serial.println("Entering cooldown...");
          isCooling = true;
          cooldownStartTime = millis();

          bme.setOpMode(BME68X_SLEEP_MODE);
          resetCycleData();
          break;
        }
      }

    } while (nFieldsLeft);
  }
}