# Bushnode — Dual-Radio ESP32 WiFi Scanner + LoRa Relay

Outdoor remote sensor node combining two ESP32 boards for WiFi 2.4GHz + 5GHz passive scanning, with LoRa uplink to a Meshtastic private mesh and SD card logging.

## What It Does

- **S3 (Heltec V3):** Scans 2.4GHz WiFi passively, looks for target SSIDs (Flock Cam, Metta Glasses, parked APs, etc.)
- **C5 (XIAO ESP32-C5):** Scans 5GHz WiFi passively, same target detection
- **LoRa relay:** High-priority alerts sent over SX1262 LoRa to Meshtastic mesh (private channel)
- **Logging:** Full scan history logged to SD card on C5 (verbose, all networks seen)
- **Display:** Heltec OLED shows scan status, battery, pairing code, recent alerts
- **TX limiting:** Duty-cycle manager (token-bucket, 1% airtime, 30-min window) prevents mesh spam

## Folder Structure

```
bushnode/
├── firmware/
│   ├── s3_main.ino              # Heltec V3 (S3) firmware
│   ├── c5_main.ino              # XIAO C5 firmware
│   ├── lora_dutycycle.cpp       # Duty-cycle limiter
│   ├── lora_dutycycle.h
│   ├── platformio.ini           # PlatformIO multi-board config
│   └── README.md                # This file
├── docs/
│   └── BUSHNODE_DESIGN.md       # Full architecture + testing checklist
└── README.md
```

## Hardware

| Board | Role | Notes |
|-------|------|-------|
| **Heltec V3 Wireless Stick** | 2.4GHz scanner + LoRa TX + display | SX1262, OLED, onboard battery charging |
| **XIAO ESP32-C5 Mini** | 5GHz scanner + SD logger | Native 5GHz, SD card slot |

**Wiring:** UART only (3 wires)
- S3 TX (GPIO 43) → C5 RX
- S3 RX (GPIO 44) → C5 TX
- S3 GND → C5 GND

## Quick Start

### 1. Flash the Boards

```bash
cd firmware/
pio run -e esp32c5_xiao -t upload    # Flash C5 first (simpler)
pio run -e esp32s3_heltec -t upload  # Flash S3
```

Or use Arduino IDE with the same .ino files.

### 2. Verify Over Serial

Open two terminals:

```bash
# Terminal 1: S3 debug output
pio device monitor -e esp32s3_heltec

# Terminal 2: C5 debug output
pio device monitor -e esp32c5_xiao
```

You should see:
- `[C5] WiFi initialized (scan mode)`
- `[S3] UART to C5 initialized`

### 3. Test Target Detection

Walk a known WiFi network (or your phone's hotspot) near the boards. You should see:
- `[C5] Target found: YourSSID (RSSI -50)` on C5 serial
- `[S3] Alert from C5: YourSSID (RSSI -50)` on S3 serial
- `[S3] Sent alert for YourSSID. Duty cycle: 99.5% remaining` once LoRa is wired

### 4. Check SD Logs

Power down, remove SD card from C5, read on your computer:

```
Bushnode C5 Scan Log
===================
Started: 1234 ms
[SCAN@5000] 5GHz networks found: 12
  MyNetwork | AA:BB:CC:DD:EE:FF | CH36 | RSSI -45
  OtherAP   | 11:22:33:44:55:66 | CH52 | RSSI -72
  [ALERT] ParkedAP_2026 found at RSSI -55
```

## Configuration

Edit the `.ino` files to customize:

- **Target SSIDs** (s3_main.ino line ~30, c5_main.ino line ~30):
  ```cpp
  const char* TARGET_SSIDS[3] = { "Flock Cam", "Metta Glasses", "ParkedAP_2026" };
  ```

- **Scan interval** (default 10 seconds):
  ```cpp
  #define SCAN_INTERVAL_MS 10000
  ```

- **Alerts per burst** (default 3, then reset duty-cycle bucket):
  ```cpp
  #define ALERTS_PER_BURST 3
  ```

- **Duty-cycle window** (default 30 min):
  ```cpp
  LoraDutyCycle duty_cycle(1.0);  // 1% airtime, 30-min window
  ```

- **Meshtastic channel** (default 1, your private channel):
  ```cpp
  #define MESHTASTIC_PRIVATE_CH 1
  ```

## Duty-Cycle Limiting

Token-bucket airtime manager prevents flooding the Meshtastic mesh:

- **Window:** 30 minutes
- **Budget:** 1% duty cycle = 18,000 ms per 30 min ≈ 6 packets/min average
- **Burst mode:** After 3 alerts sent, bucket resets for another burst

Example: If you detect 3 targets in 10 seconds, they all TX immediately. Then the bucket enters cooldown until either 30 min pass or you manually reset (see `duty_cycle.reset()`).

## Architecture Details

See `docs/BUSHNODE_DESIGN.md` for:
- Full data flow diagram
- Airtime calculation formula
- LoRa payload format
- Screen layout mockups
- Testing checklist
- Complete TODO list

## Known TODOs

- [ ] Integrate actual LoRa/Meshtastic libraries (RadioLib + Meshtastic Arduino)
- [ ] Verify 5GHz scanning on C5 (may need `esp_wifi_scan_start()` with channel mask)
- [ ] Battery voltage ADC reading for both boards
- [ ] Meshtastic RX handler (listen for incoming mesh messages)
- [ ] UART protocol robustness (add CRC/framing)
- [ ] OTA firmware update support
- [ ] Optional GPS tagging of detections

## Notes

- **RX-only scanning:** No active probes, no beacons. Quieter on spectrum, doesn't trigger IDS.
- **Separate from RTL-SDR locator:** The bushnode relays detections; there's a separate RTL-SDR direction-finder in the Raspyjack repo for tracking sources by signal strength.
- **Testing first:** Start with C5 + SD logging working, then add S3, then LoRa/Meshtastic once basic comms are solid.

---

**Author:** Zach (stackzac22)  
**Date:** 2026-08-02  
**Status:** Alpha firmware, in testing
