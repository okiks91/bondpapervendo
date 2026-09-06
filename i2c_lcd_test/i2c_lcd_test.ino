#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>

// =======================================================
// WiFi Credentials
// =======================================================
const char* ssid     = "YOTC-329FD5";
const char* password = "v0jq634t";

// I2C Pins on ESP32
int activeSDA = 21;
int activeSCL = 22;

LiquidCrystal_I2C* lcd = nullptr;
uint8_t lcdAddress = 0x00;
bool lcdFound = false;

WebServer server(80);

uint8_t probeBus(int sda, int scl) {
  Wire.end();
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  delay(5);
  Wire.begin(sda, scl);
  Wire.setClock(100000);
  Wire.setTimeOut(10);

  const uint8_t candidates[] = {0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E};
  for (uint8_t addr : candidates) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C] Found at 0x%02X on SDA=%d, SCL=%d!\n", addr, sda, scl);
      return addr;
    }
  }

  for (uint8_t address = 1; address < 127; address++) {
    bool isCandidate = false;
    for (uint8_t c : candidates) {
      if (c == address) { isCandidate = true; break; }
    }
    if (isCandidate) continue;

    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C] Found at 0x%02X on SDA=%d, SCL=%d!\n", address, sda, scl);
      return address;
    }
  }
  return 0x00;
}

// Scan I2C bus for LCD address - tests D32/D33, D21/D22, and swapped pairs
uint8_t scanI2C() {
  // 1. Try D32 / D33 (Clean, unshorted pins)
  uint8_t addr = probeBus(32, 33);
  if (addr != 0) {
    activeSDA = 32;
    activeSCL = 33;
    return addr;
  }

  addr = probeBus(33, 32);
  if (addr != 0) {
    activeSDA = 33;
    activeSCL = 32;
    return addr;
  }

  // 2. Try D21 / D22
  addr = probeBus(21, 22);
  if (addr != 0) {
    activeSDA = 21;
    activeSCL = 22;
    return addr;
  }

  addr = probeBus(22, 21);
  if (addr != 0) {
    activeSDA = 22;
    activeSCL = 21;
    return addr;
  }

  return 0x00;
}

// =======================================================
// Web Page HTML
// =======================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
  <title>I2C LCD Display Tester</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: #0b0f19; color: #f8fafc; display: flex; flex-direction: column; align-items: center; padding: 24px 16px; min-height: 100vh; }
    .card { width: 100%; max-width: 420px; background: #161f30; border-radius: 24px; padding: 24px; border: 1px solid #283548; box-shadow: 0 10px 30px rgba(0,0,0,0.5); }
    h1 { font-size: 1.4rem; color: #38bdf8; text-align: center; margin-bottom: 4px; }
    .sub { font-size: 0.82rem; color: #94a3b8; text-align: center; margin-bottom: 20px; }

    /* LCD Screen Simulation */
    .lcd-frame {
      background: #064e3b;
      border: 6px solid #0f172a;
      border-radius: 12px;
      padding: 16px;
      margin-bottom: 20px;
      box-shadow: inset 0 0 15px rgba(0,0,0,0.6), 0 0 20px rgba(16, 185, 129, 0.2);
    }
    .lcd-text {
      font-family: 'Courier New', Courier, monospace;
      font-size: 1.25rem;
      font-weight: 900;
      color: #34d399;
      letter-spacing: 0.12em;
      line-height: 1.6;
      white-space: pre;
    }

    .btn-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 16px; }
    .btn {
      background: #0284c7;
      color: white;
      border: none;
      border-radius: 10px;
      padding: 12px;
      font-size: 0.85rem;
      font-weight: 700;
      cursor: pointer;
    }
    .btn:active { background: #38bdf8; }

    .custom-box { display: flex; gap: 8px; margin-bottom: 16px; }
    input {
      flex: 1;
      background: #0d1524;
      border: 1px solid #1e2c42;
      color: white;
      padding: 10px 14px;
      border-radius: 10px;
      font-size: 0.9rem;
    }

    .help-box { font-size: 0.78rem; color: #94a3b8; line-height: 1.4; background: #0d1524; border-radius: 12px; padding: 12px; border: 1px solid #1e2c42; }
    .help-box b { color: #f8fafc; }
  </style>
</head>
<body>
  <div class="card">
    <h1>I2C LCD Live Tester</h1>
    <div id="subText" class="sub">Scanning I2C Bus...</div>

    <!-- LCD Simulated Display -->
    <div class="lcd-frame">
      <div id="lcdLine1" class="lcd-text">BOND PAPER VENDO</div>
      <div id="lcdLine2" class="lcd-text">READY TO DISPENSE</div>
    </div>

    <!-- Scan Button -->
    <button class="btn" style="width:100%; margin-bottom:8px; background:#059669; padding: 14px; font-size: 0.95rem;" onclick="scanNow()">🔍 Scan / Re-Detect LCD</button>
    <div id="diagText" style="font-family:monospace; font-size:0.75rem; color:#38bdf8; text-align:center; margin-bottom:16px; line-height:1.4;"></div>

    <!-- Preset Test Messages -->
    <div class="btn-grid">
      <button class="btn" onclick="sendMsg('BOND PAPER VENDO', 'INSERT COINS')">Screen 1 (Standby)</button>
      <button class="btn" onclick="sendMsg('CREDIT: PHP 5.00', 'PRESS BUTTON (5)')">Screen 2 (Credit)</button>
      <button class="btn" onclick="sendMsg('DISPENSING PAPER', 'Remaining: 2')">Screen 3 (Dispense)</button>
      <button class="btn" onclick="sendMsg('OUT OF PAPER!', 'Please Refill')">Screen 4 (Empty)</button>
    </div>

    <!-- Custom Text -->
    <div class="custom-box">
      <input id="line1" type="text" maxlength="16" placeholder="Line 1 text...">
    </div>
    <div class="custom-box">
      <input id="line2" type="text" maxlength="16" placeholder="Line 2 text...">
      <button class="btn" onclick="sendCustom()">Send</button>
    </div>

    <div class="help-box">
      <b>Wiring Check:</b><br>
      • <b>VCC</b> &rarr; ESP32 <b>5V / VIN</b><br>
      • <b>GND</b> &rarr; ESP32 <b>GND</b><br>
      • <b>SDA</b> &rarr; ESP32 <b>GPIO 21</b><br>
      • <b>SCL</b> &rarr; ESP32 <b>GPIO 22</b><br>
      <i>If the screen is blank, turn the small blue potentiometer screw on the back to adjust contrast!</i>
    </div>
  </div>

  <script>
    function sendMsg(l1, l2) {
      document.getElementById('lcdLine1').innerText = l1;
      document.getElementById('lcdLine2').innerText = l2;
      fetch('/print?l1=' + encodeURIComponent(l1) + '&l2=' + encodeURIComponent(l2));
    }

    function sendCustom() {
      const l1 = document.getElementById('line1').value;
      const l2 = document.getElementById('line2').value;
      sendMsg(l1, l2);
    }

    function updateStatusUI(d) {
      if (d.found) {
        document.getElementById('subText').innerText = '✅ LCD Connected at Address: ' + d.addr;
        document.getElementById('subText').style.color = '#34d399';
      } else {
        document.getElementById('subText').innerText = '⏳ No LCD detected (Connect SDA: 21, SCL: 22, VCC: 5V, GND: GND)';
        document.getElementById('subText').style.color = '#f59e0b';
      }
    }

    function checkDiag() {
      fetch('/diag')
        .then(r => r.json())
        .then(d => {
          document.getElementById('diagText').innerText =
            'P21: ' + (d.raw21 ? 'HIGH' : 'LOW') +
            ' | P22: ' + (d.raw22 ? 'HIGH' : 'LOW') +
            ' | P32: ' + (d.raw32 ? 'HIGH' : 'LOW') +
            ' | P33: ' + (d.raw33 ? 'HIGH' : 'LOW') +
            '\nActive: SDA=' + d.activeSDA + ', SCL=' + d.activeSCL +
            ' | 0x27: err ' + d.err_0x27 + ' | 0x3F: err ' + d.err_0x3F;
        })
        .catch(() => {});
    }

    function scanNow() {
      document.getElementById('subText').innerText = '🔍 Scanning I2C bus...';
      fetch('/scan')
        .then(r => r.json())
        .then(d => {
          updateStatusUI(d);
          checkDiag();
        })
        .catch(() => {});
    }

    function checkStatus() {
      fetch('/status')
        .then(r => r.json())
        .then(updateStatusUI)
        .catch(() => {});
      checkDiag();
    }
    checkStatus();
    setInterval(checkStatus, 3000);
  </script>
</body>
</html>
)rawliteral";

void printToLCD(String l1, String l2) {
  if (!lcdFound || lcd == nullptr) return;

  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print(l1);
  lcd->setCursor(0, 1);
  lcd->print(l2);

  Serial.println("\n[LCD Display Updated]:");
  Serial.println("  Line 1: [" + l1 + "]");
  Serial.println("  Line 2: [" + l2 + "]");
}

void initLCD(uint8_t addr) {
  lcdAddress = addr;
  lcdFound = true;
  if (lcd != nullptr) {
    delete lcd;
  }
  lcd = new LiquidCrystal_I2C(lcdAddress, 16, 2);
  lcd->init();
  lcd->backlight();
  printToLCD("BOND PAPER VENDO", "READY TO DISPENSE");
  Serial.printf("[LCD] Initialized successfully at 0x%02X!\n", addr);
}

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handlePrint() {
  String l1 = server.hasArg("l1") ? server.arg("l1") : "";
  String l2 = server.hasArg("l2") ? server.arg("l2") : "";
  printToLCD(l1, l2);
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  String addrStr = "0x" + String(lcdAddress, HEX);
  addrStr.toUpperCase();

  String json = "{";
  json += "\"found\":" + String(lcdFound ? "true" : "false") + ",";
  json += "\"addr\":\"" + addrStr + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleDiag() {
  Wire.end();
  pinMode(21, INPUT_PULLUP);
  pinMode(22, INPUT_PULLUP);
  pinMode(32, INPUT_PULLUP);
  pinMode(33, INPUT_PULLUP);
  delay(5);
  int raw21 = digitalRead(21);
  int raw22 = digitalRead(22);
  int raw32 = digitalRead(32);
  int raw33 = digitalRead(33);

  Wire.begin(activeSDA, activeSCL);
  Wire.setClock(100000);
  Wire.setTimeOut(10);

  Wire.beginTransmission(0x27);
  uint8_t err27 = Wire.endTransmission();

  Wire.beginTransmission(0x3F);
  uint8_t err3F = Wire.endTransmission();

  String json = "{";
  json += "\"raw21\":" + String(raw21) + ",";
  json += "\"raw22\":" + String(raw22) + ",";
  json += "\"raw32\":" + String(raw32) + ",";
  json += "\"raw33\":" + String(raw33) + ",";
  json += "\"activeSDA\":" + String(activeSDA) + ",";
  json += "\"activeSCL\":" + String(activeSCL) + ",";
  json += "\"err_0x27\":" + String(err27) + ",";
  json += "\"err_0x3F\":" + String(err3F);
  json += "}";

  server.send(200, "application/json", json);
}

void handlePinScan() {
  Wire.end();
  const int testPins[] = {4, 5, 12, 13, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
  String json = "{";
  for (size_t i = 0; i < sizeof(testPins)/sizeof(testPins[0]); i++) {
    int p = testPins[i];
    pinMode(p, INPUT_PULLUP);
    delayMicroseconds(100);
    int valPU = digitalRead(p);

    pinMode(p, INPUT_PULLDOWN);
    delayMicroseconds(100);
    int valPD = digitalRead(p);

    if (i > 0) json += ",";
    json += "\"p" + String(p) + "\":{\"pu\":" + String(valPU) + ",\"pd\":" + String(valPD) + "}";
  }
  json += "}";
  server.send(200, "application/json", json);
}

void handleScan() {
  uint8_t addr = scanI2C();
  if (addr != 0x00) {
    initLCD(addr);
  }
  handleStatus();
}

unsigned long lastScanMs = 0;

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n==========================================");
  Serial.println(" I2C LCD Display Live Tester               ");
  Serial.println(" Auto-checks 21/22 and swapped 22/21       ");
  Serial.println("==========================================");

  uint8_t initialAddr = scanI2C();
  if (initialAddr != 0x00) {
    initLCD(initialAddr);
  } else {
    Serial.println("[LCD] No LCD backpack detected yet.");
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 25) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected!");
    Serial.print("[WiFi] Live Web Tester: http://");
    Serial.println(WiFi.localIP());
  }

  server.on("/", handleRoot);
  server.on("/print", handlePrint);
  server.on("/status", handleStatus);
  server.on("/scan", handleScan);
  server.on("/diag", handleDiag);
  server.on("/pinscan", handlePinScan);
  server.begin();
  Serial.println("[Web] Server running.");
}

void loop() {
  server.handleClient();

  if (!lcdFound && millis() - lastScanMs >= 3000) {
    lastScanMs = millis();
    uint8_t addr = scanI2C();
    if (addr != 0x00) {
      initLCD(addr);
    }
  }
}
