// ESP32 Main module code



#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <LittleFS.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =====================
// Nordic Home IoT Main Hub V3
// ESP32-S3 + OLED + RTC + ESP-NOW + LittleFS + Local Dashboard
// Compatible with:
// Node 1 = Kitchen
// Node 2 = Bedroom
// =====================

// ---------- OLED ----------
#define OLED_SDA 8
#define OLED_SCL 9
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------- RTC DS3231 ----------
#define RTC_SDA 17
#define RTC_SCL 18
#define DS3231_ADDRESS 0x68

TwoWire RTCWire = TwoWire(1);

// ---------- Button ----------
#define BUTTON_PIN 0   // BOOT button on many ESP32-S3 boards

// ---------- Dashboard Wi-Fi ----------
const char* AP_SSID = "NordicHome";
const char* AP_PASSWORD = "nordichome123";

// ESP-NOW and AP should use same Wi-Fi channel
#define WIFI_CHANNEL 1

// Dashboard active time after button press
const unsigned long DASHBOARD_ACTIVE_MS = 5UL * 60UL * 1000UL; // 5 minutes

// Final nodes send every 30 minutes.
// 45 minutes means the dashboard does not mark them stale too early.
const unsigned long NODE_STALE_MS = 45UL * 60UL * 1000UL; // 45 minutes

// ---------- Web server ----------
WebServer server(80);
bool routesConfigured = false;

// ---------- LittleFS ----------
const char* DATA_FILE = "/data.csv";

// ---------- Sensor packet ----------
typedef struct SensorData {
  int nodeId;
  float temperature;
  float humidity;
  unsigned long readingNumber;
} SensorData;

// ---------- Stored node state ----------
struct NodeState {
  bool hasData = false;
  float temperature = 0;
  float humidity = 0;
  unsigned long readingNumber = 0;
  unsigned long lastMillis = 0;

  int year = 2000;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
};

NodeState node1;
NodeState node2;

// ---------- History in RAM for dashboard graphs ----------
#define HISTORY_SIZE 672
// 672 points:
// - at 30 sec interval = about 5.6 hours
// - at 30 min interval = 14 days
// Full long-term data is stored in /data.csv

struct HistoryPoint {
  bool valid = false;
  int nodeId = 0;
  float temperature = 0;
  float humidity = 0;

  int year = 2000;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;

  unsigned long readingNumber = 0;
};

HistoryPoint history1[HISTORY_SIZE];
HistoryPoint history2[HISTORY_SIZE];

int historyIndex1 = 0;
int historyIndex2 = 0;

// ---------- ESP-NOW receive queue ----------
#define RECEIVE_QUEUE_SIZE 8

struct QueuedPacket {
  bool valid = false;
  SensorData data;
  uint8_t mac[6];
};

QueuedPacket receiveQueue[RECEIVE_QUEUE_SIZE];
volatile int queueHead = 0;
volatile int queueTail = 0;

portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;

// ---------- Dashboard state ----------
bool dashboardActive = false;
bool oledActive = false;
unsigned long dashboardStartedAt = 0;

bool lastButtonReading = HIGH;
unsigned long lastButtonChange = 0;
const unsigned long debounceMs = 80;

// ---------- RTC helpers ----------
byte bcdToDec(byte value) {
  return ((value / 16) * 10) + (value % 16);
}

byte decToBcd(byte value) {
  return ((value / 10) * 16) + (value % 10);
}

// RTC is already set. Do not call this unless intentionally resetting time.
void setRtcTime() {
  RTCWire.beginTransmission(DS3231_ADDRESS);
  RTCWire.write(0x00);

  RTCWire.write(decToBcd(0));    // seconds
  RTCWire.write(decToBcd(40));   // minutes
  RTCWire.write(decToBcd(23));   // hours
  RTCWire.write(decToBcd(2));    // day of week
  RTCWire.write(decToBcd(23));   // day
  RTCWire.write(decToBcd(6));    // month
  RTCWire.write(decToBcd(26));   // year = 2026

  RTCWire.endTransmission();

  Serial.println("RTC time manually set.");
}

bool readRtcTime(int &year, int &month, int &day, int &hour, int &minute, int &second) {
  RTCWire.beginTransmission(DS3231_ADDRESS);
  RTCWire.write(0x00);

  if (RTCWire.endTransmission() != 0) {
    return false;
  }

  RTCWire.requestFrom(DS3231_ADDRESS, 7);

  if (RTCWire.available() < 7) {
    return false;
  }

  second = bcdToDec(RTCWire.read() & 0x7F);
  minute = bcdToDec(RTCWire.read());
  hour = bcdToDec(RTCWire.read() & 0x3F);

  RTCWire.read(); // day of week ignored

  day = bcdToDec(RTCWire.read());
  month = bcdToDec(RTCWire.read() & 0x1F);
  year = 2000 + bcdToDec(RTCWire.read());

  return true;
}

String twoDigits(int value) {
  if (value < 10) return "0" + String(value);
  return String(value);
}

String timeString(int h, int m, int s) {
  return twoDigits(h) + ":" + twoDigits(m) + ":" + twoDigits(s);
}

String dateString(int y, int mo, int d) {
  return twoDigits(d) + "." + twoDigits(mo) + "." + String(y);
}

String isoDateString(int y, int mo, int d) {
  return String(y) + "-" + twoDigits(mo) + "-" + twoDigits(d);
}

String fullTimestamp(int y, int mo, int d, int h, int mi, int s) {
  return isoDateString(y, mo, d) + " " + timeString(h, mi, s);
}

String nodeName(int nodeId) {
  if (nodeId == 1) return "Kitchen";
  if (nodeId == 2) return "Bedroom";
  return "Unknown";
}

// ---------- OLED ----------
void oledOn() {
  display.ssd1306_command(SSD1306_DISPLAYON);
  oledActive = true;
}

void oledOff() {
  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  oledActive = false;
}

void drawOLED() {
  if (!oledActive) return;

  int year, month, day, hour, minute, second;
  bool rtcOk = readRtcTime(year, month, day, hour, minute, second);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Nordic Home IoT");

  display.setCursor(0, 10);
  if (rtcOk) {
    display.print(timeString(hour, minute, second));
    display.print(" ");
    display.print(twoDigits(day));
    display.print(".");
    display.print(twoDigits(month));
  } else {
    display.println("RTC error");
  }

  display.setCursor(0, 24);
  if (node1.hasData) {
    display.print("Kitchen ");
    display.print(node1.temperature, 1);
    display.print("C ");
    display.print(node1.humidity, 0);
    display.println("%");
  } else {
    display.println("Kitchen waiting");
  }

  display.setCursor(0, 37);
  if (node2.hasData) {
    display.print("Bedroom ");
    display.print(node2.temperature, 1);
    display.print("C ");
    display.print(node2.humidity, 0);
    display.println("%");
  } else {
    display.println("Bedroom waiting");
  }

  display.setCursor(0, 54);
  if (dashboardActive) {
    display.println("WiFi: NordicHome");
  } else {
    display.println("Dashboard off");
  }

  display.display();
}

// ---------- File logging ----------
void ensureDataFile() {
  if (!LittleFS.exists(DATA_FILE)) {
    File file = LittleFS.open(DATA_FILE, FILE_WRITE);

    if (!file) {
      Serial.println("Failed to create data.csv");
      return;
    }

    file.println("timestamp,date,time,nodeId,nodeName,temperature,humidity,readingNumber");
    file.close();

    Serial.println("Created /data.csv");
  }
}

void appendReadingToFlash(
  int nodeId,
  float temperature,
  float humidity,
  unsigned long readingNumber,
  int year,
  int month,
  int day,
  int hour,
  int minute,
  int second
) {
  File file = LittleFS.open(DATA_FILE, FILE_APPEND);

  if (!file) {
    Serial.println("Failed to open data.csv for append");
    return;
  }

  String timestamp = fullTimestamp(year, month, day, hour, minute, second);

  file.print(timestamp);
  file.print(",");
  file.print(dateString(year, month, day));
  file.print(",");
  file.print(timeString(hour, minute, second));
  file.print(",");
  file.print(nodeId);
  file.print(",");
  file.print(nodeName(nodeId));
  file.print(",");
  file.print(temperature, 2);
  file.print(",");
  file.print(humidity, 2);
  file.print(",");
  file.println(readingNumber);

  file.close();
}

size_t logFileSize() {
  if (!LittleFS.exists(DATA_FILE)) return 0;

  File file = LittleFS.open(DATA_FILE, FILE_READ);
  if (!file) return 0;

  size_t size = file.size();
  file.close();

  return size;
}

// ---------- History ----------
void addHistoryPoint(
  int nodeId,
  float temperature,
  float humidity,
  unsigned long readingNumber,
  int year,
  int month,
  int day,
  int hour,
  int minute,
  int second
) {
  HistoryPoint* targetHistory = nullptr;
  int* targetIndex = nullptr;

  if (nodeId == 1) {
    targetHistory = history1;
    targetIndex = &historyIndex1;
  } else if (nodeId == 2) {
    targetHistory = history2;
    targetIndex = &historyIndex2;
  } else {
    return;
  }

  int index = *targetIndex;

  targetHistory[index].valid = true;
  targetHistory[index].nodeId = nodeId;
  targetHistory[index].temperature = temperature;
  targetHistory[index].humidity = humidity;
  targetHistory[index].readingNumber = readingNumber;
  targetHistory[index].year = year;
  targetHistory[index].month = month;
  targetHistory[index].day = day;
  targetHistory[index].hour = hour;
  targetHistory[index].minute = minute;
  targetHistory[index].second = second;

  *targetIndex = (*targetIndex + 1) % HISTORY_SIZE;
}

// ---------- Stats ----------
String calculateStatsJson(HistoryPoint history[]) {
  int count = 0;

  float tempMin = 0;
  float tempMax = 0;
  float tempSum = 0;

  float humMin = 0;
  float humMax = 0;
  float humSum = 0;

  for (int i = 0; i < HISTORY_SIZE; i++) {
    if (!history[i].valid) continue;

    float t = history[i].temperature;
    float h = history[i].humidity;

    if (count == 0) {
      tempMin = t;
      tempMax = t;
      humMin = h;
      humMax = h;
    } else {
      if (t < tempMin) tempMin = t;
      if (t > tempMax) tempMax = t;
      if (h < humMin) humMin = h;
      if (h > humMax) humMax = h;
    }

    tempSum += t;
    humSum += h;
    count++;
  }

  float tempAvg = 0;
  float humAvg = 0;

  if (count > 0) {
    tempAvg = tempSum / count;
    humAvg = humSum / count;
  }

  String json = "{";

  json += "\"count\":";
  json += String(count);

  json += ",\"tempMin\":";
  json += String(tempMin, 2);

  json += ",\"tempMax\":";
  json += String(tempMax, 2);

  json += ",\"tempAvg\":";
  json += String(tempAvg, 2);

  json += ",\"humMin\":";
  json += String(humMin, 2);

  json += ",\"humMax\":";
  json += String(humMax, 2);

  json += ",\"humAvg\":";
  json += String(humAvg, 2);

  json += "}";

  return json;
}

// ---------- ESP-NOW callback and queue ----------
void onDataReceived(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(SensorData)) {
    return;
  }

  portENTER_CRITICAL_ISR(&queueMux);

  int nextHead = (queueHead + 1) % RECEIVE_QUEUE_SIZE;

  if (nextHead != queueTail) {
    memcpy(&receiveQueue[queueHead].data, data, sizeof(SensorData));
    memcpy(receiveQueue[queueHead].mac, info->src_addr, 6);
    receiveQueue[queueHead].valid = true;
    queueHead = nextHead;
  }

  portEXIT_CRITICAL_ISR(&queueMux);
}

bool popPacket(QueuedPacket &packet) {
  bool hasPacket = false;

  portENTER_CRITICAL(&queueMux);

  if (queueTail != queueHead && receiveQueue[queueTail].valid) {
    packet = receiveQueue[queueTail];
    receiveQueue[queueTail].valid = false;
    queueTail = (queueTail + 1) % RECEIVE_QUEUE_SIZE;
    hasPacket = true;
  }

  portEXIT_CRITICAL(&queueMux);

  return hasPacket;
}

void printMac(const uint8_t* mac) {
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 16) Serial.print("0");
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(":");
  }
}

void processIncomingPacket(const QueuedPacket& packet) {
  SensorData incoming = packet.data;

  int year, month, day, hour, minute, second;
  bool rtcOk = readRtcTime(year, month, day, hour, minute, second);

  if (!rtcOk) {
    year = 2000;
    month = 1;
    day = 1;
    hour = 0;
    minute = 0;
    second = 0;
  }

  NodeState* target = nullptr;

  if (incoming.nodeId == 1) target = &node1;
  if (incoming.nodeId == 2) target = &node2;

  if (target == nullptr) {
    Serial.print("Unknown node ID: ");
    Serial.println(incoming.nodeId);
    return;
  }

  target->hasData = true;
  target->temperature = incoming.temperature;
  target->humidity = incoming.humidity;
  target->readingNumber = incoming.readingNumber;
  target->lastMillis = millis();

  target->year = year;
  target->month = month;
  target->day = day;
  target->hour = hour;
  target->minute = minute;
  target->second = second;

  addHistoryPoint(
    incoming.nodeId,
    incoming.temperature,
    incoming.humidity,
    incoming.readingNumber,
    year,
    month,
    day,
    hour,
    minute,
    second
  );

  appendReadingToFlash(
    incoming.nodeId,
    incoming.temperature,
    incoming.humidity,
    incoming.readingNumber,
    year,
    month,
    day,
    hour,
    minute,
    second
  );

  Serial.print(fullTimestamp(year, month, day, hour, minute, second));
  Serial.print(" | ");
  Serial.print(nodeName(incoming.nodeId));
  Serial.print(" | ");
  Serial.print(incoming.temperature, 2);
  Serial.print(" C | ");
  Serial.print(incoming.humidity, 2);
  Serial.print(" % | #");
  Serial.print(incoming.readingNumber);
  Serial.print(" | saved | ");
  printMac(packet.mac);
  Serial.println();

  drawOLED();
}

void processReceiveQueue() {
  QueuedPacket packet;

  while (popPacket(packet)) {
    processIncomingPacket(packet);
  }
}

// ---------- JSON helpers ----------
String nodeStatusJson(const NodeState& node) {
  if (!node.hasData) return "waiting";

  unsigned long age = millis() - node.lastMillis;

  if (age > NODE_STALE_MS) return "stale";

  return "online";
}

String readableAge(const NodeState& node) {
  if (!node.hasData) return "--";

  unsigned long seconds = (millis() - node.lastMillis) / 1000;

  if (seconds < 60) {
    return String(seconds) + " sec ago";
  }

  unsigned long minutes = seconds / 60;

  if (minutes < 60) {
    return String(minutes) + " min ago";
  }

  unsigned long hours = minutes / 60;
  return String(hours) + " h ago";
}

String nodeJson(const NodeState& node, int nodeId) {
  String json = "{";

  json += "\"id\":";
  json += String(nodeId);

  json += ",\"name\":\"";
  json += nodeName(nodeId);
  json += "\"";

  json += ",\"hasData\":";
  json += node.hasData ? "true" : "false";

  json += ",\"status\":\"";
  json += nodeStatusJson(node);
  json += "\"";

  json += ",\"temperature\":";
  json += String(node.temperature, 2);

  json += ",\"humidity\":";
  json += String(node.humidity, 2);

  json += ",\"readingNumber\":";
  json += String(node.readingNumber);

  json += ",\"lastSeenSeconds\":";
  if (node.hasData) json += String((millis() - node.lastMillis) / 1000);
  else json += "null";

  json += ",\"lastSeenText\":\"";
  json += readableAge(node);
  json += "\"";

  json += ",\"time\":\"";
  json += timeString(node.hour, node.minute, node.second);
  json += "\"";

  json += ",\"date\":\"";
  json += dateString(node.year, node.month, node.day);
  json += "\"";

  json += ",\"timestamp\":\"";
  json += fullTimestamp(node.year, node.month, node.day, node.hour, node.minute, node.second);
  json += "\"";

  json += "}";

  return json;
}

String historyJson(HistoryPoint history[], int startIndex, int limit) {
  if (limit < 1) limit = 1;
  if (limit > HISTORY_SIZE) limit = HISTORY_SIZE;

  String json = "[";

  bool first = true;
  int added = 0;

  int start = startIndex - limit;
  if (start < 0) start += HISTORY_SIZE;

  for (int i = 0; i < HISTORY_SIZE && added < limit; i++) {
    int index = (start + i) % HISTORY_SIZE;

    if (!history[index].valid) continue;

    if (!first) json += ",";
    first = false;

    json += "{";

    json += "\"date\":\"";
    json += dateString(history[index].year, history[index].month, history[index].day);
    json += "\"";

    json += ",\"time\":\"";
    json += timeString(history[index].hour, history[index].minute, history[index].second);
    json += "\"";

    json += ",\"timestamp\":\"";
    json += fullTimestamp(
      history[index].year,
      history[index].month,
      history[index].day,
      history[index].hour,
      history[index].minute,
      history[index].second
    );
    json += "\"";

    json += ",\"temperature\":";
    json += String(history[index].temperature, 2);

    json += ",\"humidity\":";
    json += String(history[index].humidity, 2);

    json += ",\"readingNumber\":";
    json += String(history[index].readingNumber);

    json += "}";

    added++;
  }

  json += "]";
  return json;
}

// ---------- API ----------
void handleApiLive() {
  int year, month, day, hour, minute, second;
  bool rtcOk = readRtcTime(year, month, day, hour, minute, second);

  String json = "{";

  json += "\"system\":\"Nordic Home IoT\"";

  json += ",\"rtcOk\":";
  json += rtcOk ? "true" : "false";

  json += ",\"currentTime\":\"";
  if (rtcOk) json += timeString(hour, minute, second);
  else json += "RTC error";
  json += "\"";

  json += ",\"currentDate\":\"";
  if (rtcOk) json += dateString(year, month, day);
  else json += "RTC error";
  json += "\"";

  json += ",\"dashboardTimeoutSeconds\":";
  if (dashboardActive) {
    unsigned long elapsed = millis() - dashboardStartedAt;
    unsigned long remaining = elapsed >= DASHBOARD_ACTIVE_MS ? 0 : (DASHBOARD_ACTIVE_MS - elapsed) / 1000;
    json += String(remaining);
  } else {
    json += "0";
  }

  json += ",\"storageUsed\":";
  json += String(LittleFS.usedBytes());

  json += ",\"storageTotal\":";
  json += String(LittleFS.totalBytes());

  json += ",\"logFileSize\":";
  json += String(logFileSize());

  json += ",\"node1\":";
  json += nodeJson(node1, 1);

  json += ",\"node2\":";
  json += nodeJson(node2, 2);

  json += "}";

  server.send(200, "application/json", json);
}

void handleApiHistory() {
  int limit = 96;

  if (server.hasArg("limit")) {
    limit = server.arg("limit").toInt();
  }

  if (limit < 10) limit = 10;
  if (limit > 300) limit = 300;

  String json = "{";

  json += "\"limit\":";
  json += String(limit);

  json += ",\"node1Stats\":";
  json += calculateStatsJson(history1);

  json += ",\"node2Stats\":";
  json += calculateStatsJson(history2);

  json += ",\"history1\":";
  json += historyJson(history1, historyIndex1, limit);

  json += ",\"history2\":";
  json += historyJson(history2, historyIndex2, limit);

  json += "}";

  server.send(200, "application/json", json);
}

void handleDownload() {
  if (!LittleFS.exists(DATA_FILE)) {
    server.send(404, "text/plain", "No data.csv file found");
    return;
  }

  File file = LittleFS.open(DATA_FILE, FILE_READ);

  if (!file) {
    server.send(500, "text/plain", "Failed to open data.csv");
    return;
  }

  server.sendHeader("Content-Disposition", "attachment; filename=nordic_home_data.csv");
  server.streamFile(file, "text/csv");
  file.close();
}

void handleClearLog() {
  LittleFS.remove(DATA_FILE);
  ensureDataFile();

  for (int i = 0; i < HISTORY_SIZE; i++) {
    history1[i].valid = false;
    history2[i].valid = false;
  }

  historyIndex1 = 0;
  historyIndex2 = 0;

  server.send(200, "text/plain", "Log cleared");
}

// Backward compatibility
void handleApi() {
  handleApiLive();
}

// ---------- Dashboard HTML ----------
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Nordic Home IoT Dashboard</title>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">

  <style>
    * {
      box-sizing: border-box;
    }

    body {
      margin: 0;
      font-family: Arial, Helvetica, sans-serif;
      background: #08111f;
      color: #ffffff;
    }

    .page {
      max-width: 980px;
      margin: 0 auto;
      padding: 18px;
    }

    .header {
      border: 1px solid rgba(255,255,255,0.16);
      background: #101d2d;
      padding: 18px;
      margin-bottom: 14px;
    }

    h1 {
      margin: 0;
      font-size: 28px;
    }

    h2 {
      margin: 0 0 12px 0;
      font-size: 21px;
    }

    .subtitle {
      margin-top: 8px;
      color: #aebbd0;
      font-size: 14px;
      line-height: 1.4;
    }

    .topline {
      display: grid;
      grid-template-columns: repeat(2, 1fr);
      gap: 8px;
      margin-top: 14px;
    }

    .pill {
      padding: 9px 10px;
      background: #0b1624;
      border: 1px solid rgba(255,255,255,0.1);
      color: #dce7f7;
      font-size: 14px;
    }

    .grid {
      display: grid;
      grid-template-columns: repeat(2, 1fr);
      gap: 14px;
    }

    .card {
      border: 1px solid rgba(255,255,255,0.16);
      background: #101d2d;
      padding: 18px;
    }

    .wide {
      grid-column: 1 / -1;
    }

    .metric-row {
      display: grid;
      grid-template-columns: repeat(2, 1fr);
      gap: 10px;
    }

    .metric {
      background: #0b1624;
      border: 1px solid rgba(255,255,255,0.1);
      padding: 12px;
    }

    .label {
      color: #9fb0c7;
      font-size: 13px;
      margin-bottom: 6px;
    }

    .value {
      font-size: 34px;
      font-weight: 800;
    }

    .meta {
      margin-top: 12px;
      color: #c9d6e6;
      line-height: 1.7;
      font-size: 14px;
    }

    .status {
      font-weight: 800;
    }

    .online {
      color: #7CFF9B;
    }

    .stale {
      color: #FFD166;
    }

    .waiting {
      color: #FF8FA3;
    }

    .actions {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      margin-top: 18px;
    }

    button, a.button {
      color: #ffffff;
      background: #1b314d;
      border: 1px solid rgba(255,255,255,0.2);
      padding: 10px 14px;
      font-weight: 800;
      text-decoration: none;
      cursor: pointer;
    }

    canvas {
      width: 100%;
      height: 220px;
      background: #07101c;
      border: 1px solid rgba(255,255,255,0.1);
      margin-top: 12px;
    }

    table {
      width: 100%;
      border-collapse: collapse;
      margin-top: 8px;
      font-size: 14px;
    }

    th, td {
      text-align: left;
      padding: 8px;
      border-bottom: 1px solid rgba(255,255,255,0.1);
    }

    th {
      color: #9fb0c7;
    }

    .footer {
      margin-top: 14px;
      color: #9fb0c7;
      font-size: 13px;
      line-height: 1.5;
    }

    @media (max-width: 760px) {
      .grid {
        grid-template-columns: 1fr;
      }

      .topline {
        grid-template-columns: 1fr;
      }

      .value {
        font-size: 29px;
      }

      table {
        font-size: 12px;
      }

      th, td {
        padding: 6px;
      }
    }

    @media print {
      body {
        background: #ffffff;
        color: #000000;
      }

      .page {
        max-width: none;
        padding: 0;
      }

      .header,
      .card {
        background: #ffffff;
        color: #000000;
        border: 1px solid #999999;
        break-inside: avoid;
      }

      .pill,
      .metric {
        background: #ffffff;
        color: #000000;
        border: 1px solid #bbbbbb;
      }

      .subtitle,
      .label,
      .meta,
      .footer,
      th {
        color: #333333;
      }

      .actions {
        display: none;
      }

      canvas {
        border: 1px solid #999999;
      }
    }
  </style>
</head>

<body>
  <div class="page">
    <div class="header">
      <h1>Nordic Home IoT</h1>
      <div class="subtitle">
        Local low-power environmental monitoring prototype.
        ESP32-C3 sensor nodes send temperature and humidity to this ESP32-S3 hub using ESP-NOW.
      </div>

      <div class="topline">
        <div class="pill">Hub time: <strong><span id="currentTime">--:--:--</span></strong></div>
        <div class="pill">Date: <strong><span id="currentDate">--.--.----</span></strong></div>
        <div class="pill">Dashboard auto-off: <strong><span id="timeout">--</span>s</strong></div>
        <div class="pill">CSV log size: <strong><span id="logSize">--</span></strong></div>
      </div>
    </div>

    <div class="grid">
      <div class="card">
        <h2>Kitchen</h2>
        <div class="metric-row">
          <div class="metric">
            <div class="label">Temperature</div>
            <div class="value"><span id="n1temp">--</span>°C</div>
          </div>
          <div class="metric">
            <div class="label">Humidity</div>
            <div class="value"><span id="n1hum">--</span>%</div>
          </div>
        </div>
        <div class="meta">
          Status: <span id="n1status" class="status waiting">waiting</span><br>
          Last update: <span id="n1time">--</span><br>
          Last seen: <span id="n1seen">--</span><br>
          Reading: <span id="n1reading">--</span>
        </div>
      </div>

      <div class="card">
        <h2>Bedroom</h2>
        <div class="metric-row">
          <div class="metric">
            <div class="label">Temperature</div>
            <div class="value"><span id="n2temp">--</span>°C</div>
          </div>
          <div class="metric">
            <div class="label">Humidity</div>
            <div class="value"><span id="n2hum">--</span>%</div>
          </div>
        </div>
        <div class="meta">
          Status: <span id="n2status" class="status waiting">waiting</span><br>
          Last update: <span id="n2time">--</span><br>
          Last seen: <span id="n2seen">--</span><br>
          Reading: <span id="n2reading">--</span>
        </div>
      </div>

      <div class="card wide">
        <h2>Temperature history</h2>
        <canvas id="tempChart" width="900" height="240"></canvas>
      </div>

      <div class="card wide">
        <h2>Humidity history</h2>
        <canvas id="humChart" width="900" height="240"></canvas>
      </div>

      <div class="card wide">
        <h2>Latest readings</h2>
        <table>
          <thead>
            <tr>
              <th>Node</th>
              <th>Time</th>
              <th>Temperature</th>
              <th>Humidity</th>
              <th>Reading</th>
            </tr>
          </thead>
          <tbody id="latestTable"></tbody>
        </table>
      </div>

      <div class="card wide">
        <h2>Data summary</h2>
        <table>
          <thead>
            <tr>
              <th>Node</th>
              <th>Samples</th>
              <th>Temp min</th>
              <th>Temp avg</th>
              <th>Temp max</th>
              <th>Hum min</th>
              <th>Hum avg</th>
              <th>Hum max</th>
            </tr>
          </thead>
          <tbody id="statsTable"></tbody>
        </table>

        <div class="actions">
          <button onclick="generateReport()">Generate PDF Report</button>
          <a class="button" href="/download">Download CSV</a>
        </div>
      </div>
    </div>

    <div class="footer">
      Data is stored locally on the hub in CSV format. The dashboard is available only when activated with the BOOT button.
    </div>
  </div>

<script>
function formatValue(value, decimals) {
  if (value === undefined || value === null || isNaN(value)) return "--";
  return Number(value).toFixed(decimals);
}

function bytesToText(bytes) {
  if (!bytes && bytes !== 0) return "--";
  if (bytes < 1024) return bytes + " B";
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
  return (bytes / (1024 * 1024)).toFixed(2) + " MB";
}

function setStatus(id, status) {
  const el = document.getElementById(id);
  el.textContent = status;
  el.className = "status " + status;
}

function updateNode(prefix, node) {
  if (!node.hasData) {
    setStatus(prefix + "status", "waiting");
    return;
  }

  document.getElementById(prefix + "temp").textContent = formatValue(node.temperature, 1);
  document.getElementById(prefix + "hum").textContent = formatValue(node.humidity, 0);
  document.getElementById(prefix + "time").textContent = node.time + " / " + node.date;
  document.getElementById(prefix + "seen").textContent = node.lastSeenText;
  document.getElementById(prefix + "reading").textContent = node.readingNumber;

  setStatus(prefix + "status", node.status);
}

function updateLatestTable(data) {
  const rows = [];

  if (data.node1.hasData) {
    rows.push(`
      <tr>
        <td>Kitchen</td>
        <td>${data.node1.time}</td>
        <td>${formatValue(data.node1.temperature, 2)} °C</td>
        <td>${formatValue(data.node1.humidity, 2)} %</td>
        <td>${data.node1.readingNumber}</td>
      </tr>
    `);
  }

  if (data.node2.hasData) {
    rows.push(`
      <tr>
        <td>Bedroom</td>
        <td>${data.node2.time}</td>
        <td>${formatValue(data.node2.temperature, 2)} °C</td>
        <td>${formatValue(data.node2.humidity, 2)} %</td>
        <td>${data.node2.readingNumber}</td>
      </tr>
    `);
  }

  if (rows.length === 0) {
    rows.push(`
      <tr>
        <td colspan="5">No measurements yet</td>
      </tr>
    `);
  }

  document.getElementById("latestTable").innerHTML = rows.join("");
}

function statsRow(name, stats) {
  if (!stats || stats.count === 0) {
    return `
      <tr>
        <td>${name}</td>
        <td colspan="7">No measurements yet</td>
      </tr>
    `;
  }

  return `
    <tr>
      <td>${name}</td>
      <td>${stats.count}</td>
      <td>${formatValue(stats.tempMin, 2)} °C</td>
      <td>${formatValue(stats.tempAvg, 2)} °C</td>
      <td>${formatValue(stats.tempMax, 2)} °C</td>
      <td>${formatValue(stats.humMin, 2)} %</td>
      <td>${formatValue(stats.humAvg, 2)} %</td>
      <td>${formatValue(stats.humMax, 2)} %</td>
    </tr>
  `;
}

function updateStatsTable(history) {
  const rows = [];

  rows.push(statsRow("Kitchen", history.node1Stats));
  rows.push(statsRow("Bedroom", history.node2Stats));

  document.getElementById("statsTable").innerHTML = rows.join("");
}

function generateReport() {
  window.print();
}

function drawChart(canvasId, history1, history2, field, unit) {
  const canvas = document.getElementById(canvasId);
  const ctx = canvas.getContext("2d");

  const w = canvas.width;
  const h = canvas.height;

  ctx.clearRect(0, 0, w, h);

  ctx.fillStyle = "#07101c";
  ctx.fillRect(0, 0, w, h);

  const allHistory = []
    .concat(history1 || [])
    .concat(history2 || []);

  const allValues = allHistory
    .map(p => p[field])
    .filter(v => typeof v === "number" && !isNaN(v));

  if (allValues.length < 1) {
    ctx.fillStyle = "#c9d6e6";
    ctx.font = "16px Arial";
    ctx.fillText("No measurements yet", 20, 42);
    return;
  }

  let min = Math.min(...allValues);
  let max = Math.max(...allValues);

  if (max - min < 1) {
    min -= 1;
    max += 1;
  }

  const left = 52;
  const right = 20;
  const top = 30;
  const bottom = 50;

  ctx.strokeStyle = "rgba(255,255,255,0.12)";
  ctx.lineWidth = 1;

  for (let i = 1; i < 5; i++) {
    const y = top + i * ((h - top - bottom) / 5);
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(w - right, y);
    ctx.stroke();
  }

  function xFor(i, len) {
    if (len <= 1) return left + (w - left - right) / 2;
    return left + i * ((w - left - right) / (len - 1));
  }

  function yFor(v) {
    return h - bottom - ((v - min) / (max - min)) * (h - top - bottom);
  }

  function drawLineAndPoints(history, color) {
    if (!history || history.length < 1) return;

    ctx.strokeStyle = color;
    ctx.lineWidth = 3;

    if (history.length >= 2) {
      ctx.beginPath();

      history.forEach((p, i) => {
        const x = xFor(i, history.length);
        const y = yFor(p[field]);

        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });

      ctx.stroke();
    }

    ctx.fillStyle = color;

    history.forEach((p, i) => {
      const x = xFor(i, history.length);
      const y = yFor(p[field]);

      ctx.beginPath();
      ctx.arc(x, y, 5, 0, Math.PI * 2);
      ctx.fill();
    });
  }

  // Kitchen = green, Bedroom = blue
  drawLineAndPoints(history1, "#7CFF9B");
  drawLineAndPoints(history2, "#66d9ff");

  // Legend
  ctx.fillStyle = "#7CFF9B";
  ctx.font = "13px Arial";
  ctx.fillText("Kitchen", left, 18);

  ctx.fillStyle = "#66d9ff";
  ctx.fillText("Bedroom", left + 90, 18);

  // Y-axis labels
  ctx.fillStyle = "#c9d6e6";
  ctx.font = "12px Arial";
  ctx.fillText(max.toFixed(1) + unit, 4, top + 5);
  ctx.fillText(min.toFixed(1) + unit, 4, h - bottom);

  // Timeline source
  const timelineSource =
    history1 && history1.length >= history2.length ? history1 :
    history2 && history2.length ? history2 :
    history1;

  if (timelineSource && timelineSource.length > 0) {
    const first = timelineSource[0];
    const last = timelineSource[timelineSource.length - 1];

    ctx.fillStyle = "#9fb0c7";
    ctx.font = "12px Arial";

    if (timelineSource.length === 1) {
      const x = left + (w - left - right) / 2 - 38;
      ctx.fillText(first.time, x, h - 26);
      ctx.fillText(first.date, x - 8, h - 10);
    } else {
      ctx.fillText(first.time, left, h - 26);
      ctx.fillText(first.date, left, h - 10);

      ctx.fillText(last.time, w - right - 58, h - 26);
      ctx.fillText(last.date, w - right - 74, h - 10);

      if (timelineSource.length >= 3) {
        const mid = timelineSource[Math.floor(timelineSource.length / 2)];
        const midX = w / 2 - 35;

        ctx.fillText(mid.time, midX, h - 26);
        ctx.fillText(mid.date, midX - 8, h - 10);
      }
    }
  }
}

async function updateDashboard() {
  try {
    const liveResponse = await fetch("/api/live");
    const live = await liveResponse.json();

    document.getElementById("currentTime").textContent = live.currentTime;
    document.getElementById("currentDate").textContent = live.currentDate;
    document.getElementById("timeout").textContent = live.dashboardTimeoutSeconds;
    document.getElementById("logSize").textContent = bytesToText(live.logFileSize);

    updateNode("n1", live.node1);
    updateNode("n2", live.node2);
    updateLatestTable(live);

    const historyResponse = await fetch("/api/history?limit=96");
    const history = await historyResponse.json();

    updateStatsTable(history);

    drawChart("tempChart", history.history1, history.history2, "temperature", "°C");
    drawChart("humChart", history.history1, history.history2, "humidity", "%");
  } catch (e) {
    console.log("Dashboard update failed", e);
  }
}

setInterval(updateDashboard, 5000);
updateDashboard();
</script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

// ---------- Dashboard control ----------
void configureRoutesOnce() {
  if (routesConfigured) return;

  server.on("/", handleRoot);
  server.on("/api", handleApi);
  server.on("/api/live", handleApiLive);
  server.on("/api/history", handleApiHistory);
  server.on("/download", handleDownload);
  server.on("/clear-log", handleClearLog);

  routesConfigured = true;
}

void startDashboard() {
  if (dashboardActive) {
    dashboardStartedAt = millis();
    oledOn();
    drawOLED();
    return;
  }

  Serial.println("Starting OLED + local Wi-Fi dashboard...");

  oledOn();

  WiFi.softAP(AP_SSID, AP_PASSWORD, WIFI_CHANNEL);
  delay(300);

  IPAddress ip = WiFi.softAPIP();

  Serial.print("Dashboard Wi-Fi started. SSID: ");
  Serial.println(AP_SSID);

  Serial.print("Password: ");
  Serial.println(AP_PASSWORD);

  Serial.print("Open browser: http://");
  Serial.println(ip);

  configureRoutesOnce();
  server.begin();

  dashboardActive = true;
  dashboardStartedAt = millis();

  drawOLED();
}

void stopDashboard() {
  if (!dashboardActive && !oledActive) return;

  Serial.println("Stopping OLED + local Wi-Fi dashboard...");

  server.stop();
  WiFi.softAPdisconnect(true);

  dashboardActive = false;
  oledOff();

  // Keep ESP-NOW receiver alive on correct channel
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.println("Dashboard off. ESP-NOW receiver still active.");
}

void checkDashboardTimeout() {
  if (!dashboardActive) return;

  if (millis() - dashboardStartedAt > DASHBOARD_ACTIVE_MS) {
    stopDashboard();
  }
}

void checkButton() {
  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonReading) {
    lastButtonChange = millis();
    lastButtonReading = reading;
  }

  if ((millis() - lastButtonChange) > debounceMs) {
    static bool handledPress = false;

    if (reading == LOW && !handledPress) {
      handledPress = true;
      Serial.println("Button pressed.");
      startDashboard();
    }

    if (reading == HIGH) {
      handledPress = false;
    }
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("Nordic Home IoT Main Hub V3");
  Serial.println("ESP32-S3 + OLED + RTC + ESP-NOW + LittleFS + Web");
  Serial.println("======================================");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // OLED I2C bus
  Wire.begin(OLED_SDA, OLED_SCL);

  // RTC I2C bus
  RTCWire.begin(RTC_SDA, RTC_SCL);

  // Initialize OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED failed!");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("OLED found at 0x3C");

  // Check RTC
  RTCWire.beginTransmission(DS3231_ADDRESS);
  if (RTCWire.endTransmission() == 0) {
    Serial.println("RTC DS3231 found at 0x68");
  } else {
    Serial.println("RTC DS3231 NOT found!");
  }

  // RTC is already set, so do NOT call setRtcTime().
  // setRtcTime();

  // Initialize flash storage
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
    Serial.println("Check Arduino partition scheme with SPIFFS/LittleFS.");
  } else {
    Serial.println("LittleFS mounted successfully.");
    Serial.print("LittleFS used bytes: ");
    Serial.println(LittleFS.usedBytes());
    Serial.print("LittleFS total bytes: ");
    Serial.println(LittleFS.totalBytes());
    ensureDataFile();
  }

  // Wi-Fi setup for ESP-NOW + AP mode support
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPdisconnect(true);
  WiFi.disconnect();

  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.print("Main hub STA MAC: ");
  Serial.println(WiFi.macAddress());

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
    while (true) {
      delay(1000);
    }
  }

  esp_now_register_recv_cb(onDataReceived);

  Serial.println("ESP-NOW receiver ready.");
  Serial.println("Normal mode:");
  Serial.println("- OLED off");
  Serial.println("- Wi-Fi dashboard off");
  Serial.println("- ESP-NOW receiver on");
  Serial.println("- readings saved to flash /data.csv");
  Serial.println();
  Serial.println("Press BOOT button to start OLED + dashboard.");
  Serial.println("Dashboard Wi-Fi: NordicHome");
  Serial.println("Password: nordichome123");
  Serial.println("URL: http://192.168.4.1");

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Nordic Home IoT");
  display.setCursor(0, 14);
  display.println("Main hub V3");
  display.setCursor(0, 28);
  display.println("Flash logging ON");
  display.setCursor(0, 42);
  display.println("Press BOOT");
  display.setCursor(0, 54);
  display.println("for dashboard");
  display.display();

  delay(3000);

  oledOff();
}

// ---------- Main loop ----------
void loop() {
  processReceiveQueue();

  checkButton();
  checkDashboardTimeout();

  if (dashboardActive) {
    server.handleClient();

    static unsigned long lastOledUpdate = 0;
    if (millis() - lastOledUpdate > 1000) {
      lastOledUpdate = millis();
      drawOLED();
    }
  }

  delay(10);
}
