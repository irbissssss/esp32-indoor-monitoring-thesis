#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "Adafruit_SHT31.h"

// =====================
// Nordic Home Sensor Node 2
// Bedroom
// ESP32-C3 + SHT3X + ESP-NOW
// Safe FINAL version
// =====================

// ---------- Node identity ----------
#define NODE_ID 2
#define NODE_NAME "Bedroom"

// ---------- Mode selection ----------
#define MODE_DEVELOPMENT 1
#define MODE_TESTING 2
#define MODE_FINAL 3

// CHANGE ONLY THIS LINE:
//
// MODE_DEVELOPMENT = no deep sleep, sends every 10 sec, best for coding
// MODE_TESTING     = sends once, waits 45 sec, sleeps 30 sec
// MODE_FINAL       = sends once, waits 15 sec, sleeps 30 min
#define NODE_MODE MODE_FINAL

#if NODE_MODE == MODE_DEVELOPMENT
  #define SEND_INTERVAL_MS 10000UL
  #define USE_DEEP_SLEEP false
  #define SLEEP_SECONDS 0
  #define BEFORE_SLEEP_DELAY_MS 0
#elif NODE_MODE == MODE_TESTING
  #define SEND_INTERVAL_MS 0
  #define USE_DEEP_SLEEP true
  #define SLEEP_SECONDS 30
  #define BEFORE_SLEEP_DELAY_MS 45000UL
#elif NODE_MODE == MODE_FINAL
  #define SEND_INTERVAL_MS 0
  #define USE_DEEP_SLEEP true
  #define SLEEP_SECONDS 1800
  #define BEFORE_SLEEP_DELAY_MS 15000UL
#else
  #error "Invalid NODE_MODE selected"
#endif

// ---------- SHT3X wiring ----------
#define SDA_PIN 4
#define SCL_PIN 5
#define SHT31_ADDRESS 0x44

// ---------- ESP-NOW ----------
#define WIFI_CHANNEL 1

// Main ESP32-S3 hub STA MAC address
uint8_t mainHubMac[] = {0xE0, 0x72, 0xA1, 0xD6, 0xDF, 0x94};

Adafruit_SHT31 sht31 = Adafruit_SHT31();

// Survives deep sleep, resets only when power is removed
RTC_DATA_ATTR unsigned long readingCounter = 0;

typedef struct SensorData {
  int nodeId;
  float temperature;
  float humidity;
  unsigned long readingNumber;
} SensorData;

SensorData dataToSend;

bool sensorReady = false;
bool espNowReady = false;

void printMode() {
  Serial.print("Mode: ");

#if NODE_MODE == MODE_DEVELOPMENT
  Serial.println("DEVELOPMENT");
  Serial.println("Behavior: sends every 10 seconds, no deep sleep");
#elif NODE_MODE == MODE_TESTING
  Serial.println("TESTING");
  Serial.println("Behavior: sends once, waits 45 sec, sleeps 30 sec");
#elif NODE_MODE == MODE_FINAL
  Serial.println("FINAL");
  Serial.println("Behavior: sends once, waits 15 sec, sleeps 30 min");
#endif
}

void scanI2C() {
  Serial.println();
  Serial.println("I2C scan started...");

  byte count = 0;

  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("I2C device found at 0x");

      if (address < 16) {
        Serial.print("0");
      }

      Serial.println(address, HEX);
      count++;
    }
  }

  if (count == 0) {
    Serial.println("No I2C devices found.");
  } else {
    Serial.print("I2C devices found: ");
    Serial.println(count);
  }

  Serial.println("I2C scan finished.");
  Serial.println();
}

void shutDownRadio() {
  // Only deinitialize ESP-NOW if it was successfully initialized.
  // This prevents crashes if setup failed before ESP-NOW started.
  if (espNowReady) {
    esp_now_deinit();
    espNowReady = false;
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void goToSleep() {
  if (!USE_DEEP_SLEEP) {
    return;
  }

  Serial.println();
  Serial.print("Upload safety window: ");
  Serial.print(BEFORE_SLEEP_DELAY_MS / 1000);
  Serial.println(" seconds before sleep.");

  Serial.println(
    "If you need to upload new code, click Upload now or use BOOT mode."
  );

  Serial.flush();

  delay(BEFORE_SLEEP_DELAY_MS);

  Serial.println();
  Serial.print("Going to deep sleep for ");
  Serial.print(SLEEP_SECONDS);
  Serial.println(" seconds...");
  Serial.println();

  Serial.flush();

  shutDownRadio();

  Wire.end();
  delay(100);

  Serial.end();
  delay(200);

  esp_sleep_enable_timer_wakeup(
    (uint64_t)SLEEP_SECONDS * 1000000ULL
  );

  esp_deep_sleep_start();
}

bool setupSensor() {
  Wire.begin(SDA_PIN, SCL_PIN);

  // Slower I2C is more stable with jumper wires and prototypes.
  Wire.setClock(50000);

  delay(300);

  scanI2C();

  if (!sht31.begin(SHT31_ADDRESS)) {
    Serial.println("ERROR: SHT3X not found at 0x44");
    Serial.println("Check wiring:");
    Serial.println("VIN -> 3.3V");
    Serial.println("GND -> G");
    Serial.println("SDA -> GPIO 4");
    Serial.println("SCL -> GPIO 5");

    return false;
  }

  Serial.println("SHT3X sensor found.");

  // Reset sensor before the first reading.
  sht31.reset();
  delay(500);

  // Make sure the sensor heater is disabled.
  sht31.heater(false);
  delay(300);

  return true;
}

bool setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  delay(100);

  esp_wifi_set_channel(
    WIFI_CHANNEL,
    WIFI_SECOND_CHAN_NONE
  );

  Serial.print("Node MAC address: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: ESP-NOW init failed");

    espNowReady = false;
    return false;
  }

  espNowReady = true;

  esp_now_peer_info_t peerInfo = {};

  memcpy(
    peerInfo.peer_addr,
    mainHubMac,
    6
  );

  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("ERROR: failed to add main hub peer");

    return false;
  }

  Serial.println("Main hub peer added.");

  return true;
}

bool readSht31WithRetry(
  float &temperature,
  float &humidity
) {
  for (int attempt = 1; attempt <= 3; attempt++) {
    temperature = sht31.readTemperature();

    delay(200);

    humidity = sht31.readHumidity();

    if (!isnan(temperature) && !isnan(humidity)) {
      return true;
    }

    Serial.print("SHT3X read failed, attempt ");
    Serial.print(attempt);
    Serial.println("/3");

    delay(500);
  }

  return false;
}

bool readAndSendSensorData() {
  float temperature = NAN;
  float humidity = NAN;

  if (!readSht31WithRetry(temperature, humidity)) {
    Serial.println("ERROR: SHT3X read failed after retries");

    return false;
  }

  readingCounter++;

  dataToSend.nodeId = NODE_ID;
  dataToSend.temperature = temperature;
  dataToSend.humidity = humidity;
  dataToSend.readingNumber = readingCounter;

  esp_err_t result = esp_now_send(
    mainHubMac,
    reinterpret_cast<uint8_t *>(&dataToSend),
    sizeof(dataToSend)
  );

  Serial.print(NODE_NAME);
  Serial.print(" | #");
  Serial.print(readingCounter);
  Serial.print(" | ");
  Serial.print(temperature, 2);
  Serial.print(" C | ");
  Serial.print(humidity, 2);
  Serial.print(" % | ");

  if (result == ESP_OK) {
    Serial.println("send request OK");

    return true;
  }

  Serial.print("send failed: ");
  Serial.println(result);

  return false;
}

void setup() {
  Serial.begin(115200);

  delay(1500);

  Serial.println();
  Serial.println("=================================");
  Serial.println("Nordic Home Sensor Node 2");
  Serial.println("Location: Bedroom");
  Serial.println("ESP32-C3 + SHT3X + ESP-NOW");
  Serial.println("Safe FINAL version");
  Serial.println("=================================");

  printMode();

  sensorReady = setupSensor();

  if (!sensorReady) {
    Serial.println("Sensor failed. Will sleep and retry later.");

    delay(1500);

    goToSleep();
    return;
  }

  espNowReady = setupEspNow();

  if (!espNowReady) {
    Serial.println("ESP-NOW failed. Will sleep and retry later.");

    delay(1500);

    goToSleep();
    return;
  }

  readAndSendSensorData();

  // Give ESP-NOW time to transmit before entering deep sleep.
  delay(1000);

  if (USE_DEEP_SLEEP) {
    goToSleep();
  } else {
    Serial.println("Development mode active. USB stays connected.");
    Serial.println("Sending again every 10 seconds.");
  }
}

void loop() {
  if (USE_DEEP_SLEEP) {
    return;
  }

  static unsigned long lastSend = 0;

  if (millis() - lastSend >= SEND_INTERVAL_MS) {
    lastSend = millis();

    if (sensorReady && espNowReady) {
      readAndSendSensorData();
    }
  }

  delay(10);
}