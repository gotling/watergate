/**
 * Watergate - Automatic watering for growhouse
 *
 * FTDebouncer by Ubi de Feo
 * https://github.com/ubidefeo/FTDebouncer
 * DHT sensor library by Adafruit 
 * https://github.com/adafruit/DHT-sensor-library
 * DallasTemperature by Miles Burton
 * https://github.com/milesburton/Arduino-Temperature-Control-Library
 * OneWire by Jim Studt, ...
 * https://www.pjrc.com/teensy/td_libs_OneWire.html
 * WiFiManager by tzapu
 * https://github.com/tzapu/WiFiManager
 * PubSubClient by Nick O'Leary
 * https://github.com/knolleary/pubsubclient
 */

#include <FTDebouncer.h>

#include "esp_wifi.h"
#include "driver/adc.h"
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <time.h>
#include "RTClib.h"

#include "config.h"
#include "globals.h"
#include "sensor.h"
#include "time_functions.h"
#include "output.h"

// Global
#define NUT_TARGET 14

// Buttons
FTDebouncer pinDebouncer;
#define BTN_NUT 4 // Green/White
#define BTN_TEST 15 // 
#define BTN_ACTION 26

// MOSFET
#define MOSFET_NUT 2 // Green
#define MOSFET_PUMP 13

// Network
//WiFiManager wm;
WiFiClientSecure client;
PubSubClient mqtt(client);

#define MQTT_SERVER "io.adafruit.com"
#define MQTT_PORT 8883
// Set the following in config.h
//#define MQTT_USER ""
//#define MQTT_PASSWORD ""
//#define MQTT_TOPIC ""

// State
bool hygroActive = false;
short readingRound = 0;
float hyg1 = 0;
float hyg2 = 0;
float hyg3 = 0;
float temperature;
short humidity;
float soilTemperature;
short analogVoltage;
float voltage;
short level = 0;

long nutCounter = 0;
bool actionPump = false;
bool actionNut = false;

typedef enum {
  MEASURING,
  WATERING
} states;

states state = MEASURING;

// Timers
unsigned long wakeTime = millis();
#define  MAX_PUMP_TIME 180000 // 3 minute
unsigned long pumpStartTime = millis();
unsigned long timeToStayAwake;
#define MAX_WAKE_TIME 600000 // 10 minutes

// Automation
short waterHour[] = { 6 };
RTC_DATA_ATTR short wateredHour = -1;

short nutritionWeekDays[] = { 1, 4 };
RTC_DATA_ATTR short nutritionAddedDay = -1;

typedef enum {
  ACTION,
  LED,
  NUT,
  PUMP,
  LEVEL,
  TIMER
} Event;

void addNutrition() {
  nutCounter = 0;
  digitalWrite(MOSFET_NUT, HIGH);
}

void pump(bool start) {
  if (start) {
    state = WATERING;
    pumpStartTime = millis();
    actionPump = true;
    digitalWrite(MOSFET_PUMP, HIGH);
    mqttSendEvent(PUMP, 1);
  } else {
    actionPump = false;
    digitalWrite(MOSFET_PUMP, LOW);
    mqttSendEvent(PUMP, 0);
  }
}

// Button pressed
void onPinActivated(int pinNumber) {
  switch (pinNumber) {
    case BTN_NUT:
      nutCounter++;
      Serial.print("Nut: ");
      Serial.println(nutCounter);
      if (nutCounter >= NUT_TARGET) {
        digitalWrite(MOSFET_NUT, LOW);
        digitalWrite(LED_ACTION, LOW);
        Serial.println("Nut: Target reached");
      }
      mqttSendEvent(NUT, nutCounter);
      break;
    case BTN_TEST:
      Serial.println("More nuts");
      digitalWrite(MOSFET_NUT, HIGH);
      break;
    case BTN_LEVEL_2L:
      Serial.println("Liquid: > 2l");
      // If nutrition should be added, do it now
      if (actionNut) {
        addNutrition();
        actionNut = false;
      }
      level = 2;
      mqttSendEvent(LEVEL, 2);
      break;
    case BTN_LEVEL_5L:
      Serial.println("Liquid: > 5l");
      // Stop pump
      pump(false);
      level = 5;
      mqttSendEvent(LEVEL, 5);
      break;
    case BTN_ACTION:
      
      // Cancel pump and nutrition
      if (actionNut) {
        Serial.println("Action: Cancel");
        
        pump(false);
        actionNut = false;
        state = MEASURING;
        
        digitalWrite(LED_ACTION, LOW);
        
        digitalWrite(MOSFET_NUT, LOW);
        mqttSendEvent(ACTION, 0);
        break;
      }

      // If already pumping
      if (actionPump) {
        Serial.println("Action: Enable nutrition");
        // Add nutrition when water level reached
        actionNut = true;
        digitalWrite(LED_ACTION, HIGH);
        mqttSendEvent(ACTION, -2);
      } else {
        // Enable pump
        Serial.println("Action: Start pump");
        pump(true);
        mqttSendEvent(ACTION, -1);
      }
      break;
  }    
}

// Button released
void onPinDeactivated(int pinNumber) {
  switch (pinNumber) {
     case BTN_LEVEL_2L:
      Serial.println("Liquid: < 2l");
      level = 0;
      mqttSendEvent(LEVEL, 0);
      wakeTime = millis();
      timeToStayAwake = WAKE_TIME_AFTER_EMPTY;
      state = MEASURING;
      break;
    case BTN_LEVEL_5L:
      Serial.println("Liquid: < 5l");
      level = 2;
      mqttSendEvent(LEVEL, 2);
      break;
    case BTN_ACTION:
      break;
  }
}

bool shouldWater(short hour) {
  // Do nothing if we already watered this hour
  if (wateredHour == hour)
    return false;

  if (hour > wateredHour)
    wateredHour = -1;


  for(int i = 0; i < sizeof(waterHour) / sizeof(waterHour[0]); i++) {
    if (waterHour[i] == hour) {
      return true;
    }
  }

  return false;
}

bool shouldAddNutrition(short weekDay) {
  // Do nothing if we already added nutrtion this day
  if (nutritionAddedDay == weekDay)
    return false;

  for(int i = 0; i < sizeof(nutritionWeekDays) / sizeof(nutritionWeekDays[0]); i++) {
    if (nutritionWeekDays[i] == weekDay) {
      return true;
    }
  }

  return false;
}

void setup() {
  // Setup Serial
  Serial.begin(115200);
  delay(1000);
  Serial.println("Ready");
  esp_wifi_start();
  esp_wifi_restore();

  // MOSFET
  pinMode(MOSFET_PUMP, OUTPUT);
  digitalWrite(MOSFET_PUMP, LOW);
  pinMode(MOSFET_NUT, OUTPUT);
  digitalWrite(MOSFET_NUT, LOW);

  // Button
  pinDebouncer.addPin(BTN_NUT, HIGH, INPUT_PULLUP);
  pinDebouncer.addPin(BTN_TEST, HIGH, INPUT_PULLUP);
  pinDebouncer.addPin(BTN_LEVEL_2L, HIGH, INPUT_PULLUP);
  pinDebouncer.addPin(BTN_LEVEL_5L, HIGH, INPUT_PULLUP);
  pinDebouncer.addPin(BTN_ACTION, HIGH, INPUT_PULLUP);
  pinDebouncer.begin();

  // LED
  ledSetup();
  ledBlink(1000);

  // Hygro, OneWire & DHT
  setupSensor();

  // Setup WiFiManager
  setupWiFi();

  // Setup NTP
  setupTime();

  // Setup RTC
  setupRTC();

  // Setup deep sleep
  bool manual = check_wakeup_reason();

  // Wake up on button press
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_26, 0);

  short hour = getHour();
  short weekDay = getWeekDay();

  // Auto water if hour is correct and this is not our first boot
  if ((bootCount > 0) && shouldWater(hour) || (waterFailSafe <= 0)) {
    // TODO: Water fail safe needs to be updated now we have a working RTC
    if (waterFailSafe <= 0)
      mqttSendEvent(ACTION, 5);
    
    wateredHour = hour;

    if (shouldAddNutrition(weekDay)) {
      actionNut = true;
      nutritionAddedDay = weekDay;
    }

    pump(true);
    timeToStayAwake = WAKE_TIME_LONG;
    state = WATERING;
    waterFailSafe = AUTO_WATER_FAIL_SAFE;
    Serial.printf("Auto water of plants. Staying awake till %d seconds after water emptied\n", timeToStayAwake);
    mqttSendEvent(ACTION, 2);
  } else if (bootCount == 1) {
    timeToStayAwake = WAKE_TIME_LONG;
    Serial.printf("First boot. Staying awake for %d seconds\n", timeToStayAwake);
    mqttSendEvent(ACTION, 0);
  } else if (manual) {
    timeToStayAwake = WAKE_TIME_LONG;
    Serial.printf("Manual wakeup. Staying awake for %d seconds\n", timeToStayAwake);
  } else {
    timeToStayAwake = WAKE_TIME_SHORT;
    Serial.printf("Measurement only. Staying awake for %d seconds\n", timeToStayAwake);
    mqttSendEvent(ACTION, 1);
  }

  ledBlink(500);
}

//bool lState = HIGH;

void loop() {
  pinDebouncer.update();
  //wm.process();
  mqtt.loop();
  ledProcess();

  if (readSensor()) {
    serialLog();

    if (readingRound % 3 == 0) {
      temperature = temperature / readingRound;
      humidity = humidity / readingRound;
      voltage = voltage / readingRound;
      hyg1 = hyg1 / readingRound;
      hyg2 = hyg2 / readingRound;
      hyg3 = hyg3 / readingRound;
      soilTemperature = soilTemperature / readingRound;

      if (WiFi.status() == WL_CONNECTED) {
        ledBlink(0);
        mqttSend();
      }
    }
  }

  // Stops pump after 3 minutes if sensors has not triggered yet
  if (actionPump && (millis() - pumpStartTime >= MAX_PUMP_TIME)) {
    Serial.println("Maximum pump time exceeded");
    pump(false);
    state = MEASURING;
    mqttSendEvent(ACTION, 3);
  }

  // Stop measuring if max time exceeded
  if ((state == WATERING) && (millis() - wakeTime >= MAX_WAKE_TIME)) {
    Serial.println("Maximum wake time exceeded");
    state = MEASURING;
    mqttSendEvent(ACTION, 4);
  }

  // Time to sleep
  // TODO: Check that no action is being performed
  if ((state == MEASURING) && (millis() - wakeTime >= timeToStayAwake * 1000)) {
    mqttSendEvent(ACTION, 0);
    int seconds = secondsTillNextHour();
    // Wait a bit longer to be sure hour has changed
    seconds += 60;
    
    Serial.println("Disconnecting MQTT..");
    mqtt.disconnect();
    delay(100);

    Serial.println("Disconnecting WiFi..");
    esp_wifi_disconnect();
    delay(100);
    esp_wifi_stop();
    delay(100);
    esp_wifi_deinit();
    delay(100);

    Serial.printf("Going to sleep for %d seconds", seconds);
    Serial.flush();
    esp_sleep_enable_timer_wakeup(seconds * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }
}