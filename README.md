# ⚡ Cost-Guard

**Real-time power monitoring, anomaly detection, and electricity cost tracking for the ESP32.**

Cost-Guard watches a live AC load through a voltage and current sensor, flags dangerous or wasteful usage patterns, estimates what that usage is actually costing you in Philippine Peso (using live Meralco tariff data), and serves everything through a local web dashboard — no cloud required.

> Developed as a thesis project. Defended June 2026, published here for documentation and reference purposes.

---

## ✨ Features

### 🛡️ Objective 1 — Anomaly & Phantom Load Detection
- **Hard Overcurrent Trip** — instantly cuts power via relay if current exceeds a safety limit
- **Spike Detection** — flags sudden jumps in wattage between samples
- **Sustained Overcurrent** — flags current that stays abnormally high for an extended period
- **Phantom Load Detection** — catches "vampire" standby draw and re-alerts periodically while it persists

### 💸 Objective 2 — Real-Time Cost Estimation
- Pulls the current PHP/kWh rate from a self-hosted [Meralco rates API](https://github.com/rairulyle/meralco-ph)
- Computes both **cumulative** and **per-session** cost in real time
- Caches the last known rate in flash (NVS) so it keeps working offline
- Falls back to a hardcoded default rate if the API and cache are both unavailable

### 📊 Objective 3 — Data Logging & Live Dashboard
- Logs readings to onboard flash (LittleFS) as CSV, once per minute
- Serves a live dashboard over WiFi via `ESPAsyncWebServer`
- Pushes real-time updates to connected clients over WebSocket
- REST endpoints for status, historical logs, alerts, and CSV export
- Remote relay control (reset a trip, or toggle the load) from the dashboard

### 🖥️ On-Device OLED Display
Cycles automatically through three info pages:
1. **Live Metrics** — voltage, current, power, cumulative energy, latest alert
2. **Cost Monitor** — tariff rate, session cost, total cost, WiFi/dashboard status
3. **System Info** — device name, firmware version, dashboard IP address

---

## 🔧 Hardware

| Component | Notes |
|---|---|
| ESP32 dev board | Core controller, WiFi, ADC |
| Voltage sensor (e.g. ZMPT101B) | Connected to `VOLT_PIN` |
| Current sensor (e.g. SCT-013 / ACS712) | Connected to `CURR_PIN` |
| Relay module | Load switching, connected to `RELAY_PIN` |
| SSD1306 OLED (128×64, I²C) | Status display |

### Pin Map

| Signal | GPIO |
|---|---|
| Relay | 26 |
| Voltage sensor | 35 |
| Current sensor | 36 |
| I²C SDA | 21 |
| I²C SCL | 22 |

> ⚠️ Sensor calibration constants (`emon.voltage(...)`, `emon.current(...)`) are tuned for the specific sensors and burden resistor used in this build. **Re-calibrate these values for your own hardware** — do not assume they'll be accurate out of the box.

---

## 📦 Dependencies

Install these via the Arduino Library Manager (or PlatformIO):

- [`Adafruit_GFX`](https://github.com/adafruit/Adafruit-GFX-Library)
- [`Adafruit_SSD1306`](https://github.com/adafruit/Adafruit_SSD1306)
- [`EmonLib`](https://github.com/openenergymonitor/EmonLib)
- [`ArduinoJson`](https://arduinojson.org/) (v6)
- [`ESPAsyncWebServer`](https://github.com/mathieucarbou/ESPAsyncWebServer) + `AsyncTCP`
- ESP32 board package (`Preferences`, `LittleFS`, `WiFi`, `HTTPClient` — bundled with Arduino-ESP32 core)

---

## 🌐 Meralco Rate API

Cost calculation relies on a small local API that serves the current Meralco PHP/kWh rate:

```bash
docker run -d -p 5000:5000 ghcr.io/rairulyle/meralco-ph:latest
```

Run this container on a PC/laptop on the same local network as the ESP32, then point the firmware at that machine's LAN IP (see [Configuration](#-configuration) below). If the API is unreachable, Cost-Guard automatically falls back to the last cached rate, or a hardcoded default if no cache exists.

---

## ⚙️ Configuration

Before flashing, edit the configuration block near the top of the sketch:

```cpp
// WiFi
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Meralco rate API — point this at the machine running the Docker container
const char* API_HOST = "http://192.168.x.x";
const int   API_PORT  = 5000;
```

**🔒 Security note:** Never commit real WiFi credentials or private network details to a public repository. Keep them in a local, gitignored file (e.g. `secrets.h`) if you plan to share this code.

Other tunables worth reviewing:

| Constant | Purpose | Default |
|---|---|---|
| `OVERCURRENT_LIMIT` | Hard trip threshold (A) | 9.0 |
| `SPIKE_WATT_DELTA` | Minimum jump to flag as a spike (W) | 4.0 |
| `SUSTAINED_CURRENT_THRESHOLD` | Current level considered "sustained high" (A) | 6.0 |
| `SUSTAINED_DURATION_MS` | How long before a sustained alert fires | 10 s |
| `PHANTOM_MIN_AMPS` / `PHANTOM_MAX_AMPS` | Standby-load current band (A) | 0.20 – 0.50 |
| `PHANTOM_DURATION_MS` | How long a phantom load persists before alerting | 30 s |
| `DEFAULT_FALLBACK_RATE` | Offline fallback rate (PHP/kWh) | 13.8161 |

---

## 🚀 Getting Started

1. Wire up the ESP32, voltage/current sensors, relay, and OLED per the pin map above.
2. Install the dependencies listed above.
3. Update the WiFi and API configuration constants.
4. Calibrate `emon.voltage()` and `emon.current()` for your specific sensors.
5. Flash the sketch to your ESP32.
6. On boot, the device will connect to WiFi, sync time via NTP, fetch the current tariff rate, and start the web dashboard.
7. Find the dashboard IP on the OLED's **System Info** page and open it in a browser on the same network.

---

## 📡 REST API

| Endpoint | Method | Description |
|---|---|---|
| `/api/status` | `GET` | Current snapshot: voltage, current, power, energy, cost, relay state, latest alert |
| `/api/log` | `GET` | Full CSV log of historical readings |
| `/api/alerts` | `GET` | CSV log of all triggered alerts |
| `/api/export` | `GET` | Download the log as a CSV file attachment |
| `/api/relay` | `POST` | Set relay state (`{"state": true/false}`), also clears a tripped state |
| `/ws` | WebSocket | Live push updates (~every 2 s) |

---

## 🧠 Notable Engineering Fixes

A few stability fixes are baked into this version, worth knowing about if you're adapting the code:

- **Median filtering** on Vrms/Irms (5-sample window) to smooth out ADC noise from WiFi TX bursts, with a hard-reject boundary that discards physically implausible readings before they enter the buffer.
- **Spike detection uses raw, unfiltered wattage** rather than the median-smoothed value, so genuine transient spikes aren't averaged away before they can be caught.
- **WebSocket broadcasts run on a fixed timer**, decoupled from the sensor sampling loop, to avoid ADC sampling colliding with WiFi transmission windows.
- **Explicit `ADC_11db` attenuation** is set at boot, since the ESP32's default ADC reference behavior was found to be inconsistent otherwise.

---

## 📁 Data Storage

Readings and alerts are logged to LittleFS as rolling CSV files:

- `/log.csv` — one row per minute, capped at 7 days of history (auto-trimmed)
- `/alerts.csv` — timestamped record of every triggered alert

---

## 📜 Version

**Cost-Guard v3.3** — see the changelog at the top of the `.ino` file for full version history.

## 🙏 Acknowledgments

- Live tariff data is powered by [**meralco-ph**](https://github.com/rairulyle/meralco-ph) by [**Lyle Vince Dela Cuesta**](https://github.com/rairulyle) — a REST API and Home Assistant add-on that parses and serves current MERALCO electricity rates in the Philippines. Huge thanks for making this available.
