/**
 * Copyright (C) 2021 Bosch Sensortec GmbH
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * 
 */

/*
 * The code runs on the NodeMCU 1.0 (ESP-12E Module)
 * which employs the BME688 sensor and takes measurements related
 * to combustion-derived emissions during pyrolysis/smoldering
 * conditions and baseline ambient "clean" air
*/

#include "Arduino.h"
#include "bme68xLibrary.h"

#include <FS.h>
#include <LittleFS.h>

#define NEW_GAS_MEAS (BME68X_GASM_VALID_MSK | BME68X_HEAT_STAB_MSK | BME68X_NEW_DATA_MSK)
#define MEAS_DUR 140

#define PIN_CS 5  // Note: PIN_CS 15 is the default
Bme68x bme;


bool isCooling = false;
unsigned long cooldownStartTime = 0;
// ver 3

void readFile(const char *path) {
	Serial.printf("Reading file: %s\n", path);

	File file = LittleFS.open(path, "r");
	if (!file) {
		Serial.println("Failed to open file for reading :'(");
		return;
	}

	Serial.print("Read from file: ");
	while (file.available()) { Serial.write(file.read()); }
	file.close();
}

void writeFile(const char *path, const char *message) {
	Serial.printf("Writing file: %s\n", path);

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
	if (file.print(message)) {
    // Serial.println("Message appended");
  } else {
    Serial.println("Append failed :(");
  }

	file.close();
}

void setup(void) {
  SPI.begin();
	pinMode(LED_BUILTIN, OUTPUT);

  Serial.begin(115200);
	Serial.flush();

	/* Little FS stuff */

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
		// Create File and add header
		writeFile("/data.txt", "BME688 measurements \r\n");
	} else {
		Serial.println("File already exists");
	}
	readFile("/data.txt");
	
	/* BME init stuff */
	bme.begin(PIN_CS, SPI);

	if (bme.checkStatus()) {
		if (bme.checkStatus() == BME68X_ERROR) {
			Serial.println("Sensor error:" + bme.statusString());

      /* Since the sensor has to run for at least 2-3 days to provide reliable data,
       * this routine performs a quick sanity check, to ensure wiring and setup are OK.
       * It is good to have, if you don't have another way of checking it.
       * Better safe than sorry :)
      */
			digitalWrite(LED_BUILTIN, HIGH); delay(330); digitalWrite(LED_BUILTIN, LOW); delay(500);
			digitalWrite(LED_BUILTIN, HIGH); delay(330); digitalWrite(LED_BUILTIN, LOW); delay(500);
			digitalWrite(LED_BUILTIN, HIGH); delay(330); digitalWrite(LED_BUILTIN, LOW); delay(500);
			digitalWrite(LED_BUILTIN, HIGH); delay(330); digitalWrite(LED_BUILTIN, LOW); delay(500);
			digitalWrite(LED_BUILTIN, HIGH); delay(330); digitalWrite(LED_BUILTIN, LOW); delay(500);
			digitalWrite(LED_BUILTIN, HIGH); delay(330); digitalWrite(LED_BUILTIN, LOW); delay(500);
			digitalWrite(LED_BUILTIN, HIGH); delay(330); digitalWrite(LED_BUILTIN, LOW); delay(500);
			
			return;
		} else if (bme.checkStatus() == BME68X_WARNING) {
			Serial.println("Sensor Warning:" + bme.statusString());
		}
	}
	
	/* The default configuration for temperature, pressure and humidity */
	bme.setTPH();

	/* Set up for the Heating profile 354 */
	uint16_t tempProf[10] = { 320, 100, 100, 100, 200, 200, 200, 320, 320, 320 };
	/* Multiplier to the shared heater duration */
	uint16_t mulProf[10] = { 5, 2, 10, 30, 5, 5, 5, 5, 5, 5 };
	/* Shared heating duration in milliseconds */
	uint16_t sharedHeatrDur = MEAS_DUR - (bme.getMeasDur(BME68X_PARALLEL_MODE) / 1000);

	bme.setHeaterProf(tempProf, mulProf, sharedHeatrDur, 10);
	bme.setOpMode(BME68X_PARALLEL_MODE);

	Serial.println("After setOpMode: " + bme.statusString());

	Serial.println("TimeStamp(ms), Temperature(deg C), Pressure(Pa), Humidity(%), Gas resistance(ohm), Status, Gas index");
	
}

void loop() {

  // ========= STATE 1: Cooling down =========
  if (isCooling) {
    // Check if 60 seconds have passed
    if (millis() - cooldownStartTime >= 60000) {
      Serial.println("Cooldown complete. Starting new profile..");
      isCooling = false;
      
      // Wake up the sensor
      bme.setOpMode(BME68X_PARALLEL_MODE);
      // Important: The sensor takes a moment to wake. 
    }
    // If not yet 60s, return. 
    // This keeps the ESP responsive.
    return; 
  }

  // ========= STATE 2: Measuring ========= 
  bme68xData data;
  uint8_t nFieldsLeft = 0;

  // Standard BME delay
  delay(MEAS_DUR); 

  if (bme.fetchData()) {
    do {
      nFieldsLeft = bme.getData(data);
      
      if (data.status & NEW_GAS_MEAS) {
        
        // --------- STEP 1. LOG DATA  ---------
        char logBuffer[128]; 
        snprintf(logBuffer, sizeof(logBuffer), "%lu,%d,%.2f,%.2f,%.2f,%.2f\n",
                 millis() / 1000,
                 data.gas_index,
                 data.gas_resistance,
                 data.temperature,
                 data.humidity,
                 data.pressure);
                 
        appendFile("/data.txt", logBuffer);
        Serial.print("Logged Index: "); Serial.println(data.gas_index);

        // --------- 2. CHECK FOR END OF PROFILE ---------
        if (data.gas_index == 9) {
          Serial.println("Index 9 detected. Entering Cooldown...");
          
          // A. Set flag
          isCooling = true;
          cooldownStartTime = millis();
          
          // B. Sleep the sensor
          bme.setOpMode(BME68X_SLEEP_MODE); 
          break; 
        }
      }
    } while (nFieldsLeft);
  }

	// ver 3
}
