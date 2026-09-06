# 📄 ESP32 Standalone Bond Paper Vendo Machine (Printer Mechanism)

An industrial-grade, standalone bond paper vending machine firmware running on an **ESP32 (NodeMCU 30-pin)**. It converts standard inkjet printer paper feed mechanics (pickup cam + roller assembly) into a reliable commercial bond paper kiosk.

Includes **Allan universal multi-coin acceptor integration**, **high-torque 24V BTS7960 motor driver control**, **1602 I2C LCD display manager**, **physical arcade button dispense**, and a **real-time local WiFi Web Dashboard**.

---

## 🚀 Key Features

* **Calibrated Paper Dispense Sequence**:
  * **Reverse Kick (150ms @ 100% PWM)**: Disengages the printer cam and resets the paper separator pad.
  * **Forward Feed (1,000ms / 1,500ms @ 100% PWM)**: Grabs the single sheet and feeds it through the exit rollers. Last sheet in queue runs for 1.5s to ensure clean ejection.
  * **Inter-Sheet Pause (200ms)**: Eliminates back-EMF spikes and protects the H-bridge during multi-sheet orders.
* **Allan Universal Coin Selector (1N4007 Anti-Noise Filter)**:
  * Hardware interrupt with strict pulse-width verification (15ms–120ms).
  * Rejects motor driver electrical noise and high-frequency EMI.
  * Multi-coin support: ₱1 (1 pulse), ₱5 (5 pulses), ₱10 (10 pulses).
* **Physical Push Button Control**:
  * Active-HIGH input with internal pulldown (`GPIO 23`) and 3.3V source (`GPIO 19`).
  * Short press (<3s): Triggers paper dispensing for accumulated credit.
  * Long press (>=3s): Refills paper tray inventory to 50 sheets.
* **1602 I2C LCD Manager**:
  * Address autodetection (`0x27` / `0x3F`) on `SDA: GPIO 32`, `SCL: GPIO 33`.
  * Hardware health guard (`isI2CBusHealthy()`) prevents I2C bus lockups if SDA/SCL lines are grounded.
  * Flicker-free text buffer displays credit, remaining sheets, and boot IP address.
* **Local WiFi Web Control Dashboard**:
  * Embedded responsive dark-mode Web UI hosted directly on the ESP32 port 80.
  * Manual motor controls: **FORWARD**, **REVERSE**, **STOP**, and **Pulse Bursts**.
  * Remote vendo actions: Dispense 1 sheet, add test credit, refill tray.
  * Live status API (`/api/status`) polling every 1 second.
  * **Automatic Fallback Access Point**: If the local WiFi router is unreachable, automatically broadcasts `BondPaper-Vendo` (`192.168.4.1`, password: `12345678`).

---

## 📌 Pinout & Hardware Connections

| Peripheral | ESP32 Pin | Function / Notes |
| :--- | :--- | :--- |
| **Allan Coin Selector** | **GPIO 18** | Coin pulse input. Connected via **1N4007 diode** (Cathode to Coin line, Anode to GPIO 18 with `INPUT_PULLUP`). |
| **Push Button (Signal)**| **GPIO 23** | Button switch contact (`INPUT_PULLDOWN`, Active-HIGH). |
| **Push Button (Power)** | **GPIO 19** | Output set to 3.3V constant (`OUTPUT HIGH`) to supply switch. |
| **BTS7960 `RPWM`** | **GPIO 27** | Forward motor speed PWM (0–255). |
| **BTS7960 `LPWM`** | **GPIO 26** | Reverse motor speed PWM (0–255). |
| **BTS7960 `R_EN` / `L_EN`** | **VCC (5V)** | **Jumpered directly to `VCC` (5V)** on the BTS7960 board for full gate saturation. |
| **BTS7960 `VCC`** | **VIN (5V)** | Connect to ESP32 `VIN` (5V USB/Regulator). *Never connect to 3V3!* |
| **BTS7960 `GND`** | **GND** | Common ground with ESP32 and 24V Power Supply negative. |
| **BTS7960 `R_IS` / `L_IS`** | **NC (Empty)** | Current sense alarm outputs. **Leave completely disconnected.** |
| **LCD 1602 `SDA`** | **GPIO 32** | I2C Data line with internal pullup. |
| **LCD 1602 `SCL`** | **GPIO 33** | I2C Clock line with internal pullup. |
| **LCD 1602 `VCC`** | **VIN (5V)** | 5V power for LCD backlight and logic. |
| **LCD 1602 `GND`** | **GND** | Common ground. |

---

## ⚠️ Critical Hardware & BTS7960 Guidelines

1. **Do NOT connect `R_IS` or `L_IS` to power!**
   * `IS` pins are **Current Sense Outputs** from the BTS7960 internal diagnostic circuitry.
   * Connecting voltage to `IS` activates the chip's internal fault shutdown and clamps the PWM inputs to 0V. Leave them floating.
2. **BTS7960 Logic `VCC` Must Be 5V**:
   * The onboard 74HC244 buffer requires 5V logic. Connecting `VCC` to 3.3V prevents the MOSFET gates from opening fully, dropping your 24V supply to <10V.
3. **Common Ground**:
   * The negative terminal (`-`) of the 24V motor power supply **must be connected to ESP32 `GND`**.

---

## 🌐 Web Dashboard & Endpoints

Open `http://<ESP32_IP>/` in any browser on the same network:

* `GET /` — Serves the interactive dark-mode control dashboard.
* `GET /api/status` — Returns JSON live telemetry:
  ```json
  {
    "credit": 0,
    "queue": 0,
    "paper": 50,
    "dispensed": 0,
    "earnings": 0,
    "state": "IDLE",
    "manual": "",
    "ip": "192.168.8.46",
    "wifi": "YOTC-329FD5"
  }
  ```
* `POST /api/motor?action=<ACTION>` — Controls motor actions:
  * `action=forward` — Runs motor forward at 100% (with 8s safety timeout).
  * `action=reverse` — Runs motor reverse at 100%.
  * `action=stop` — Emergency stop.
  * `action=pulse_forward` — 1.5s forward feed burst.
  * `action=pulse_reverse` — 0.5s reverse kick burst.
  * `action=dispense` — Queues and dispenses 1 sheet.
  * `action=add_credit&amount=5` — Injects test credit.
  * `action=refill` — Refills paper tray to 50.

---

## 📁 Repository Structure

```
.
├── bond_paper_vendo/            # Main Vendo Firmware
│   └── bond_paper_vendo.ino     # Complete integrated firmware with Web Server & LCD
├── button_test/                 # Standalone Active-HIGH Button Web Tester
│   └── button_test.ino
├── i2c_lcd_test/                # Standalone 1602 I2C Scanner & LCD Web Tester
│   └── i2c_lcd_test.ino
├── ir_sensor_test/              # Standalone IR Paper Detection Sensor Web Tester
│   └── ir_sensor_test.ino
├── .gitignore                   # Ignores build artifacts and binaries
└── README.md                    # Project documentation and schematics
```

---

## 🛠️ Compilation & Flashing

Using **Arduino CLI**:
```bash
# Compile
arduino-cli compile --fqbn esp32:esp32:esp32 bond_paper_vendo

# Upload
arduino-cli upload -p COM10 --fqbn esp32:esp32:esp32 bond_paper_vendo
```
