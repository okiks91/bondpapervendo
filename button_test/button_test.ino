#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

// =======================================================
// WiFi Credentials
// =======================================================
const char* ssid     = "YOTC-329FD5";
const char* password = "MarcAron102705";

// =======================================================
// Active-HIGH Button Configuration
// =======================================================
const int BUTTON_PIN = 23; // Configured as INPUT_PULLDOWN (Normally 0V)
const int POWER_PIN  = 19; // Output pin providing 3.3V (Always HIGH)

WebServer server(80);

int buttonPressCount = 0;
int lastButtonState = LOW;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY_MS = 50;

// =======================================================
// Web Page HTML
// =======================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
  <title>Active-HIGH Button Tester</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: #0b0f19; color: #f8fafc; display: flex; flex-direction: column; align-items: center; padding: 30px 16px; min-height: 100vh; }
    .card { width: 100%; max-width: 380px; background: #161f30; border-radius: 24px; padding: 24px; border: 1px solid #283548; box-shadow: 0 10px 30px rgba(0,0,0,0.5); text-align: center; }
    h1 { font-size: 1.4rem; color: #38bdf8; margin-bottom: 4px; }
    .sub { font-size: 0.82rem; color: #94a3b8; margin-bottom: 24px; }

    .btn-visual {
      width: 180px;
      height: 180px;
      margin: 0 auto 24px auto;
      border-radius: 50%;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      font-size: 1.4rem;
      font-weight: 900;
      letter-spacing: 0.05em;
      transition: all 0.08s ease;
      box-shadow: 0 10px 25px rgba(0,0,0,0.4);
      user-select: none;
    }
    .state-released {
      background: #1e293b;
      color: #94a3b8;
      border: 6px solid #334155;
      transform: translateY(0);
    }
    .state-pressed {
      background: #10b981;
      color: #ffffff;
      border: 6px solid #34d399;
      transform: scale(0.94) translateY(4px);
      box-shadow: 0 0 35px rgba(16, 185, 129, 0.6);
    }
    .btn-visual span { font-size: 0.75rem; font-weight: 600; margin-top: 4px; opacity: 0.85; }

    .counter-card { background: #0d1524; border-radius: 14px; padding: 14px; border: 1px solid #1e2c42; margin-bottom: 16px; }
    .counter-val { font-size: 2.2rem; font-weight: 900; color: #38bdf8; font-family: monospace; }
    .counter-lbl { font-size: 0.72rem; color: #94a3b8; text-transform: uppercase; margin-top: 2px; }

    .wiring-box { font-size: 0.78rem; color: #94a3b8; line-height: 1.4; background: #0d1524; border-radius: 12px; padding: 12px; border: 1px solid #1e2c42; text-align: left; }
    .wiring-box b { color: #f8fafc; }
  </style>
</head>
<body>
  <div class="card">
    <h1>Active-HIGH Button Tester</h1>
    <div class="sub">Pin: GPIO 23 (INPUT_PULLDOWN)</div>

    <div id="btnVisual" class="btn-visual state-released">
      <div id="btnText">RELEASED</div>
      <span id="btnPinState">GPIO 23: 0V (LOW)</span>
    </div>

    <div class="counter-card">
      <div id="clickCount" class="counter-val">0</div>
      <div class="counter-lbl">Total Button Presses</div>
    </div>

    <div class="wiring-box">
      <b>Active-HIGH Wiring:</b><br>
      • Button Leg 1 &rarr; <b>GPIO 23</b><br>
      • Button Leg 2 &rarr; <b>GPIO 19</b> (or <b>3V3 pin</b>)<br>
      <i>GPIO 19 is permanently powered at 3.3V as the source!</i>
    </div>
  </div>

  <script>
    function updateStatus() {
      fetch('/status')
        .then(r => r.json())
        .then(d => {
          const btn = document.getElementById('btnVisual');
          const txt = document.getElementById('btnText');
          const sub = document.getElementById('btnPinState');
          document.getElementById('clickCount').innerText = d.clicks;

          if (d.pressed) {
            btn.className = 'btn-visual state-pressed';
            txt.innerText = 'PRESSED!';
            sub.innerText = 'GPIO 23: 3.3V (HIGH)';
          } else {
            btn.className = 'btn-visual state-released';
            txt.innerText = 'RELEASED';
            sub.innerText = 'GPIO 23: 0V (LOW)';
          }
        })
        .catch(() => {});
    }
    setInterval(updateStatus, 50);
  </script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  int rawState = digitalRead(BUTTON_PIN);
  bool isPressed = (rawState == HIGH); // Active-HIGH!

  String json = "{";
  json += "\"pressed\":" + String(isPressed ? "true" : "false") + ",";
  json += "\"clicks\":" + String(buttonPressCount);
  json += "}";

  server.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n==========================================");
  Serial.println(" Active-HIGH Button Tester                ");
  Serial.println(" Pin 23: INPUT_PULLDOWN                   ");
  Serial.println(" Pin 19: OUTPUT HIGH (3.3V Source)        ");
  Serial.println("==========================================");

  // Configure Button Pin with internal pull-DOWN
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);

  // Configure Pin 19 as always-HIGH 3.3V supply
  pinMode(POWER_PIN, OUTPUT);
  digitalWrite(POWER_PIN, HIGH);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 25) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected!");
    Serial.print("[WiFi] Live URL: http://");
    Serial.println(WiFi.localIP());
  }

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.begin();
  Serial.println("[Web] Server running.");
}

void loop() {
  server.handleClient();

  int reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY_MS) {
    static int debouncedState = LOW;
    if (reading != debouncedState) {
      debouncedState = reading;

      if (debouncedState == HIGH) { // Active-HIGH
        buttonPressCount++;
        Serial.println("------------------------------------------");
        Serial.printf(">>> [BUTTON PRESSED / CLICKED!] (#%d)\n", buttonPressCount);
        Serial.println("    Pin 23: Pulled to 3.3V (HIGH)");
        Serial.println("------------------------------------------");
      } else {
        Serial.println(">>> [BUTTON RELEASED] Pin 23: 0V (LOW)\n");
      }
    }
  }

  lastButtonState = reading;
}
