# KAOS — Dual-Board Portal (Pi Pico + ESP32) 
Based off: https://github.com/NicoAICP/KAOS
All this code is Ai slopped together by claude because I just wanted to give the KOAS a webportal.

Each chip does what it's best at:

| Board | Job |
|-------|-----|
| **Pi Pico** | USB HID portal — appears as a real Skylander Portal of Power to any console or emulator |
| **ESP32 DevKit v1** | WiFi Access Point + Web UI + SD card management |

No PC. No Python. No proxy. Fully self-contained.

---

## System Diagram

```
Wii / PS3 / Xbox 360 / Dolphin / RPCS3
            ↕  USB HID  (VID 0x1430 / PID 0x0150)
         Pi Pico
            ↕  UART  (3 wires, 921600 baud)
         ESP32 DevKit v1
            ├── SD card  (.bin / .sky Skylander dumps)
            └── LCD1602  (shows WiFi AP name + IP)

Your phone or browser
            ↕  WiFi → 192.168.4.1
         Web UI
            (pick files, load P1/P2 slots, swap Skylanders live)
```

---

## Wiring

### ESP32 ↔ Pi Pico (UART, 3 wires)

| ESP32 | Pico | Signal |
|-------|------|--------|
| GPIO17 | GPIO5 | TX → RX |
| GPIO16 | GPIO4 | RX ← TX |
| GND    | GND  | Ground  |

Both boards run at 3.3V — no level shifting needed.

### SD Card → ESP32 (HSPI)

| Signal | ESP32 GPIO |
|--------|------------|
| MOSI   | 13 |
| MISO   | 12 |
| CLK    | 14 |
| CS     | 15 |

### LCD1602 → ESP32 (I2C, PCF8574 backpack)

| Signal | ESP32 GPIO |
|--------|------------|
| SDA    | 21 |
| SCL    | 22 |

Default I2C address: **0x27** (try **0x3F** if blank — change `LCD_I2C_ADDR` in `esp32/main/main.c`).

---

## SD Card

Format as **FAT32**. Place Skylander dumps in the root directory.

Supported formats: `.bin` `.dmp` `.dump` `.sky`

---

## Building — Pi Pico

Requires the [Pico SDK](https://github.com/raspberrypi/pico-sdk).

```bash
cd pico
export PICO_SDK_PATH=/path/to/pico-sdk
mkdir build && cd build
cmake ..
make -j4
```

Copy `kaos_pico.uf2` to the Pico (hold BOOTSEL while plugging in).

---

## Building — ESP32

Requires [ESP-IDF v5.1+](https://docs.espressif.com/projects/esp-idf/).

```bash
cd esp32
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash    # Windows: COM3 etc.
```

---

## Using the Web UI

1. Power both boards (the Pico via USB to the console/PC, the ESP32 via its USB-C)
2. LCD shows `KAOS-Portal` / `192.168.4.1`
3. Connect your phone or PC to WiFi **`KAOS-Portal`** (open, no password)
4. Open **http://192.168.4.1** in a browser
5. Two cards appear — P1 and P2
6. Select a file from the dropdown → **Load**
7. The card shows the Skylander's name, element, and emoji
8. The ESP32 instantly pushes the dump to the Pico over UART
9. The game detects the Skylander on the portal

**Force Re-Sense:** if the game doesn't detect a newly loaded Skylander, hit the re-sense button — it briefly unloads and reloads on the Pico to trigger detection.

---

## How Write-Back Works

When the game saves progress to a Skylander (XP, gold, abilities), it writes blocks back through the HID protocol to the Pico. The Pico re-encrypts the updated dump and sends it back to the ESP32 over UART (`MSG_WRITE_BACK`). The ESP32 saves it back to the original file on the SD card — so your Skylander's progress is preserved.

---

## Protocol (kaos_protocol.h)

Three messages over UART at 921600 baud. Framed with SOF byte, length, type, payload, XOR checksum.

| Message | Direction | Payload |
|---------|-----------|---------|
| `MSG_LOAD` | ESP32 → Pico | `slot(1)` + raw dump `(1024)` |
| `MSG_UNLOAD` | ESP32 → Pico | `slot(1)` |
| `MSG_WRITE_BACK` | Pico → ESP32 | `slot(1)` + updated dump `(1024)` |
| `MSG_PICO_READY` | Pico → ESP32 | *(none — boot signal)* |

---

## Customising

All pin assignments and the AP name/password are at the top of each `main.c`:

**ESP32** (`esp32/main/main.c`):
```c
#define WIFI_AP_SSID     "KAOS-Portal"
#define WIFI_AP_PASSWORD ""    // set a password to lock the AP
#define PIN_SD_MOSI      13
#define PIN_SD_CLK       14
// ...
```

**Pico** (`pico/main.c`):
```c
#define KAOS_UART_TX  4   // to ESP32 RX
#define KAOS_UART_RX  5   // from ESP32 TX
```

---

## File Map

```
kaos-dual/
├── kaos_protocol.h          Shared UART protocol (copied into both projects)
├── pico/
│   ├── main.c               USB HID portal + UART RX (core 1)
│   ├── skylander_slots.c/h  Slot state, block read/write
│   ├── usb_descriptors.c/h  TinyUSB descriptors (VID/PID, HID report)
│   ├── SkylanderCrypt.c/h   AES sector key derivation
│   ├── rijndael.c/h         AES-128, pure C
│   ├── kaos_protocol.h      Protocol (local copy)
│   ├── tusb_config.h        TinyUSB config
│   └── CMakeLists.txt
└── esp32/
    ├── main/
    │   ├── main.c           WiFi AP, SD, LCD, glue
    │   ├── web_ui.c/h       HTTP server + single-page web app
    │   ├── pico_bridge.c/h  UART2 link to Pico
    │   ├── Skylander.c/h    Slot metadata + file loading
    │   ├── SkylanderCrypt.c/h
    │   ├── rijndael.c/h
    │   ├── skylander_ids.h  Character ID → name/element table
    │   └── kaos_protocol.h  Protocol (local copy)
    ├── CMakeLists.txt
    └── sdkconfig.defaults
```
