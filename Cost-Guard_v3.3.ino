/*
Changelog 1\2026\February 2
- First functional version.

Changelog 2\2026\May 28
- Added Objective 1: Threshold-based anomaly and phantom load detection
  * Spike Detection, Sustained Overcurrent, Phantom Load Detection

Changelog 3\2026\May 28
- Added Objective 2: Real-time cost estimation
  * Fetches live PHP/kWh from Meralco API (rairulyle/meralco-ph)
  * Computes cumulative cost and per-use cost in PHP
  * Caches last known rate in Preferences (NVS) for offline operation
  * Displays cost on OLED
  * Falls back to hardcoded default if API and cache both fail

Changelog 4\2026\May 29
- Added Objective 1: Anomaly & phantom load detection
  - Added Objective 2: Real-time cost estimation via Meralco API
  - Added Objective 3: LittleFS data logging + ESPAsyncWebServer + WebSocket
  - Includes EmonLib calibration and startup noise fixes

Changelog 5\2026\May 30
- Fix 1: Added MEDIAN_SAMPLES median filter for Vrms/Irms stability
- Fix 2: Hard-reject boundary on calcVI samples before buffering
- Fix 3: Decoupled WebSocket broadcast from sensor loop (timed interval)
- Fix 4: Explicit ADC_11db attenuation set at boot for ADC1 pins

Changelog 6\2026\June 1
- Added OLED Page 3: System Info (device name, version, dashboard IP)

Changelog 7\2026\June 30
- Thesis has been defended, code will be pushed to github for documentation purposes.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "EmonLib.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <ESPAsyncWebServer.h>

// ================================================================
// WIFI CONFIGURATION
// ================================================================
const char* WIFI_SSID     = "RJB iPhone";
const char* WIFI_PASSWORD = "Unit00707";

// NTP — Philippines = UTC+8
const char* NTP_SERVER      = "pool.ntp.org";
const long  GMT_OFFSET      = 28800;
const int   DAYLIGHT_OFFSET = 0;

// ================================================================
// MERALCO API CONFIGURATION
// Run the Docker container on your PC/laptop on the same network:
//   docker run -d -p 5000:5000 ghcr.io/rairulyle/meralco-ph:latest
// Then set API_HOST to that machine's local IP address.
// ================================================================
const char* API_HOST      = "http://172.20.10.8"; 
const int   API_PORT      = 5000;
const char* API_ENDPOINT  = "/rates/typical";
const unsigned long TARIFF_REFRESH_INTERVAL_MS = 86400000UL; // 24 hours
const float DEFAULT_FALLBACK_RATE = 13.8161; // PHP/kWh

// ================================================================
// HARDWARE PINS
// ================================================================
#define RELAY_PIN  26
#define VOLT_PIN   35
#define CURR_PIN   36
#define SDA_PIN    21
#define SCL_PIN    22

// ================================================================
// OLED
// ================================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================================================================
// OBJECT INSTANCES
// ================================================================
EnergyMonitor      emon;
Preferences        prefs;
AsyncWebServer     server(80);
AsyncWebSocket     ws("/ws");

// ================================================================
// THRESHOLDS
// ================================================================
const unsigned long STARTUP_GRACE_PERIOD     = 30000;
const float  OVERCURRENT_LIMIT               = 9.0;
const float  SPIKE_WATT_DELTA                = 4.0;
const float  SUSTAINED_CURRENT_THRESHOLD     = 6.0;
const unsigned long SUSTAINED_DURATION_MS    = 10000;
const float  PHANTOM_MIN_AMPS                = 0.20; // Widened from 0.005 to account for median smoothing
const float  PHANTOM_MAX_AMPS                = 0.50;  // Widened from 0.20 to account for median smoothing
const unsigned long PHANTOM_DURATION_MS      = 30000;

// ================================================================
// DATA LOGGING CONFIG
// ================================================================
const char* LOG_FILE           = "/log.csv";
const char* ALERT_FILE         = "/alerts.csv";
const int     LOG_INTERVAL_SEC   = 60;     // One row per minute
const int     MAX_LOG_ROWS       = 10080;  // 7 days @ 1 row/min
unsigned long lastLogMs          = 0;

// ================================================================
// FIX 1 & 2: MEDIAN FILTER FOR VRMS / IRMS STABILITY
// Absorbs 1-2 corrupt ADC samples caused by WiFi TX bursts.
// Hard-reject boundary discards obviously bad readings before
// they enter the buffer, preventing false spike alerts.
// ================================================================
#define MEDIAN_SAMPLES 5
float vrmsBuffer[MEDIAN_SAMPLES] = {0};
float irmsBuffer[MEDIAN_SAMPLES] = {0};
int   medianIndex = 0;

float computeMedian(float* buf, int n) {
  float sorted[n];
  memcpy(sorted, buf, n * sizeof(float));
  // Bubble sort (small n, fine for embedded)
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (sorted[j] > sorted[j + 1]) {
        float t = sorted[j];
        sorted[j] = sorted[j + 1];
        sorted[j + 1] = t;
      }
  return sorted[n / 2];
}

// ================================================================
// SYSTEM STATE
// ================================================================
bool  relayStatus    = true;
bool  tripped        = false;
bool  wifiConnected  = false;

// Sensor & Energy State
float latestVrms     = 0;
float latestIrms     = 0;
float latestWatts    = 0;
float accumulatedWh  = 0;
unsigned long lastEnergyMillis = 0;

// Session & Cost State
float sessionStartWh = 0;
float sessionCostPHP = 0;
float totalCostPHP   = 0;
float tariffRate     = 0.0;
bool  tariffValid    = false;
unsigned long lastTariffFetchMs = 0;

// Detection state
float         lastWatts          = 0;
float         lastRawWatts       = 0;  // Bug 1 fix: spike detection uses raw watts, not median
bool          sustainedFlag      = false;
unsigned long sustainedStartMs   = 0;
bool          sustainedAlertSent = false;
bool          phantomFlag        = false;
unsigned long phantomStartMs     = 0;
bool          phantomAlertSent   = false;
String        lastAlertMessage   = "None";
String        lastAlertType      = "none"; 

// OLED paging
int           displayPage        = 0;
unsigned long lastPageSwitchMs   = 0;
const unsigned long PAGE_INTERVAL = 4000;

// FIX 3: Timed WebSocket broadcast — decoupled from sensor loop
// Prevents broadcastLiveData() from firing mid-ADC-sample during
// WiFi TX bursts triggered by WebSocket client activity.
unsigned long lastBroadcastMs = 0;
const unsigned long BROADCAST_INTERVAL_MS = 2000; // broadcast every 2s

// ================================================================
// FUNCTION PROTOTYPES
// ================================================================
void connectWiFi();
void fetchTariffRate();
void initLogFiles();
void logDataRow();
void logAlert(String message, String type);
void triggerRelayTrip(String reason);
void runDetectionAlgorithms(float currentRawWatts);
void setupWebServer();
void broadcastLiveData();
void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len);
int  countFileRows(const char* path);
void trimLogFile(const char* path, int rowsToRemove);
String getTimestamp();
void showBootScreen(String msg);
void updateDisplayMetrics();
void updateDisplayCost();
void updateDisplaySysInfo();

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  // Relay ON by default
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED failed"));
    for (;;);
  }
  display.setTextColor(WHITE);
  showBootScreen("Starting...");

  // FIX 4: Set explicit ADC attenuation before EmonLib calibration.
  // Without this, the ESP32 ADC reference voltage is inconsistent,
  // contributing to erratic Vrms readings on VOLT_PIN and CURR_PIN.
  analogSetAttenuation(ADC_11db);
  analogSetPinAttenuation(VOLT_PIN, ADC_11db);
  analogSetPinAttenuation(CURR_PIN, ADC_11db);

  // Sensor calibration
  emon.voltage(VOLT_PIN, 132.7, 1.7);
  emon.current(CURR_PIN, 20.0);

  // Load cached tariff from NVS flash
  prefs.begin("costguard", false);
  float cached = prefs.getFloat("tariff", 0.0);
  if (cached > 0.0) {
    tariffRate = cached;
    tariffValid = true;
    Serial.print("Loaded cached tariff: PHP ");
    Serial.println(tariffRate, 4);
  } else {
    tariffRate = DEFAULT_FALLBACK_RATE;
    tariffValid = true;
    Serial.println("No cached tariff — using fallback rate.");
  }

  // Mount LittleFS
  showBootScreen("Mounting FS...");
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
    showBootScreen("FS ERROR!");
    delay(3000);
  } else {
    Serial.println("LittleFS mounted OK.");
    initLogFiles();
  }

  // Connect to WiFi
  showBootScreen("Connecting WiFi...");
  connectWiFi();

  if (wifiConnected) {
    // Sync NTP Time
    configTime(GMT_OFFSET, DAYLIGHT_OFFSET, NTP_SERVER);
    showBootScreen("Syncing time...");
    delay(2000);
    Serial.println("NTP time synced.");

    // Fetch Tariff from API
    showBootScreen("Fetching tariff...");
    fetchTariffRate();

    // Start web server
    showBootScreen("Starting server...");
    setupWebServer();
  }

  // Pre-charge EmonLib filter to clear ghost readings
  showBootScreen("Calibrating...");
  Serial.println("Pre-charging EmonLib filter...");
  for (int i = 0; i < 20; i++) {
    emon.calcVI(40, 2000);
    delay(50);
  }

  lastEnergyMillis = millis();
  lastLogMs        = millis();
  lastPageSwitchMs = millis();
  lastBroadcastMs  = millis();
  sessionStartWh   = 0;

  Serial.println("=== Cost-Guard v3.3 Ready ===");
}

// ================================================================
// MAIN LOOP
// ================================================================
void loop() {
  // FIX 1 & 2: Sample into median buffer with hard-reject boundary.
  // Readings outside the plausible range (e.g. 12-15V ghost spikes
  // caused by WiFi TX noise) are discarded before entering the buffer.
  // The median of MEDIAN_SAMPLES readings is used as the final value,
  // absorbing 1-2 corrupt samples per window without affecting accuracy.
  emon.calcVI(40, 2000);

  // Bug 1 fix: capture raw watts BEFORE median buffering so spike
  // detection sees the unsmoothed delta between loop iterations.
  float currentRawWatts = emon.Vrms * emon.Irms;

  if (emon.Vrms > 1.0 && emon.Vrms < 280.0 && emon.Irms < 50.0) {
    vrmsBuffer[medianIndex] = emon.Vrms;
    irmsBuffer[medianIndex] = emon.Irms;
  } else {
    // Corrupt sample — repeat last known good value in buffer slot
    vrmsBuffer[medianIndex] = (medianIndex > 0) ? vrmsBuffer[medianIndex - 1] : 0;
    irmsBuffer[medianIndex] = (medianIndex > 0) ? irmsBuffer[medianIndex - 1] : 0;
    Serial.printf("[FILTER] Rejected corrupt sample: V=%.2f I=%.3f\n", emon.Vrms, emon.Irms);
  }
  medianIndex = (medianIndex + 1) % MEDIAN_SAMPLES;

  latestVrms  = computeMedian(vrmsBuffer, MEDIAN_SAMPLES);
  latestIrms  = computeMedian(irmsBuffer, MEDIAN_SAMPLES);
  latestWatts = latestVrms * latestIrms;

  // --- Objective 1: Algorithms and Grace Period ---
  if (millis() > STARTUP_GRACE_PERIOD) {
    
    // Accumulate Energy (Ensures boot noise is ignored)
    unsigned long now = millis();
    float deltaHours  = (now - lastEnergyMillis) / 3600000.0;
    accumulatedWh    += latestWatts * deltaHours;
    lastEnergyMillis  = now;

    // Cost Calculation
    totalCostPHP = (accumulatedWh / 1000.0) * tariffRate;
    sessionCostPHP = ((accumulatedWh - sessionStartWh) / 1000.0) * tariffRate;

    // Run Safety Detections
    runDetectionAlgorithms(currentRawWatts);
  } else {
    // Keep relay on and timers updated during warmup
    if (!tripped) digitalWrite(RELAY_PIN, HIGH);
    lastEnergyMillis = millis(); 
    Serial.print("Warming up... ");
    Serial.println(millis());
  }

  lastWatts    = latestWatts;
  lastRawWatts = currentRawWatts;

  // --- Objective 2: Periodic Tariff Refresh ---
  if (wifiConnected && (millis() - lastTariffFetchMs >= TARIFF_REFRESH_INTERVAL_MS)) {
    fetchTariffRate();
  }

  // --- Objective 3: Data Logging & Web Server Broadcast ---
  if ((millis() - lastLogMs) >= (LOG_INTERVAL_SEC * 1000UL)) {
    logDataRow();
    lastLogMs = millis();
  }

  // FIX 3: Broadcast on a fixed 2-second interval instead of every
  // loop tick. This prevents WiFi TX bursts from coinciding with the
  // calcVI() ADC sampling window, which was causing the Vrms spikes.
  if (wifiConnected && (millis() - lastBroadcastMs >= BROADCAST_INTERVAL_MS)) {
    broadcastLiveData();
    ws.cleanupClients();
    lastBroadcastMs = millis();
  }

  // --- OLED Page Cycling ---
  if (millis() - lastPageSwitchMs >= PAGE_INTERVAL) {
    displayPage      = (displayPage + 1) % 3;
    lastPageSwitchMs = millis();
  }

  if (displayPage == 0)      updateDisplayMetrics();
  else if (displayPage == 1) updateDisplayCost();
  else                       updateDisplaySysInfo();

  delay(1000);
}

// ================================================================
// API TARIFF FETCH
// ================================================================
void fetchTariffRate() {
  if (WiFi.status() != WL_CONNECTED) return;

  String url = String(API_HOST) + ":" + String(API_PORT) + API_ENDPOINT;
  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000); 

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error && doc["success"]) {
      float newRate = doc["data"]["rate"].as<float>();
      if (newRate > 0.0) {
        tariffRate  = newRate;
        prefs.putFloat("tariff", tariffRate);
        lastTariffFetchMs = millis();
        Serial.print("Tariff updated: PHP ");
        Serial.println(tariffRate, 4);
      }
    }
  } else {
    Serial.print("HTTP error fetching API: ");
    Serial.println(httpCode);
  }
  http.end();
}

// ================================================================
// DETECTION ALGORITHMS
// ================================================================
void runDetectionAlgorithms(float currentRawWatts) {
  // 1. Hard Overcurrent
  if (latestIrms > OVERCURRENT_LIMIT && latestIrms < 50.0 && !tripped) {
    triggerRelayTrip("OVERCURRENT TRIP");
    sessionStartWh = accumulatedWh; // Reset session tracking
    return;
  }

  if (tripped) return;

  // 2. Spike Detection — uses raw (unmediated) watts so the median
  // filter does not absorb the delta before it can be compared.
  float delta = currentRawWatts - lastRawWatts;
  if (delta > SPIKE_WATT_DELTA) {
    logAlert("SPIKE:+" + String(delta, 0) + "W", "spike");
  }

  // 3. Sustained Overcurrent
  if (latestIrms > SUSTAINED_CURRENT_THRESHOLD) {
    if (!sustainedFlag) {
      sustainedFlag      = true;
      sustainedStartMs   = millis();
      sustainedAlertSent = false;
    } else if ((millis() - sustainedStartMs >= SUSTAINED_DURATION_MS) && !sustainedAlertSent) {
      logAlert("SUSTAINED:" + String(latestIrms, 2) + "A", "sustained");
      sustainedAlertSent = true;
    }
  } else {
    sustainedFlag      = false;
    sustainedAlertSent = false;
  }

  // 4. Phantom Load — re-alerts every PHANTOM_DURATION_MS while the
  // load persists, instead of going silent after the first alert.
  if (latestIrms >= PHANTOM_MIN_AMPS && latestIrms <= PHANTOM_MAX_AMPS) {
    if (!phantomFlag) {
      phantomFlag      = true;
      phantomStartMs   = millis();
      phantomAlertSent = false;
    } else if ((millis() - phantomStartMs >= PHANTOM_DURATION_MS) && !phantomAlertSent) {
      float phantomW = latestVrms * latestIrms;
      logAlert("PHANTOM:" + String(phantomW, 1) + "W", "phantom");
      // Bug 2 fix: reset timer and flag so it re-alerts while load persists
      phantomAlertSent = false;
      phantomStartMs   = millis();
    }
  } else {
    phantomFlag      = false;
    phantomAlertSent = false;
  }
}

// ================================================================
// LITTLEFS: INIT & LOGGING
// ================================================================
void initLogFiles() {
  if (!LittleFS.exists(LOG_FILE)) {
    File f = LittleFS.open(LOG_FILE, "w");
    if (f) {
      f.println("timestamp,vrms,irms,watts,kwh,cost_php,relay,alert");
      f.close();
    }
  }
  if (!LittleFS.exists(ALERT_FILE)) {
    File f = LittleFS.open(ALERT_FILE, "w");
    if (f) {
      f.println("timestamp,type,message");
      f.close();
    }
  }
}

void logDataRow() {
  if (countFileRows(LOG_FILE) >= MAX_LOG_ROWS) {
    trimLogFile(LOG_FILE, 100);
  }
  File f = LittleFS.open(LOG_FILE, "a");
  if (!f) return;
  f.printf("%s,%.2f,%.3f,%.1f,%.4f,%.4f,%d,%s\n",
    getTimestamp().c_str(), latestVrms, latestIrms, latestWatts,
    accumulatedWh / 1000.0, totalCostPHP, relayStatus ? 1 : 0, lastAlertMessage.c_str()
  );
  f.close();
}

void logAlert(String message, String type) {
  lastAlertMessage = message;
  lastAlertType    = type;
  File f = LittleFS.open(ALERT_FILE, "a");
  if (!f) return;
  f.printf("%s,%s,%s\n", getTimestamp().c_str(), type.c_str(), message.c_str());
  f.close();
}

int countFileRows(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  int count = 0;
  while (f.available()) { f.readStringUntil('\n'); count++; }
  f.close();
  return count;
}

void trimLogFile(const char* path, int rowsToRemove) {
  File src = LittleFS.open(path, "r");
  if (!src) return;
  String header = src.readStringUntil('\n');
  for (int i = 0; i < rowsToRemove && src.available(); i++) {
    src.readStringUntil('\n');
  }
  String remaining = "";
  while (src.available()) {
    remaining += src.readStringUntil('\n') + "\n";
  }
  src.close();
  File dst = LittleFS.open(path, "w");
  if (!dst) return;
  dst.println(header);
  dst.print(remaining);
  dst.close();
}

// ================================================================
// RELAY TRIP & WIFI SETUP
// ================================================================
void triggerRelayTrip(String reason) {
  digitalWrite(RELAY_PIN, LOW);
  relayStatus = false;
  tripped     = true;
  logAlert(reason, "overload");
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500); Serial.print("."); attempts++;
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);
}

// ================================================================
// WEB SERVER & WEBSOCKET ROUTING
// ================================================================
void setupWebServer() {
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<384> doc;
    doc["vrms"]      = latestVrms;
    doc["irms"]      = latestIrms;
    doc["watts"]     = latestWatts;
    doc["kwh"]       = accumulatedWh / 1000.0;
    doc["session"]   = sessionCostPHP;
    doc["cost"]      = totalCostPHP;
    doc["rate"]      = tariffRate;
    doc["relay"]     = relayStatus;
    doc["tripped"]   = tripped;
    doc["alert"]     = lastAlertMessage;
    doc["alertType"] = lastAlertType;
    doc["timestamp"] = getTimestamp();
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });

  server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (LittleFS.exists(LOG_FILE)) req->send(LittleFS, LOG_FILE, "text/csv");
    else req->send(404, "text/plain", "Log not found");
  });

  server.on("/api/alerts", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (LittleFS.exists(ALERT_FILE)) req->send(LittleFS, ALERT_FILE, "text/csv");
    else req->send(404, "text/plain", "Alerts not found");
  });

  server.on("/api/export", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (LittleFS.exists(LOG_FILE)) {
      AsyncWebServerResponse* resp = req->beginResponse(LittleFS, LOG_FILE, "text/csv");
      resp->addHeader("Content-Disposition", "attachment; filename=\"costguard_export.csv\"");
      req->send(resp);
    } else req->send(404, "text/plain", "No data to export");
  });

  server.on("/api/relay", HTTP_POST, [](AsyncWebServerRequest* req) {}, NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<64> doc;
      DeserializationError err = deserializeJson(doc, (char*)data);
      if (err) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }

      bool state = doc["state"];
      if (tripped && state) {
        tripped          = false;
        lastAlertMessage = "RESET via dashboard";
        lastAlertType    = "reset";
      }

      relayStatus = state;
      digitalWrite(RELAY_PIN, state ? HIGH : LOW);
      String response = "{\"success\":true,\"relay\":" + String(state ? "true" : "false") + "}";
      req->send(200, "application/json", response);
      broadcastLiveData();
    }
  );

  server.on("/api/relay", HTTP_OPTIONS, [](AsyncWebServerRequest* req) { req->send(200); });

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();
}

void broadcastLiveData() {
  if (ws.count() == 0) return;
  StaticJsonDocument<384> doc;
  doc["vrms"]      = latestVrms;
  doc["irms"]      = latestIrms;
  doc["watts"]     = latestWatts;
  doc["kwh"]       = accumulatedWh / 1000.0;
  doc["session"]   = sessionCostPHP;
  doc["cost"]      = totalCostPHP;
  doc["rate"]      = tariffRate;
  doc["relay"]     = relayStatus;
  doc["tripped"]   = tripped;
  doc["alert"]     = lastAlertMessage;
  doc["alertType"] = lastAlertType;
  doc["timestamp"] = getTimestamp();
  String json;
  serializeJson(doc, json);
  ws.textAll(json);
}

void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) broadcastLiveData(); 
}

String getTimestamp() {
  struct tm ti;
  if (getLocalTime(&ti)) {
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
    return String(buf);
  }
  return "T+" + String(millis() / 1000);
}

// ================================================================
// OLED DISPLAYS
// ================================================================
void showBootScreen(String msg) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);  display.println("=== Cost-Guard ===");
  display.drawFastHLine(0, 10, 128, WHITE);
  display.setCursor(0, 20); display.println(msg);
  display.display();
  delay(600);
}

void updateDisplayMetrics() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (tripped) display.print("!! TRIPPED-RESET !!");
  else if (millis() < STARTUP_GRACE_PERIOD) display.print("INITIALIZING...");
  else display.print("MONITORING  [1/3]");
  display.drawFastHLine(0, 10, 128, WHITE);

  display.setCursor(0, 13);
  display.print("V: "); display.print(latestVrms, 1); display.print(" VAC");
  display.setCursor(0, 24);
  display.print("I: "); display.print(latestIrms, 3); display.print(" A");
  display.setCursor(0, 35);
  display.print("P: "); display.print(latestWatts, 1); display.print(" W");
  display.setCursor(0, 46);
  display.print("E: "); display.print(accumulatedWh / 1000.0, 4); display.print(" kWh");

  display.drawFastHLine(0, 56, 128, WHITE);
  display.setCursor(0, 58);
  String a = lastAlertMessage;
  if (a.length() > 21) a = a.substring(0, 21);
  display.print(a);
  display.display();
}

void updateDisplayCost() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("COST MONITOR  [2/3]");
  display.drawFastHLine(0, 10, 128, WHITE);

  display.setCursor(0, 13);
  display.print("Rate: PHP "); display.print(tariffRate, 4);
  display.setCursor(0, 24);
  display.print("Session: PHP "); display.print(sessionCostPHP, 4);
  display.setCursor(0, 35);
  display.print("Total: PHP "); display.print(totalCostPHP, 4);
  display.setCursor(0, 46);
  display.print(wifiConnected ? WiFi.localIP().toString() : "No WiFi");

  display.drawFastHLine(0, 56, 128, WHITE);
  display.setCursor(0, 58);
  display.print(wifiConnected ? "WiFi:OK  WS:LIVE" : "WiFi:-- OFFLINE");
  display.display();
}

void updateDisplaySysInfo() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("SYSTEM INFO   [3/3]");
  display.drawFastHLine(0, 10, 128, WHITE);

  // Device name — large text for visibility
  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print("Cost-Guard");
  display.setTextSize(1);
  display.setCursor(0, 32);
  display.print("Version: v3.3");

  display.drawFastHLine(0, 43, 128, WHITE);

  // Dashboard IP
  display.setCursor(0, 46);
  display.print("Dashboard IP:");
  display.setCursor(0, 56);
  if (wifiConnected) {
    display.print(WiFi.localIP().toString());
  } else {
    display.print("Not connected");
  }

  display.display();
}
