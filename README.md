# C4-68 Design Demonstration — Hydro Turbine Electricity

A multi-sensor monitoring system for a hydro turbine generator, built on ESP32 with FreeRTOS.  
Reads multiple sensors simultaneously and sends telemetry data to **Arduino IoT Cloud**.

---

## 📋 Table of Contents

- [System Overview](#system-overview)
- [Hardware Requirements](#hardware-requirements)
- [Wiring Diagram](#wiring-diagram)
- [Software Prerequisites](#software-prerequisites)
- [Getting Started](#getting-started)
- [Arduino IoT Cloud Setup](#arduino-iot-cloud-setup)
- [Project Structure](#project-structure)
- [Cloud Variables](#cloud-variables)
- [Configuration](#configuration)
- [Troubleshooting](#troubleshooting)

---

## System Overview

The system reads data from 4 sensor types, displays it on a 20×4 LCD, and uploads telemetry to Arduino IoT Cloud:

| Sensor              | Measurement                     | Protocol           |
| ------------------- | ------------------------------- | ------------------ |
| PZEM-017            | Voltage, Current, Power, Energy | Modbus RTU (RS485) |
| Flow Sensor         | Flow Rate (L/min)               | Pulse / Interrupt  |
| Pressure Transducer | Pressure (bar / MPa)            | Analog 0.5–4.5V    |
| Hall Sensor         | RPM (Pulse Count)               | Pulse / Interrupt  |

The system runs as **multitask** on FreeRTOS with tasks distributed across both ESP32 cores:

- **Core 0** — Arduino IoT Cloud + WiFi
- **Core 1** — PZEM, LCD, Flow, Pressure, RPM

---

## Hardware Requirements

- ESP32 DOIT DevKit V1
- PZEM-017 DC Energy Meter (50A Shunt)
- Flow Sensor (YF-S201 or equivalent)
- Pressure Transducer 0–8 bar (0.5–4.5V output)
- Hall Effect Sensor (4-pole magnet)
- LCD 20×4 I2C (Address: `0x27`)
- Voltage Divider Resistors: 10kΩ and 22kΩ (for Pressure Sensor)
- 2× Status LEDs (Power / WiFi)

---

## Wiring Diagram

| Device                   | ESP32 Pin | Notes                           |
| ------------------------ | --------- | ------------------------------- |
| Flow Sensor (Signal)     | GPIO 27   | INPUT_PULLUP, Interrupt FALLING |
| Pressure Sensor (Analog) | GPIO 34   | Through 10k:22k Voltage Divider |
| Hall Sensor (Signal)     | GPIO 26   | INPUT_PULLUP, Interrupt FALLING |
| PZEM-017 TX              | GPIO 17   | Serial2 TX                      |
| PZEM-017 RX              | GPIO 16   | Serial2 RX                      |
| LCD SDA                  | GPIO 21   | I2C (default)                   |
| LCD SCL                  | GPIO 22   | I2C (default)                   |
| LED Power                | GPIO 19   | Active HIGH                     |
| LED Status (WiFi)        | GPIO 18   | Active HIGH                     |

---

## Software Prerequisites

1. **[Visual Studio Code](https://code.visualstudio.com/)** — IDE
2. **[PlatformIO IDE Extension](https://platformio.org/install/ide?install=vscode)** — Install via VS Code Extensions
3. **[Antigravity Extension](https://marketplace.visualstudio.com/)** — Install the **Antigravity** extension via VS Code Extensions marketplace for AI-assisted development and pair programming
4. **[Git](https://git-scm.com/downloads)** — For cloning the repository
5. **Arduino IoT Cloud Account** — Sign up at [cloud.arduino.cc](https://cloud.arduino.cc/)

---

## Getting Started

### 1. Clone the Repository

```bash
git clone https://github.com/Jiranuwat-k/C4-68-Design-Demonstration-HydroTurbineElectricity.git
cd C4-68-Design-Demonstration-HydroTurbineElectricity
```

### 2. Open the Project in VS Code

```bash
code .
```

> PlatformIO will automatically detect `platformio.ini` and configure the project.

### 3. Wait for PlatformIO to Install Dependencies

On first open, PlatformIO will download:

- **Platform:** `espressif32`
- **Libraries:**
  - `LiquidCrystal_I2C` (v1.1.4)
  - `ArduinoIoTCloud` (v2.9.1)
  - `Arduino_ConnectionHandler` (v1.2.0)
- **Local Library:** `ModbusMasterPzem017` (located in `lib/` folder)

> ⏳ This may take a few minutes on the first download.

### 4. Configure Arduino IoT Cloud Credentials

Edit `src/thingProperties.h`:

```cpp
const char DEVICE_LOGIN_NAME[] = "your-device-id";       // Device ID from Arduino IoT Cloud
const char SSID[]              = "your-wifi-name";        // WiFi network name
const char PASS[]              = "your-wifi-password";     // WiFi password
const char DEVICE_KEY[]        = "your-device-key";        // Secret Key from Cloud
```

> ⚠️ **Important:** Do not commit WiFi credentials or Device Keys to a public repository.

### 5. Connect the ESP32 Board

- Plug in the ESP32 via USB
- PlatformIO will auto-detect the COM port
- If not detected, install the [CP2102 USB Driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)

### 6. Build the Project

Click the **✓ (Build)** button in the VS Code bottom toolbar, or run:

```bash
pio run
```

### 7. Upload to the Board

Click the **→ (Upload)** button, or run:

```bash
pio run --target upload
```

### 8. Open Serial Monitor

Click the **🔌 (Serial Monitor)** button, or run:

```bash
pio device monitor
```

- **Baud Rate:** 115200
- You should see sensor data in Teleplot format:
  ```
  >voltage:12.50
  >current:0.350
  >flow:3.20
  >pressure:1.50
  >rpm_count:1200
  ```

---

## Arduino IoT Cloud Setup

### Create a Thing on Arduino IoT Cloud

1. Go to [cloud.arduino.cc](https://cloud.arduino.cc/) → **Things** → **Create Thing**
2. Add the following **Variables**:

| Variable Name | Type  | Permission | Update Policy |
| ------------- | ----- | ---------- | ------------- |
| `volt`        | Float | Read Only  | On Change     |
| `current`     | Float | Read Only  | On Change     |
| `flow`        | Float | Read Only  | On Change     |
| `pressure`    | Float | Read Only  | On Change     |
| `rpm`         | Float | Read Only  | On Change     |
| `power`       | Float | Read Only  | On Change     |

3. Configure **Device** → Select ESP32 → Copy `DEVICE_LOGIN_NAME` and `DEVICE_KEY`
4. Configure **Network** → Enter your WiFi SSID and Password
5. Create a **Dashboard** to visualize real-time data

---

## Project Structure

```
C4-68-Design-Demonstration-HydroTurbineElectricity/
├── include/                    # Header files
├── lib/
│   └── ModbusMasterPzem017/    # Modbus library for PZEM-017
├── src/
│   ├── main.cpp                # Main code (FreeRTOS + Sensors)
│   ├── main1.txt               # Code backup / reference
│   ├── thingProperties.h       # Arduino IoT Cloud configuration
│   └── sketch.json             # Board settings
├── test/                       # Unit tests
├── platformio.ini              # PlatformIO configuration
├── .gitignore
└── README.md                   # This file
```

---

## Cloud Variables

| Variable   | Description                 | Unit  |
| ---------- | --------------------------- | ----- |
| `volt`     | DC Voltage                  | V     |
| `current`  | DC Current                  | A     |
| `power`    | Electrical Power            | W     |
| `flow`     | Water Flow Rate             | L/min |
| `pressure` | Water Pressure              | bar   |
| `rpm`      | Turbine Speed (Pulse Count) | RPM   |

---

## Configuration

The following constants can be adjusted in `main.cpp`:

```cpp
// Number of magnets on the Hall Sensor
#define RPM_PULSES_PER_REV  4

// Flow Sensor calibration factor
#define FLOW_CALIBRATION    4.5

// Voltage Divider ratio (10k:22k)
#define VOLTAGE_DIVIDER     0.6875

// Task update intervals (ms)
#define CLOUD_UPDATE_RATE   2000
#define PZEM_READ_RATE      5000
#define LCD_UPDATE_RATE     1000
```

---

## Troubleshooting

| Problem                          | Solution                                                         |
| -------------------------------- | ---------------------------------------------------------------- |
| Build fails — WiFiNINA not found | `lib_ignore = WiFiNINA` is already set in `platformio.ini`       |
| PZEM read failed                 | Check TX/RX wiring (GPIO 17/16) and Slave Address (0x01)         |
| LCD not displaying               | Verify I2C address with an I2C Scanner — default is `0x27`       |
| WiFi not connecting              | Check SSID / Password in `thingProperties.h`                     |
| RPM always 0                     | Check Hall Sensor wiring and magnet count (`RPM_PULSES_PER_REV`) |
| Negative pressure reading        | Check Voltage Divider — signal must be ≤ 3.3V at GPIO 34         |
| Upload fails                     | Hold the BOOT button on ESP32 during upload                      |

---

## 📄 License

This project is part of the **C4-68 Design Demonstration** — Hydro Turbine Electricity Generation System.
