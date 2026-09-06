#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

// =======================================================
// WiFi Credentials
// =======================================================
const char* ssid     = "YOTC-329FD5";
const char* password = "v0jq634t";

// =======================================================
// IR Sensor Pin Configuration
// =======================================================
const int IR_PIN = 32; // ADC1 channel 4, supports analogRead & digitalRead while WiFi is active

// Pin Mode state: 0 = INPUT_PULLUP, 1 = INPUT (Floating), 2 = INPUT_PULLDOWN
int currentPinMode = 0; 

WebServer server(80);

// =======================================================
// Web Page HTML
// =======================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
  <title>IR Paper Sensor Tester</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: #0b0f19; color: #f8fafc; display: flex; flex-direction: column; align-items: center; padding: 24px 16px; min-height: 100vh; }
    .card { width: 100%; max-width: 420px; background: #161f30; border-radius: 20px; padding: 24px; border: 1px solid #283548; box-shadow: 0 10px 30px rgba(0,0,0,0.5); }
    h1 { font-size: 1.4rem; color: #38bdf8; text-align: center; margin-bottom: 4px; }
    .sub { font-size: 0.82rem; color: #94a3b8; text-align: center; margin-bottom: 20px; }

    /* Big Status Card */
    .status-display {
      border-radius: 16px;
      padding: 24px 16px;
      text-align: center;
      margin-bottom: 20px;
      transition: all 0.2s ease;
      background: #1e293b;
      border: 2px solid #334155;
    }
    .status-text { font-size: 1.8rem; font-weight: 900; letter-spacing: 0.03em; }
    .status-sub { font-size: 0.85rem; margin-top: 6px; opacity: 0.85; }

    /* Readings Grid */
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 20px; }
    .metric { background: #0d1524; border-radius: 12px; padding: 14px; text-align: center; border: 1px solid #1e2c42; }
    .metric-val { font-size: 1.6rem; font-weight: 800; color: #38bdf8; font-family: monospace; }
    .metric-lbl { font-size: 0.72rem; color: #94a3b8; text-transform: uppercase; margin-top: 4px; }

    /* Voltage Bar */
    .bar-container { background: #0d1524; border-radius: 8px; height: 16px; width: 100%; overflow: hidden; margin-bottom: 20px; border: 1px solid #1e2c42; }
    .bar-fill { height: 100%; width: 0%; background: linear-gradient(90deg, #0284c7, #38bdf8); transition: width 0.1s ease; }

    /* Pullup Mode Toggle */
    .mode-box { background: #0d1524; border-radius: 12px; padding: 14px; margin-bottom: 16px; border: 1px solid #1e2c42; }
    .mode-title { font-size: 0.8rem; color: #94a3b8; text-transform: uppercase; margin-bottom: 10px; font-weight: 700; text-align: center; }
    .mode-btns { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 8px; }
    .mode-btn { background: #1e293b; border: 1px solid #334155; color: #cbd5e1; border-radius: 8px; padding: 8px 4px; font-size: 0.75rem; font-weight: 600; cursor: pointer; }
    .mode-btn.active { background: #0284c7; color: white; border-color: #38bdf8; font-weight: 700; }

    .help-box { font-size: 0.78rem; color: #94a3b8; line-height: 1.4; background: #0d1524; border-radius: 10px; padding: 12px; }
    .help-box b { color: #f8fafc; }
  </style>
</head>
<body>
  <div class="card">
    <h1>IR Paper Sensor Live Test</h1>
    <div class="sub">Pin: ESP32 GPIO 32 • Real-time (100ms)</div>

    <!-- Live Status Box -->
    <div id="statusBox" class="status-display">
      <div id="statusText" class="status-text">WAITING...</div>
      <div id="statusSub" class="status-sub">Connecting to sensor...</div>
    </div>

    <!-- Digital & Analog Metrics -->
    <div class="grid">
      <div class="metric">
        <div id="digitalVal" class="metric-val">--</div>
        <div class="metric-lbl">Digital State (0 or 1)</div>
      </div>
      <div class="metric">
        <div id="analogVal" class="metric-val">--</div>
        <div class="metric-lbl">Analog Raw (0-4095)</div>
      </div>
    </div>

    <!-- Live Voltage Bar -->
    <div style="display: flex; justify-content: space-between; font-size: 0.75rem; color: #94a3b8; margin-bottom: 6px;">
      <span>0.00V</span>
      <span id="voltText" style="font-weight: 700; color: #38bdf8;">-- V</span>
      <span>3.30V</span>
    </div>
    <div class="bar-container">
      <div id="barFill" class="bar-fill"></div>
    </div>

    <!-- Internal Resistor Configuration -->
    <div class="mode-box">
      <div class="mode-title">ESP32 Pin Resistance Mode</div>
      <div class="mode-btns">
        <button id="btnPullup" class="mode-btn active" onclick="setMode('pullup')">PULL-UP<br>(Default)</button>
        <button id="btnFloat" class="mode-btn" onclick="setMode('floating')">FLOATING<br>(No Pull)</button>
        <button id="btnPulldown" class="mode-btn" onclick="setMode('pulldown')">PULL-DOWN<br>(To GND)</button>
      </div>
    </div>

    <div class="help-box">
      <b>How to test:</b><br>
      1. Slide a piece of bond paper into the sensor slot (or push the plastic lever).<br>
      2. Notice whether the <b>Digital State</b> changes from <b>0 to 1</b> (or 1 to 0).<br>
      3. If the numbers don't change, click <b>FLOATING</b> or <b>PULL-UP</b> above to match your sensor's circuit!
    </div>
  </div>

  <script>
    function setMode(m) {
      fetch('/set_mode?m=' + m)
        .then(r => r.text())
        .then(() => {
          document.querySelectorAll('.mode-btn').forEach(b => b.classList.remove('active'));
          if (m === 'pullup') document.getElementById('btnPullup').classList.add('active');
          else if (m === 'floating') document.getElementById('btnFloat').classList.add('active');
          else if (m === 'pulldown') document.getElementById('btnPulldown').classList.add('active');
        });
    }

    function readSensor() {
      fetch('/read')
        .then(r => r.json())
        .then(d => {
          document.getElementById('digitalVal').innerText = d.digital === 1 ? 'HIGH (1)' : 'LOW (0)';
          document.getElementById('analogVal').innerText = d.analog;
          document.getElementById('voltText').innerText = d.voltage.toFixed(2) + ' V';

          const pct = Math.min(100, Math.max(0, (d.voltage / 3.3) * 100));
          document.getElementById('barFill').style.width = pct + '%';

          const box = document.getElementById('statusBox');
          const txt = document.getElementById('statusText');
          const sub = document.getElementById('statusSub');

          if (d.digital === 1) {
            box.style.background = '#064e3b';
            box.style.borderColor = '#10b981';
            txt.style.color = '#6ee7b7';
            txt.innerText = 'HIGH (3.3V)';
            sub.innerText = 'Sensor output is HIGH';
          } else {
            box.style.background = '#1e1b4b';
            box.style.borderColor = '#6366f1';
            txt.style.color = '#a5b4fc';
            txt.innerText = 'LOW (0V)';
            sub.innerText = 'Sensor output is LOW';
          }
        })
        .catch(() => {});
    }

    // Refresh every 100ms for smooth live feedback
    setInterval(readSensor, 100);
  </script>
</body>
</html>
)rawliteral";

// =======================================================
// Web Endpoints
// =======================================================
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleRead() {
  int dig = digitalRead(IR_PIN);
  int raw = analogRead(IR_PIN);
  float volt = (raw / 4095.0f) * 3.3f;

  String json = "{";
  json += "\"digital\":" + String(dig) + ",";
  json += "\"analog\":" + String(raw) + ",";
  json += "\"voltage\":" + String(volt, 2);
  json += "}";

  server.send(200, "application/json", json);
}

void handleSetMode() {
  String m = server.arg("m");
  if (m == "pullup") {
    pinMode(IR_PIN, INPUT_PULLUP);
    currentPinMode = 0;
  } else if (m == "floating") {
    pinMode(IR_PIN, INPUT);
    currentPinMode = 1;
  } else if (m == "pulldown") {
    pinMode(IR_PIN, INPUT_PULLDOWN);
    currentPinMode = 2;
  }
  server.send(200, "text/plain", "OK");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n==========================================");
  Serial.println(" IR Paper Sensor Live Web Tester           ");
  Serial.println(" Pin: GPIO 32                              ");
  Serial.println("==========================================");

  // Default to internal pull-up
  pinMode(IR_PIN, INPUT_PULLUP);

  // Connect WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected!");
    Serial.print("[WiFi] Test URL: http://");
    Serial.println(WiFi.localIP());
  } else {
    WiFi.softAP("ESP32-IR-Test", "12345678");
    Serial.print("[WiFi AP] Test URL: http://");
    Serial.println(WiFi.softAPIP());
  }

  server.on("/", handleRoot);
  server.on("/read", handleRead);
  server.on("/set_mode", handleSetMode);
  server.begin();
  Serial.println("[Web] Server listening on port 80.");
}

unsigned long lastSerialPrintMs = 0;
int lastDigitalState = -1;

void loop() {
  server.handleClient();

  int dig = digitalRead(IR_PIN);
  int raw = analogRead(IR_PIN);
  float volt = (raw / 4095.0f) * 3.3f;
  unsigned long now = millis();

  // 1. Instant event print whenever paper is inserted or removed
  if (dig != lastDigitalState) {
    lastDigitalState = dig;
    Serial.println("\n**************************************************");
    if (dig == HIGH) {
      Serial.printf(">>> [EVENT] BEAM BLOCKED / PAPER DETECTED! (Digital: HIGH / 1, Volt: %.2fV)\n", volt);
    } else {
      Serial.printf(">>> [EVENT] BEAM CLEAR / NO PAPER!         (Digital: LOW / 0,  Volt: %.2fV)\n", volt);
    }
    Serial.println("**************************************************\n");
  }

  // 2. Continuous readout every 400ms
  if (now - lastSerialPrintMs >= 400) {
    lastSerialPrintMs = now;
    Serial.printf("[IR Sensor (GPIO 32)] Digital: %s (%d) | Analog: %4d / 4095 | Voltage: %.2f V\n",
                  (dig == HIGH ? "HIGH" : "LOW "), dig, raw, volt);
  }
}
