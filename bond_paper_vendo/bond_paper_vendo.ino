#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// =======================================================
// Pin Configuration
// =======================================================
const int COIN_PIN       = 18; // Allan Coin Selector pulse input (via 1N4007)
const int RPWM_PIN       = 27; // Forward speed (PWM) on D27
const int LPWM_PIN       = 26; // Reverse speed (PWM) on D26
const int REN_PIN        = -1; // Unused (R_EN and L_EN jumpered to VCC on BTS7960)
const int BUTTON_PIN     = 23; // Physical Push Button (Active-HIGH with INPUT_PULLDOWN)
const int BUTTON_PWR_PIN = 19; // Provides 3.3V source for button (OUTPUT HIGH)
int activeSDA            = 32; // Primary I2C Data line (GPIO 32)
int activeSCL            = 33; // Primary I2C Clock line (GPIO 33)

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

void testI2CPinVoltages() {
  Wire.end();
  Serial.println("\n========== I2C ELECTRICAL DIAGNOSTIC ==========");
  // Test SDA (GPIO 32)
  pinMode(32, INPUT_PULLUP);
  delay(5);
  int sdaPU = digitalRead(32);
  pinMode(32, INPUT_PULLDOWN);
  delay(5);
  int sdaPD = digitalRead(32);
  pinMode(32, INPUT);
  delay(5);
  int sdaFloat = digitalRead(32);

  // Test SCL (GPIO 33)
  pinMode(33, INPUT_PULLUP);
  delay(5);
  int sclPU = digitalRead(33);
  pinMode(33, INPUT_PULLDOWN);
  delay(5);
  int sclPD = digitalRead(33);
  pinMode(33, INPUT);
  delay(5);
  int sclFloat = digitalRead(33);

  // Test GPIO 21 / 22
  pinMode(21, INPUT_PULLUP);
  delay(5);
  int p21PU = digitalRead(21);
  pinMode(21, INPUT_PULLDOWN);
  delay(5);
  int p21PD = digitalRead(21);

  pinMode(22, INPUT_PULLUP);
  delay(5);
  int p22PU = digitalRead(22);
  pinMode(22, INPUT_PULLDOWN);
  delay(5);
  int p22PD = digitalRead(22);

  Serial.printf("  GPIO 32 (SDA): PU=%d, PD=%d, RAW=%d -> %s\n", 
    sdaPU, sdaPD, sdaFloat, 
    (sdaPU==0 && sdaPD==0) ? "CLAMPED LOW (LCD 5V VCC is OFF or wire shorted to GND!)" :
    (sdaPU==1 && sdaPD==1) ? "PULLED HIGH OK (LCD 5V VCC is powered on)" : "FLOATING (Wire disconnected)");

  Serial.printf("  GPIO 33 (SCL): PU=%d, PD=%d, RAW=%d -> %s\n", 
    sclPU, sclPD, sclFloat, 
    (sclPU==0 && sclPD==0) ? "CLAMPED LOW (LCD 5V VCC is OFF or wire shorted to GND!)" :
    (sclPU==1 && sclPD==1) ? "PULLED HIGH OK (LCD 5V VCC is powered on)" : "FLOATING (Wire disconnected)");

  Serial.printf("  GPIO 21 (D21): PU=%d, PD=%d\n", p21PU, p21PD);
  Serial.printf("  GPIO 22 (D22): PU=%d, PD=%d\n", p22PU, p22PD);
  Serial.println("================================================\n");

  Wire.begin(activeSDA, activeSCL);
}

bool probeI2CPair(int sda, int scl, uint8_t &outAddr) {
  Wire.end();
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  delay(10);

  // An I2C bus MUST idle HIGH when pullups are present.
  // If either line is LOW, the bus is grounded, shorted, or the LCD is unpowered!
  if (digitalRead(sda) == LOW || digitalRead(scl) == LOW) {
    return false;
  }

  Wire.begin(sda, scl);
  Wire.setClock(100000);
  Wire.setTimeOut(30);

  const uint8_t candidates[] = {0x27, 0x3F, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26};
  for (uint8_t a : candidates) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      outAddr = a;
      return true;
    }
  }
  return false;
}

// =======================================================
// Direct Low-Level HD44780 over PCF8574 Initializer
// =======================================================
void lcdSendNibbleRaw(uint8_t addr, uint8_t nibble, bool rs, bool backlight) {
  uint8_t bl = backlight ? 0x08 : 0x00;
  uint8_t rsBit = rs ? 0x01 : 0x00;
  uint8_t val = (nibble & 0xF0) | bl | rsBit;

  // Pulse Enable (Bit 2 = 0x04) HIGH then LOW
  Wire.beginTransmission(addr);
  Wire.write(val | 0x04);
  Wire.endTransmission();
  delayMicroseconds(2); // Enable pulse width > 450ns

  Wire.beginTransmission(addr);
  Wire.write(val & ~0x04);
  Wire.endTransmission();
  delayMicroseconds(50); // Command execution wait > 37us
}

void lcdSendCommandRaw(uint8_t addr, uint8_t cmd, bool backlight = true) {
  lcdSendNibbleRaw(addr, cmd & 0xF0, false, backlight);
  lcdSendNibbleRaw(addr, (cmd << 4) & 0xF0, false, backlight);
}

bool initHD44780Direct(uint8_t addr, int sda, int scl) {
  Wire.begin(sda, scl);
  Wire.setClock(100000);
  Wire.setTimeOut(50);

  delay(100);

  Wire.beginTransmission(addr);
  if (Wire.endTransmission() != 0) {
    return false;
  }

  // Backlight ON
  Wire.beginTransmission(addr);
  Wire.write(0x08);
  Wire.endTransmission();
  delay(10);

  // Hardware 4-Bit Reset Sequence (Hitachi HD44780 Table 24)
  lcdSendNibbleRaw(addr, 0x30, false, true);
  delay(6);
  lcdSendNibbleRaw(addr, 0x30, false, true);
  delay(6);
  lcdSendNibbleRaw(addr, 0x30, false, true);
  delay(2);

  // Switch to 4-bit interface
  lcdSendNibbleRaw(addr, 0x20, false, true);
  delay(2);

  // Function Set: 4-bit, 2 lines, 5x8 font (0x28)
  lcdSendCommandRaw(addr, 0x28, true);
  delay(2);

  // Display ON, Cursor OFF, Blink OFF (0x0C)
  lcdSendCommandRaw(addr, 0x0C, true);
  delay(2);

  // Clear Display (0x01)
  lcdSendCommandRaw(addr, 0x01, true);
  delay(5);

  // Entry Mode Set: Increment cursor, No shift (0x06)
  lcdSendCommandRaw(addr, 0x06, true);
  delay(2);

  return true;
}

bool detectAndInitLCD() {
  const int pairs[][2] = {
    {32, 33}, // Primary I2C pins: SDA=32, SCL=33
    {21, 22}, // Fallback standard ESP32 pins: SDA=21, SCL=22
    {33, 32}, // In case SDA/SCL were reversed
    {22, 21}
  };

  uint8_t foundAddr = 0;
  int foundSDA = -1;
  int foundSCL = -1;

  for (auto &pair : pairs) {
    int s = pair[0];
    int c = pair[1];
    for (int attempt = 0; attempt < 3; attempt++) {
      if (probeI2CPair(s, c, foundAddr)) {
        foundSDA = s;
        foundSCL = c;
        break;
      }
      delay(30);
    }
    if (foundAddr != 0) break;
  }

  if (foundAddr != 0) {
    activeSDA = foundSDA;
    activeSCL = foundSCL;
    activeLcdAddr = foundAddr;
    Serial.printf("[LCD] SUCCESS! LCD found at 0x%02X on SDA=%d, SCL=%d!\n", activeLcdAddr, activeSDA, activeSCL);

    if (initHD44780Direct(activeLcdAddr, activeSDA, activeSCL)) {
      lcd = LiquidCrystal_I2C(activeLcdAddr, 16, 2);
      lcd.begin(16, 2);
      lcd.backlight();
      lcd.clear();
      lcdReady = true;
      Serial.println("[LCD] HD44780 Controller configured in 4-bit mode successfully!");
      return true;
    } else {
      Serial.println("[LCD ERROR] Failed to send initialization commands to HD44780!");
    }
  } else {
    Serial.println("[LCD WARNING] No I2C backpack detected on pins 32/33 or 21/22.");
  }
  return false;
}

// =======================================================
// LCD Display Manager (Flicker-Free Text Buffer)
// =======================================================
void updateLCD(String l1, String l2, bool force = false) {
  if (!lcdReady) return;

  static unsigned long lastForceRepaintMs = 0;
  if (millis() - lastForceRepaintMs >= 5000) {
    force = true;
    lastForceRepaintMs = millis();
  }

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
// Setup
// =======================================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout detector
  Serial.begin(115200);
  delay(500); // 0.5s power rail stabilization on cold boot

  Serial.println("\n==========================================");
  Serial.println(" Bond Paper Vendo Machine (Standalone)    ");
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
  delay(50);

  if (detectAndInitLCD()) {
    updateLCD("BOND PAPER VENDO", "READY TO DISPENSE", true);
  } else {
    Serial.println("[LCD] Not detected on standard pin pairs. Background auto-scan active.");
  }

  // Print all live pin statuses
  printPinStatus();

  refreshLCDScreen();
  Serial.println("[Vendo] System initialized and ready for coins.");
}

// =======================================================
// Main Loop
// =======================================================
void loop() {
  // Check for Serial status query commands
  if (Serial.available() > 0) {
    char c = Serial.read();
    if (c == 'p' || c == 's' || c == '?') {
      printPinStatus();
    } else if (c == 'i') {
      testI2CPinVoltages();
    } else if (c == 'l') {
      Serial.println("\n[LCD] Manual re-initialization requested...");
      detectAndInitLCD();
      refreshLCDScreen();
    } else if (c == 'b') {
      Serial.println("\n[LCD Test] Blinking backlight and printing test text...");
      if (lcdReady) {
        lcd.noBacklight();
        delay(400);
        lcd.backlight();
        delay(200);
        updateLCD("LCD TEST OK!    ", "1234567890ABCDEF", true);
        Serial.println("[LCD Test] Test pattern written to LCD!");
      } else {
        Serial.println("[LCD Test] LCD not ready!");
      }
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
      Serial.println(">> (Press physical button to dispense)");
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

  // 3. Dispenser State Machine (Runs when sheetsQueue > 0)
  unsigned long now = millis();

  switch (currentState) {
    case STATE_IDLE:
      if (sheetsQueue > 0) {
        Serial.printf("[Dispenser] Dispensing sheet #%d...\n", totalSheetsDispensed + 1);
        currentState = STATE_REVERSE;
        stateStartTime = now;
        motorReverse(REVERSE_SPEED); // Step 1: 0.15s Reverse (cam engagement at 50% PWM)
        refreshLCDScreen();
      }
      break;

    case STATE_REVERSE:
      if (now - stateStartTime >= REVERSE_TIME_MS) {
        currentState = STATE_FORWARD;
        stateStartTime = now;
        motorForward(FORWARD_SPEED); // Step 2: Forward (paper feed at 100% PWM)
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

  // 4. Periodic LCD synchronization (refresh text without tearing down I2C bus)
  static unsigned long lastLcdSyncMs = 0;
  if (lcdReady && currentState == STATE_IDLE && millis() - lastLcdSyncMs >= 2000) {
    lastLcdSyncMs = millis();
    refreshLCDScreen();
  } else if (!lcdReady && millis() - lastLcdSyncMs >= 3000) {
    lastLcdSyncMs = millis();
    if (detectAndInitLCD()) {
      refreshLCDScreen();
    }
  }
}
