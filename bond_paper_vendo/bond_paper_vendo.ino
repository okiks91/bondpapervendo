#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>

// =======================================================
// Pin Configuration
// =======================================================
const int COIN_PIN       = 18; // Allan Coin Selector pulse input (via 1N4007)
const int RPWM_PIN       = 27; // Forward speed (PWM) moved to D27!
const int LPWM_PIN       = 26; // Reverse speed (PWM) on D26
const int REN_PIN        = -1; // Unused (R_EN and L_EN jumpered to VCC on BTS7960)
const int BUTTON_PIN     = 23; // Physical Push Button (Active-HIGH with INPUT_PULLDOWN)
const int BUTTON_PWR_PIN = 19; // Provides 3.3V source for button (OUTPUT HIGH)
int activeSDA            = 32; // I2C Data line for LCD display (auto-detected)
int activeSCL            = 33; // I2C Clock line for LCD display (auto-detected)

// =======================================================
// WiFi & Web Server Configuration
// =======================================================
const char* WIFI_SSID = "YOTC-329FD5";
const char* WIFI_PASS = "MarcAron102705";
const char* AP_SSID   = "BondPaper-Vendo";
const char* AP_PASS   = "12345678";

WebServer server(80);
bool wifiConnected = false;
String localIPStr = "";

// =======================================================
// LCD Display Manager (Address 0x27, 16x2)
// =======================================================
LiquidCrystal_I2C lcd(0x27, 16, 2);
uint8_t activeLcdAddr = 0x27;
bool lcdReady = false;
String currentL1 = "";
String currentL2 = "";

// =======================================================
// Motor Speed Settings
// =======================================================
const int FORWARD_SPEED = 255; // 100% max speed (Full 24V)
const int REVERSE_SPEED = 128; // 50% speed (~12V equivalent PWM)

// =======================================================
// Calibrated Paper Dispense Cycle Timings
// =======================================================
const unsigned long REVERSE_TIME_MS      = 150;  // 0.15s reverse (cam engagement)
const unsigned long FORWARD_TIME_MS      = 1000; // 1.0s forward for intermediate sheets
const unsigned long LAST_FORWARD_TIME_MS = 1500; // 1.5s forward for the final sheet in queue
const unsigned long PAUSE_BETWEEN_MS     = 200;  // 0.20s pause between sheets

// =======================================================
// Anti-Noise Pulse Width Filter Variables
// =======================================================
const unsigned long MIN_PULSE_WIDTH_MS = 15;  // Ignore spikes < 15ms
const unsigned long MAX_PULSE_WIDTH_MS = 120; // Ignore pulses > 120ms
const unsigned long COIN_TIMEOUT_MS    = 450; // Finalize coin after 450ms of silence

volatile int pulseCount = 0;
volatile unsigned long fallTimestampMs = 0;
volatile unsigned long lastValidPulseMs = 0;

// =======================================================
// Dispenser State Machine & Variables
// =======================================================
enum DispenseState {
  STATE_IDLE,
  STATE_REVERSE,
  STATE_FORWARD,
  STATE_PAUSE
};

DispenseState currentState = STATE_IDLE;
unsigned long stateStartTime = 0;

// Credit & Queue Management
int insertedCredit = 0;       // Pesos / sheets waiting for user to press Dispense
int sheetsQueue = 0;          // Sheets currently being actively dispensed
int totalSheetsDispensed = 0; // Total count since power on
int totalEarningsPHP = 0;     // Total revenue
int paperRemaining = 50;      // Smart paper inventory tracker
const int TRAY_CAPACITY = 50; // Tray capacity

// Manual Web Motor Control
bool manualMotorActive = false;
unsigned long manualMotorStartMs = 0;
unsigned long manualMotorDuration = 0; // 0 = runs until stop (max 8s safety timeout), >0 = timed burst

// =======================================================
// Pulse Width Verification ISR (Rejects Electrical Noise)
// =======================================================
void IRAM_ATTR coinPinChangeISR() {
  unsigned long now = millis();
  int pinVal = digitalRead(COIN_PIN);

  if (pinVal == LOW) {
    fallTimestampMs = now;
  } else {
    if (fallTimestampMs > 0) {
      unsigned long pulseWidth = now - fallTimestampMs;
      fallTimestampMs = 0;

      if (pulseWidth >= MIN_PULSE_WIDTH_MS && pulseWidth <= MAX_PULSE_WIDTH_MS) {
        pulseCount++;
        lastValidPulseMs = now;
      }
    }
  }
}

// =======================================================
// Motor Hardware Functions
// =======================================================
void motorStop() {
  analogWrite(RPWM_PIN, 0);
  analogWrite(LPWM_PIN, 0);
  if (REN_PIN >= 0) digitalWrite(REN_PIN, LOW);
  manualMotorActive = false;
}

void motorForward(int speed) {
  if (REN_PIN >= 0) digitalWrite(REN_PIN, HIGH);
  analogWrite(RPWM_PIN, speed);
  analogWrite(LPWM_PIN, 0);
}

void motorReverse(int speed) {
  if (REN_PIN >= 0) digitalWrite(REN_PIN, HIGH);
  analogWrite(RPWM_PIN, 0);
  analogWrite(LPWM_PIN, speed);
}

// =======================================================
// I2C Bus Hardware Health Guard & Multi-Pin Auto-Detection
// =======================================================
bool isI2CBusHealthy() {
  if (!lcdReady || activeLcdAddr == 0) return false;
  Wire.beginTransmission(activeLcdAddr);
  return (Wire.endTransmission() == 0);
}

bool probeI2CPair(int sda, int scl, uint8_t &outAddr) {
  Wire.end();
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  delay(5);
  Wire.begin(sda, scl);
  Wire.setClock(100000);
  Wire.setTimeOut(30);

  const uint8_t candidates[] = {0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E};
  for (uint8_t a : candidates) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      outAddr = a;
      return true;
    }
  }
  return false;
}

bool detectAndInitLCD() {
  const int pairs[][2] = {
    {32, 33},
    {33, 32},
    {21, 22},
    {22, 21}
  };

  uint8_t foundAddr = 0;
  for (auto &pair : pairs) {
    int s = pair[0];
    int c = pair[1];
    if (probeI2CPair(s, c, foundAddr)) {
      activeSDA = s;
      activeSCL = c;
      activeLcdAddr = foundAddr;
      Serial.printf("[LCD] SUCCESS! LCD found at 0x%02X on SDA=%d, SCL=%d!\n", activeLcdAddr, activeSDA, activeSCL);

      lcd = LiquidCrystal_I2C(activeLcdAddr, 16, 2);
      lcd.init();
      Wire.begin(activeSDA, activeSCL);
      Wire.setClock(100000);
      Wire.setTimeOut(50);
      lcd.backlight();
      lcd.clear();
      lcdReady = true;
      return true;
    }
  }
  return false;
}

// =======================================================
// LCD Display Manager (Flicker-Free Text Buffer)
// =======================================================
void updateLCD(String l1, String l2, bool force = false) {
  if (!lcdReady) return;
  if (!force && l1 == currentL1 && l2 == currentL2) return;

  currentL1 = l1;
  currentL2 = l2;

  char line1Buf[17];
  char line2Buf[17];
  snprintf(line1Buf, sizeof(line1Buf), "%-16.16s", l1.c_str());
  snprintf(line2Buf, sizeof(line2Buf), "%-16.16s", l2.c_str());

  lcd.setCursor(0, 0);
  lcd.print(line1Buf);
  lcd.setCursor(0, 1);
  lcd.print(line2Buf);
}

void refreshLCDScreen() {
  if (!lcdReady) return;

  if (paperRemaining <= 0) {
    updateLCD(" OUT OF PAPER!  ", " PLEASE REFILL! ");
  } else if (sheetsQueue > 0) {
    char buf[17];
    snprintf(buf, sizeof(buf), "REMAINING: %-2d PCS", sheetsQueue);
    updateLCD("DISPENSING PAPER", String(buf));
  } else if (insertedCredit > 0) {
    char buf1[17];
    char buf2[17];
    snprintf(buf1, sizeof(buf1), "CREDIT: P%d.00", insertedCredit);
    snprintf(buf2, sizeof(buf2), "PRESS BUTTON: %-2d", insertedCredit);
    updateLCD(String(buf1), String(buf2));
  } else {
    char buf[17];
    snprintf(buf, sizeof(buf), "INSERT COIN (%d)", paperRemaining);
    updateLCD(String(buf), " 1P = 1 BOND PC ");
  }
}

// =======================================================
// Dispense & Refill Actions
// =======================================================
void triggerDispense() {
  if (insertedCredit > 0 && sheetsQueue == 0 && paperRemaining > 0) {
    sheetsQueue = insertedCredit;
    Serial.printf("[Vendo] Dispense triggered! Queued %d sheets.\n", sheetsQueue);
    insertedCredit = 0;
    refreshLCDScreen();
  } else if (insertedCredit == 0 && sheetsQueue == 0) {
    Serial.println("[Vendo] Button pressed, but no credit inserted.");
  }
}

void refillPaper() {
  paperRemaining = TRAY_CAPACITY;
  Serial.printf("[Vendo] Paper refilled to %d sheets.\n", paperRemaining);
  updateLCD("PAPER REFILLED! ", " TRAY: 50 PCS   ", true);
  refreshLCDScreen();
}

// =======================================================
// Live Pin Status Diagnostic Function
// =======================================================
void printPinStatus() {
  Serial.println("\n========== LIVE ESP32 PIN STATUS ==========");
  int coinVal   = digitalRead(COIN_PIN);
  int btnVal    = digitalRead(BUTTON_PIN);
  int btnPwrVal = digitalRead(BUTTON_PWR_PIN);
  int rpwmVal   = digitalRead(RPWM_PIN);
  int lpwmVal   = digitalRead(LPWM_PIN);
  int sdaVal    = digitalRead(activeSDA);
  int sclVal    = digitalRead(activeSCL);

  Serial.printf("  COIN_PIN       (GPIO %2d): %s (%s)\n", COIN_PIN, coinVal ? "HIGH" : "LOW ", coinVal ? "Idle, 3.3V via 1N4007 OK" : "LOW (Pulled down / Active Pulse)");
  Serial.printf("  BUTTON_PIN     (GPIO %2d): %s (%s)\n", BUTTON_PIN, btnVal ? "HIGH" : "LOW ", btnVal ? "PRESSED / Active-HIGH" : "Released (Pulled Down OK)");
  Serial.printf("  BUTTON_PWR_PIN (GPIO %2d): %s (%s)\n", BUTTON_PWR_PIN, btnPwrVal ? "HIGH" : "LOW ", btnPwrVal ? "3.3V Power Source Active" : "LOW (Warning: Off!)");
  Serial.printf("  RPWM_PIN       (GPIO %2d): %s (Motor Forward PWM - D27)\n", RPWM_PIN, rpwmVal ? "HIGH" : "LOW ");
  Serial.printf("  LPWM_PIN       (GPIO %2d): %s (Motor Reverse PWM - D26)\n", LPWM_PIN, lpwmVal ? "HIGH" : "LOW ");
  Serial.printf("  I2C_SDA_PIN    (GPIO %2d): %s (%s)\n", activeSDA, sdaVal ? "HIGH" : "LOW ", sdaVal ? "3.3V Pullup OK" : "LOW (Grounded / Shorted)");
  Serial.printf("  I2C_SCL_PIN    (GPIO %2d): %s (%s)\n", activeSCL, sclVal ? "HIGH" : "LOW ", sclVal ? "3.3V Pullup OK" : "LOW (Grounded / Shorted)");
  Serial.printf("  LCD Display                 : %s (Addr: 0x%02X, SDA=%d, SCL=%d)\n", lcdReady ? "ACTIVE / DISPLAYING" : "NOT DETECTED", activeLcdAddr, activeSDA, activeSCL);
  Serial.printf("  WiFi Status                 : %s (IP: %s)\n", wifiConnected ? "CONNECTED" : "AP HOTSPOT", localIPStr.c_str());
  Serial.println("===========================================\n");
}

void testBtsPins() {
  Serial.println("\n========== TESTING BTS7960 OUTPUT PINS ==========");
  pinMode(RPWM_PIN, OUTPUT);
  digitalWrite(RPWM_PIN, HIGH);
  delay(20);
  int rpwmState = digitalRead(RPWM_PIN);
  Serial.printf("  1. RPWM_PIN (GPIO %2d - D27): %s -> %s\n", RPWM_PIN, rpwmState ? "HIGH (3.3V)" : "LOW (0V / Clamped)", rpwmState ? "HEALTHY (Outputs 3.3V OK)" : "FAULT (Shorted to GND / Overloaded)");
  digitalWrite(RPWM_PIN, LOW);

  pinMode(LPWM_PIN, OUTPUT);
  digitalWrite(LPWM_PIN, HIGH);
  delay(20);
  int lpwmState = digitalRead(LPWM_PIN);
  Serial.printf("  2. LPWM_PIN (GPIO %2d - D26): %s -> %s\n", LPWM_PIN, lpwmState ? "HIGH (3.3V)" : "LOW (0V / Clamped)", lpwmState ? "HEALTHY (Outputs 3.3V OK)" : "FAULT (Shorted to GND / Overloaded)");
  digitalWrite(LPWM_PIN, LOW);

  motorStop();
  Serial.println("================================================\n");
}

// =======================================================
// Web Dashboard HTML (Dark Mode, Responsive, Real-Time)
// =======================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Bond Paper Vendo Control</title>
  <style>
    :root {
      --bg: #0f172a;
      --card: #1e293b;
      --border: #334155;
      --text: #f8fafc;
      --muted: #94a3b8;
      --green: #22c55e;
      --blue: #3b82f6;
      --red: #ef4444;
      --yellow: #f59e0b;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: var(--bg); color: var(--text); padding: 16px; display: flex; justify-content: center; }
    .container { width: 100%; max-width: 580px; display: flex; flex-direction: column; gap: 16px; }
    
    header { background: var(--card); border: 1px solid var(--border); border-radius: 14px; padding: 18px; display: flex; justify-content: space-between; align-items: center; }
    .title h1 { font-size: 1.25rem; font-weight: 700; color: #fff; }
    .title p { font-size: 0.82rem; color: var(--muted); margin-top: 2px; }
    .badge { font-size: 0.75rem; padding: 4px 10px; border-radius: 9999px; background: rgba(34, 197, 94, 0.2); color: var(--green); border: 1px solid var(--green); font-weight: 600; }

    .grid-stats { display: grid; grid-template-columns: repeat(2, 1fr); gap: 12px; }
    .stat-card { background: var(--card); border: 1px solid var(--border); border-radius: 12px; padding: 14px; }
    .stat-label { font-size: 0.75rem; color: var(--muted); text-transform: uppercase; letter-spacing: 0.5px; }
    .stat-val { font-size: 1.5rem; font-weight: 700; color: #fff; margin-top: 4px; }

    .card { background: var(--card); border: 1px solid var(--border); border-radius: 14px; padding: 18px; }
    .card h2 { font-size: 0.95rem; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; margin-bottom: 14px; color: var(--muted); }

    .btn-group { display: flex; flex-direction: column; gap: 10px; }
    .btn-row { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
    
    button {
      border: none;
      border-radius: 10px;
      padding: 15px;
      font-size: 1rem;
      font-weight: 700;
      color: #fff;
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      transition: filter 0.15s, transform 0.1s;
      user-select: none;
      -webkit-user-select: none;
    }
    button:active { transform: scale(0.98); filter: brightness(1.2); }
    
    .btn-fwd { background: var(--green); }
    .btn-rev { background: var(--blue); }
    .btn-stop { background: var(--red); grid-column: span 2; padding: 16px; font-size: 1.1rem; }
    .btn-dispense { background: linear-gradient(135deg, #3b82f6, #8b5cf6); padding: 16px; font-size: 1.1rem; }
    .btn-credit { background: #334155; }
    .btn-refill { background: #475569; }

    .pulse-btn { background: #1e293b; border: 1px solid var(--border); color: #cbd5e1; padding: 10px; font-size: 0.85rem; }
    .pulse-btn:hover { background: #334155; }

    footer { text-align: center; font-size: 0.75rem; color: var(--muted); margin-top: 10px; }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div class="title">
        <h1>Bond Paper Vendo</h1>
        <p id="ip-display">Connecting...</p>
      </div>
      <div class="badge" id="conn-badge">ONLINE</div>
    </header>

    <div class="grid-stats">
      <div class="stat-card">
        <div class="stat-label">Paper Tray</div>
        <div class="stat-val" id="stat-paper">-- / 50</div>
      </div>
      <div class="stat-card">
        <div class="stat-label">Credit (PHP)</div>
        <div class="stat-val" style="color: var(--green);" id="stat-credit">P0.00</div>
      </div>
      <div class="stat-card">
        <div class="stat-label">Dispense Queue</div>
        <div class="stat-val" style="color: var(--yellow);" id="stat-queue">0</div>
      </div>
      <div class="stat-card">
        <div class="stat-label">Motor State</div>
        <div class="stat-val" style="font-size: 1.1rem; margin-top: 8px;" id="stat-state">IDLE</div>
      </div>
    </div>

    <!-- Manual Motor Controls -->
    <div class="card">
      <h2>Manual Motor Control (24V BTS7960)</h2>
      <div class="btn-group">
        <div class="btn-row">
          <button class="btn-fwd" onclick="sendAction('forward')">
            &#9654; FORWARD (D27)
          </button>
          <button class="btn-rev" onclick="sendAction('reverse')">
            &#9664; REVERSE (D26)
          </button>
        </div>
        <button class="btn-stop" onclick="sendAction('stop')">
          &#9632; EMERGENCY STOP
        </button>
        <div class="btn-row" style="margin-top: 4px;">
          <button class="pulse-btn" onclick="sendAction('pulse_forward')">&#9654; Pulse Fwd 1.5s</button>
          <button class="pulse-btn" onclick="sendAction('pulse_reverse')">&#9664; Pulse Rev 0.5s</button>
        </div>
      </div>
    </div>

    <!-- Vendo Actions -->
    <div class="card">
      <h2>Vendo Actions</h2>
      <div class="btn-group">
        <button class="btn-dispense" onclick="sendAction('dispense')">
          &#128196; DISPENSE 1 SHEET NOW
        </button>
        <div class="btn-row">
          <button class="btn-credit" onclick="addCredit(1)">+ P1.00</button>
          <button class="btn-credit" onclick="addCredit(5)">+ P5.00</button>
        </div>
        <button class="btn-refill" onclick="sendAction('refill')">
          &#128230; Refill Paper Tray (50 Pcs)
        </button>
      </div>
    </div>

    <footer>
      Standalone Bond Paper Vendo &bull; ESP32 &bull; BTS7960 24V (RPWM=D27, LPWM=D26)
    </footer>
  </div>

  <script>
    function sendAction(action) {
      fetch('/api/motor?action=' + encodeURIComponent(action), { method: 'POST' })
        .then(r => r.json())
        .then(d => updateUI(d))
        .catch(e => console.error(e));
    }

    function addCredit(amt) {
      fetch('/api/motor?action=add_credit&amount=' + amt, { method: 'POST' })
        .then(r => r.json())
        .then(d => updateUI(d))
        .catch(e => console.error(e));
    }

    function pollStatus() {
      fetch('/api/status')
        .then(r => r.json())
        .then(d => updateUI(d))
        .catch(e => {
          document.getElementById('conn-badge').textContent = 'OFFLINE';
          document.getElementById('conn-badge').style.color = '#ef4444';
          document.getElementById('conn-badge').style.borderColor = '#ef4444';
        });
    }

    function updateUI(d) {
      if (!d) return;
      document.getElementById('conn-badge').textContent = 'ONLINE';
      document.getElementById('conn-badge').style.color = 'var(--green)';
      document.getElementById('conn-badge').style.borderColor = 'var(--green)';
      document.getElementById('ip-display').textContent = (d.wifi || 'WiFi') + ' • ' + (d.ip || '');
      document.getElementById('stat-paper').textContent = d.paper + ' / 50';
      document.getElementById('stat-credit').textContent = 'P' + d.credit + '.00';
      document.getElementById('stat-queue').textContent = d.queue + ' pcs';
      
      let st = d.state;
      if (d.manual && d.manual !== '') st = 'MANUAL ' + d.manual;
      document.getElementById('stat-state').textContent = st;
      document.getElementById('stat-state').style.color = (st === 'IDLE') ? '#fff' : 'var(--green)';
    }

    setInterval(pollStatus, 1000);
    pollStatus();
  </script>
</body>
</html>
)rawliteral";

// =======================================================
// Web Server Route Handlers
// =======================================================
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  String stateStr = "IDLE";
  if (currentState == STATE_REVERSE) stateStr = "REVERSING";
  else if (currentState == STATE_FORWARD) stateStr = "FORWARDING";
  else if (currentState == STATE_PAUSE) stateStr = "PAUSING";

  String manualStr = "";
  if (manualMotorActive) {
    if (digitalRead(RPWM_PIN) == HIGH) manualStr = "FWD 100%";
    else if (digitalRead(LPWM_PIN) == HIGH) manualStr = "REV 100%";
  }

  String json = "{";
  json += "\"credit\":" + String(insertedCredit) + ",";
  json += "\"queue\":" + String(sheetsQueue) + ",";
  json += "\"paper\":" + String(paperRemaining) + ",";
  json += "\"dispensed\":" + String(totalSheetsDispensed) + ",";
  json += "\"earnings\":" + String(totalEarningsPHP) + ",";
  json += "\"state\":\"" + stateStr + "\",";
  json += "\"manual\":\"" + manualStr + "\",";
  json += "\"ip\":\"" + localIPStr + "\",";
  json += "\"wifi\":\"" + String(wifiConnected ? WIFI_SSID : AP_SSID) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleMotor() {
  String action = server.hasArg("action") ? server.arg("action") : "";
  Serial.printf("[WebAPI] Action: %s\n", action.c_str());

  if (action == "forward") {
    motorForward(FORWARD_SPEED);
    manualMotorActive = true;
    manualMotorStartMs = millis();
    manualMotorDuration = 0; // runs until stopped or 8s safety timeout
  } else if (action == "reverse") {
    motorReverse(REVERSE_SPEED);
    manualMotorActive = true;
    manualMotorStartMs = millis();
    manualMotorDuration = 0;
  } else if (action == "stop") {
    motorStop();
    manualMotorActive = false;
  } else if (action == "pulse_forward") {
    motorForward(FORWARD_SPEED);
    manualMotorActive = true;
    manualMotorStartMs = millis();
    manualMotorDuration = 1500; // 1.5s burst
  } else if (action == "pulse_reverse") {
    motorReverse(REVERSE_SPEED);
    manualMotorActive = true;
    manualMotorStartMs = millis();
    manualMotorDuration = 500; // 0.5s burst
  } else if (action == "dispense") {
    if (paperRemaining > 0 && sheetsQueue == 0) {
      sheetsQueue = 1;
      Serial.println("[WebAPI] Dispense 1 sheet initiated.");
      refreshLCDScreen();
    }
  } else if (action == "add_credit") {
    int amt = server.hasArg("amount") ? server.arg("amount").toInt() : 1;
    insertedCredit += amt;
    totalEarningsPHP += amt;
    refreshLCDScreen();
  } else if (action == "refill") {
    refillPaper();
  }

  handleStatus();
}

// =======================================================
// Setup
// =======================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n==========================================");
  Serial.println(" Bond Paper Vendo Machine (Connected)     ");
  Serial.println(" Allan Slot + BTS7960 (D27/D26) + LCD     ");
  Serial.println("==========================================");

  // Motor pins
  pinMode(RPWM_PIN, OUTPUT);
  pinMode(LPWM_PIN, OUTPUT);
  if (REN_PIN >= 0) pinMode(REN_PIN, OUTPUT);
  motorStop();

  // Physical Push Button: Active-HIGH with INPUT_PULLDOWN
  pinMode(BUTTON_PIN, INPUT_PULLDOWN);
  // Source pin providing 3.3V to button
  pinMode(BUTTON_PWR_PIN, OUTPUT);
  digitalWrite(BUTTON_PWR_PIN, HIGH);

  // Coin Selector with noise filter
  pinMode(COIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COIN_PIN), coinPinChangeISR, CHANGE);

  // Initialize I2C with internal pullups
  pinMode(activeSDA, INPUT_PULLUP);
  pinMode(activeSCL, INPUT_PULLUP);
  delay(10);

  // Print all live pin statuses
  printPinStatus();
  testBtsPins();

  if (detectAndInitLCD()) {
    updateLCD("STARTING VENDO..", "CONNECTING WIFI ", true);
  } else {
    Serial.println("[LCD] Not detected on standard pin pairs. Background auto-scan active.");
  }

  // =======================================================
  // WiFi Connection with Auto Fallback Hotspot
  // =======================================================
  Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long wifiStartMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStartMs < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    localIPStr = WiFi.localIP().toString();
    Serial.printf("[WiFi] CONNECTED! IP Address: http://%s\n", localIPStr.c_str());
    updateLCD("WIFI CONNECTED! ", "IP:" + localIPStr, true);
    delay(2000);
  } else {
    wifiConnected = false;
    Serial.printf("[WiFi] Router not reached (status: %d). Starting Fallback Hotspot (AP)...\n", WiFi.status());
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    localIPStr = WiFi.softAPIP().toString();
    Serial.printf("[WiFi AP] Hotspot '%s' active. Open http://%s\n", AP_SSID, localIPStr.c_str());
    updateLCD("AP:" + String(AP_SSID).substring(0, 13), "IP:" + localIPStr, true);
    delay(2000);
  }

  // Start Web Server
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/motor", HTTP_ANY, handleMotor);
  server.begin();
  Serial.println("[WebServer] HTTP server started on port 80.");

  refreshLCDScreen();
  Serial.println("[Vendo] System initialized and ready for coins & web commands.");
}

// =======================================================
// Main Loop
// =======================================================
void loop() {
  // 0. Handle Web Server Requests
  server.handleClient();

  // Safety timer for manual motor control
  if (manualMotorActive) {
    unsigned long elapsed = millis() - manualMotorStartMs;
    if (manualMotorDuration > 0 && elapsed >= manualMotorDuration) {
      motorStop();
    } else if (manualMotorDuration == 0 && elapsed >= 8000) {
      Serial.println("[Safety] Manual motor timeout (8s). Stopping.");
      motorStop();
    }
  }

  // Check if background WiFi STA connected or disconnected
  if (WiFi.status() == WL_CONNECTED && !wifiConnected) {
    wifiConnected = true;
    localIPStr = WiFi.localIP().toString();
    Serial.printf("\n[WiFi] Connected to %s! IP Address: http://%s\n", WIFI_SSID, localIPStr.c_str());
    updateLCD("WIFI CONNECTED! ", "IP:" + localIPStr, true);
    delay(1000);
    refreshLCDScreen();
  } else if (WiFi.status() != WL_CONNECTED && wifiConnected) {
    wifiConnected = false;
    localIPStr = WiFi.softAPIP().toString();
    Serial.println("\n[WiFi] Connection lost. Fallback to AP active.");
  }

  // Check for Serial status query commands
  if (Serial.available() > 0) {
    char c = Serial.read();
    if (c == 'p' || c == 's' || c == '?') {
      printPinStatus();
    } else if (c == 'w') {
      Serial.println("\n[WiFi] Scanning nearby networks...");
      int n = WiFi.scanNetworks();
      Serial.printf("[WiFi] Found %d networks:\n", n);
      for (int i = 0; i < n; ++i) {
        Serial.printf("  %2d: %-32.32s (%4d dBm) %s\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "ENCRYPTED");
      }
      Serial.printf("  Current STA status: %d (Connected: %s, IP: %s)\n\n", WiFi.status(), wifiConnected ? "YES" : "NO", localIPStr.c_str());
    } else if (c == 't') {
      testBtsPins();
    } else if (c == 'f') {
      Serial.println("[Motor Test] Running FORWARD at 100% (24V on D27) for 1.5s...");
      motorForward(FORWARD_SPEED);
      delay(1500);
      motorStop();
      Serial.println("[Motor Test] Finished.");
    } else if (c == 'r') {
      Serial.println("[Motor Test] Running REVERSE at 50% (~12V on D26) for 1.0s...");
      motorReverse(REVERSE_SPEED);
      delay(1000);
      motorStop();
      Serial.println("[Motor Test] Finished.");
    }
  }

  // 1. Process coin pulses into accumulated credit
  if (pulseCount > 0) {
    noInterrupts();
    unsigned long timeSinceLast = millis() - lastValidPulseMs;
    interrupts();

    if (timeSinceLast > COIN_TIMEOUT_MS) {
      noInterrupts();
      int insertedPesos = pulseCount;
      pulseCount = 0;
      interrupts();

      totalEarningsPHP += insertedPesos;
      insertedCredit   += insertedPesos;
      refreshLCDScreen();

      Serial.println("------------------------------------------");
      Serial.printf(">> Coin Inserted : P%d.00\n", insertedPesos);
      Serial.printf(">> Total Credit  : P%d.00 (%d Sheets)\n", insertedCredit, insertedCredit);
      Serial.println(">> (Press physical button or web to dispense)");
      Serial.println("------------------------------------------");
    }
  }

  // 2. Physical Push Button Check (Active-HIGH)
  int btnReading = digitalRead(BUTTON_PIN);
  static int lastBtnState = LOW;
  static unsigned long btnPressStartTime = 0;
  static bool longPressHandled = false;

  if (btnReading != lastBtnState) {
    if (btnReading == HIGH) {
      btnPressStartTime = millis();
      longPressHandled = false;
    } else {
      unsigned long pressDuration = millis() - btnPressStartTime;
      if (!longPressHandled && pressDuration >= 50 && pressDuration < 3000) {
        Serial.println("[Button] Physical push button clicked!");
        triggerDispense();
      }
    }
    lastBtnState = btnReading;
  } else if (btnReading == HIGH && !longPressHandled) {
    if (millis() - btnPressStartTime >= 3000) {
      Serial.println("[Button] Long press detected (>3s)! Refilling paper tray...");
      longPressHandled = true;
      refillPaper();
    }
  }

  // 3. Dispenser State Machine (Runs when sheetsQueue > 0 and manual motor is not overriding)
  if (!manualMotorActive) {
    unsigned long now = millis();

    switch (currentState) {
      case STATE_IDLE:
        if (sheetsQueue > 0) {
          Serial.printf("[Dispenser] Dispensing sheet #%d...\n", totalSheetsDispensed + 1);
          currentState = STATE_REVERSE;
          stateStartTime = now;
          motorReverse(REVERSE_SPEED); // Step 1: 0.15s Reverse (cam engagement)
          refreshLCDScreen();
        }
        break;

      case STATE_REVERSE:
        if (now - stateStartTime >= REVERSE_TIME_MS) {
          currentState = STATE_FORWARD;
          stateStartTime = now;
          motorForward(FORWARD_SPEED); // Step 2: Forward (paper feed)
        }
        break;

      case STATE_FORWARD: {
        unsigned long targetForwardTime = (sheetsQueue <= 1) ? LAST_FORWARD_TIME_MS : FORWARD_TIME_MS;
        if (now - stateStartTime >= targetForwardTime) {
          motorStop();
          sheetsQueue--;
          totalSheetsDispensed++;
          if (paperRemaining > 0) paperRemaining--;
          refreshLCDScreen();

          Serial.printf("[Dispenser] Finished sheet #%d! Remaining in queue: %d (Tray Paper: %d)\n", 
                        totalSheetsDispensed, sheetsQueue, paperRemaining);

          currentState = STATE_PAUSE;
          stateStartTime = now;
        }
        break;
      }

      case STATE_PAUSE:
        if (now - stateStartTime >= PAUSE_BETWEEN_MS) {
          if (sheetsQueue > 0) {
            currentState = STATE_REVERSE;
            stateStartTime = now;
            motorReverse(REVERSE_SPEED);
            refreshLCDScreen();
          } else {
            currentState = STATE_IDLE;
            Serial.println("[Dispenser] Finished all queued sheets. Ready for next order.");
            updateLCD("TAKE YOUR PAPER ", "  THANK YOU!    ", true);
            delay(1200);
            refreshLCDScreen();
          }
        }
        break;
    }
  }

  // 4. Periodic LCD synchronization or background auto-reconnect
  static unsigned long lastLcdSyncMs = 0;
  if (!lcdReady) {
    if (millis() - lastLcdSyncMs >= 2000) {
      lastLcdSyncMs = millis();
      if (detectAndInitLCD()) {
        refreshLCDScreen();
      }
    }
  } else if (currentState == STATE_IDLE && !manualMotorActive && millis() - lastLcdSyncMs >= 2000) {
    lastLcdSyncMs = millis();
    if (isI2CBusHealthy()) {
      refreshLCDScreen();
    } else {
      Serial.println("[LCD] Bus lost connection. Will auto-reconnect.");
      lcdReady = false;
    }
  }
}
