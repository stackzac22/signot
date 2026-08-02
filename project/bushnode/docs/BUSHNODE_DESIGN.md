# Bushnode Design — Dual-Radio ESP32 Node

## Overview

**Bushnode** is an outdoor remote sensor node: dual-radio (5GHz + 2.4GHz), Meshtastic mesh participant, logs hits to SD, relays high-priority WiFi detections over LoRa.

### Hardware

| Component | Board | Role |
|-----------|-------|------|
| **WiFi Scanner (5GHz)** | ESP32-C5 (XIAO Mini or Waveshare Dev) | Passive WiFi scan, RX-only, target detection |
| **WiFi Scanner (2.4GHz)** | ESP32-S3 (Heltec V3 Wireless Stick) | Passive WiFi scan, RX-only, target detection |
| **LoRa Radio** | SX1262 (on Heltec V3) | LoRa TX/RX, Meshtastic mesh uplink |
| **Display** | OLED (Heltec V3) | Pairing code, scan status, alerts, battery, mesh status |
| **Storage** | SD Card (on C5, ideally both) | Verbose scan logs (all networks seen + timestamps) |
| **Power** | Battery (TBD by testing) | Portable operation |

### Data Flow

```
C5 (5GHz Scan)                S3 (2.4GHz Scan)
    ↓                              ↓
  [detect target SSID]        [detect target SSID]
    ↓                              ↓
  ALERT msg via UART →   S3 Receives + Enqueues
                            ↓
                        [Check duty cycle]
                            ↓ (if budget available)
                        [TX via SX1262 LoRa]
                            ↓
                        [Meshtastic Mesh]
                            ↓
                        [Mesh neighbors receive]

Parallel:
    C5 Logs all scans to SD card (full verbose)
    S3 Logs own 2.4GHz hits to... (TODO: where? see below)
```

## Firmware Structure

### S3 (Heltec V3) - `s3_main.ino`

**Responsibilities:**
1. Scan 2.4GHz WiFi passively (stay on high-traffic channels)
2. Detect target SSIDs (Flock Cam, Metta Glasses, ParkedAP)
3. Receive UART alerts from C5
4. Manage LoRa TX with duty-cycle limiting
5. Handle Meshtastic mesh RX (listen for incoming messages)
6. Display status (scan, battery, pairing code, recent alerts)

**Key Functions:**
- `handle_2_4ghz_scan()` — WiFi.scanNetworks() every 10s
- `handle_uart_from_c5()` — Parse "ALERT:SSID:RSSI" messages
- `process_alert_queue()` — Respect duty cycle before TX
- `transmit_lora_meshtastic()` — Actually send to LoRa radio
- `update_display()` — OLED refresh with status

**Dependencies:**
- `lora_dutycycle.h` — Token-bucket airtime limiter
- RadioLib or Heltec's built-in LoRa wrapper (for SX1262)
- Meshtastic Arduino library (for mesh uplink)
- Heltec's OLED library

---

### C5 (XIAO/Waveshare) - `c5_main.ino`

**Responsibilities:**
1. Scan 5GHz WiFi passively (RX only, no probes)
2. Detect target SSIDs
3. Send UART alerts to S3 for relay
4. Log all scan results to SD card (verbose)
5. Keep SD card as source-of-truth for full logs

**Key Functions:**
- `handle_5ghz_scan()` — WiFi.scanNetworks() every 10s, log all results
- `alert_s3()` — Send "ALERT:SSID:RSSI\n" over UART
- `log_to_sd()` — Append scan results and alerts to file
- `open_log_file()` — Initialize SD card logging

**Dependencies:**
- SD.h (Arduino SD library)
- WiFi.h (ESP32 WiFi for 5GHz scanning)

---

## Duty-Cycle Limiting (`lora_dutycycle.cpp`)

**Why it matters:**
- No FCC hard requirement for US915, but we self-impose ~1% to respect shared spectrum
- Protects against TX-stuck firmware bugs (sensor stuck in alert loop)
- Keeps Meshtastic mesh healthy (other nodes can use the channel too)

**Algorithm: Token Bucket + Manual Reset**

Continuous refill every **30 minutes** (shorter window = more aggressive budget per cycle):

```
token_bucket (30-min window):
  capacity = (30*60*1000 / 100) * 1% = 18,000 ms per 30 min
  available_tokens = capacity
  
on_refill_check (every TX attempt):
  elapsed = now() - last_refill
  refill_rate = capacity / 30_min_ms  (tokens/ms)
  available_tokens += (refill_rate * elapsed)
  available_tokens = min(available_tokens, capacity)
  
on_tx_request (alert):
  airtime_ms = compute_airtime(payload_len, SF)
  if available_tokens >= airtime_ms:
    proceed_with_tx()
    available_tokens -= airtime_ms
  else:
    queue_for_retry()  or drop if low-priority

manual_reset():
  available_tokens = capacity  (start 30-min window fresh)
  # Call this after N successful TXs (e.g., after 3 alerts)
  # or via button press / timer event
```

**Airtime Calculation:**

```
T_sym = 2^SF / BW_hz  (symbol time, in seconds)
T_preamble = (8 + 4.25) * T_sym  (8 preamble symbols + 4.25 overhead)
T_payload = ceil(8*N / (4*SF)) * (CR + 4) * T_sym
T_total = T_preamble + T_payload  (in milliseconds)
```

For US915 with SF7, 125 kHz BW, CR=4/5:
- ~50 byte payload ≈ 50 ms airtime
- ~100 byte payload ≈ 100 ms airtime

At 1% duty cycle over **30 min**: budget = 18,000 ms per window ≈ 180 × 100-byte packets ≈ **6 packets per minute average**, with manual reset allowing **bursts** after logging a few hits (e.g., reset after 3 alerts detected).

**Usage Pattern:**
```
// After 3 consecutive target detections, reset bucket for another burst
if (targets_found_count >= 3) {
    duty_cycle.reset();
    targets_found_count = 0;
}
```

This lets you catch "hot" targets in real-time while still respecting airtime limits long-term.

---

## LoRa Payload Format

**Simple binary format (can be extended to protobuf later):**

```
Offset  Field          Size    Description
0       Type           1 byte  0x01 = Alert message
1–32    SSID           32 byte Null-terminated string (target name)
33      RSSI           1 byte  (uint8_t cast of signed RSSI, e.g., 256-42=214 for -42 dBm)
```

**Example:** `01 46 6C 6F 63 6B 20 43 61 6D 00 ... D6` = "Alert, Flock Cam, -42 dBm"

Meshtastic wraps this in its own mesh envelope (adds hop count, sender ID, etc.) before transmitting.

---

## Screen Layout (Heltec OLED)

Assuming 128×64 pixel display, roughly 4–5 lines readable:

```
Line 1: [S:2.4GHz Scan | Bat 87%]
Line 2: Pair: ABC123            (shown once on boot)
Line 3: ⭘ Flock(-42) | ⭘ Metta(-68)
Line 4: LoRa: ✓ | Mesh: 2 nodes
```

When scanning, rotate through recent hits. When idle, show static pairing code.

---

## TODO (Not Yet Implemented)

1. **Actual LoRa/Meshtastic integration** — Wire up RadioLib or Heltec's built-in SX1262 driver + Meshtastic Arduino lib
2. **5GHz channel selection on C5** — May need direct `esp_wifi_scan_start()` with channel mask if the basic WiFi.scanNetworks() doesn't properly scan 5GHz
3. **S3 SD card (optional)** — Design decision: does S3 also log, or rely on C5 as sole storage?
4. **Battery voltage reading** — ADC for S3 and C5 battery monitoring
5. **Meshtastic RX handling** — Listen for incoming mesh messages (optional feature)
6. **OTA firmware update** — How do you update both boards when they're deployed?
7. **Persistent state** — Save/restore pairing code, channel ID, target SSID list across reboot
8. **UART protocol robustness** — Add CRC or frame markers to handle corruption
9. **GPS integration** — Optional: GPS tagging of detections (needs extra hardware)

---

## Testing Checklist

1. **UART inter-board comms**
   - [ ] C5 sends "ALERT:TestSSID:-50\n", S3 receives and parses
   - [ ] Verify baud rate match and signal levels (3.3V logic)

2. **WiFi scanning**
   - [ ] C5 scans 5GHz, reports real networks
   - [ ] S3 scans 2.4GHz, reports real networks
   - [ ] Both detect a known test SSID

3. **Target SSID detection**
   - [ ] When a target SSID is nearby, both boards alert correctly
   - [ ] Alerts queue properly if multiple targets are seen

4. **SD logging (C5)**
   - [ ] Scan results written to SD card in readable format
   - [ ] Can power cycle and resume logging without data loss
   - [ ] Log file grows continuously

5. **Duty-cycle limiting**
   - [ ] Enable high-priority alert spam (e.g., 100 per second)
   - [ ] Verify LoRa TX throttles to ~1% airtime
   - [ ] Serial output shows "Duty cycle limit hit" messages

6. **LoRa/Meshtastic TX**
   - [ ] S3 sends alert over LoRa
   - [ ] Other Meshtastic node receives message on private channel
   - [ ] Message contains correct SSID and RSSI

7. **Battery life**
   - [ ] Measure current draw at rest, during scan, during TX
   - [ ] Estimate real-world battery life based on scan/TX frequency

---

## Repository Structure (Suggested)

```
bushnode/
  ├── firmware/
  │   ├── s3_main.ino              # Main S3 sketch
  │   ├── c5_main.ino              # Main C5 sketch
  │   ├── lora_dutycycle.cpp       # Duty-cycle library
  │   ├── lora_dutycycle.h
  │   └── platformio.ini           # PlatformIO config (multi-env)
  ├── docs/
  │   ├── DESIGN.md                # This file
  │   ├── PINOUT.md                # S3↔C5 wiring diagram
  │   └── TROUBLESHOOTING.md
  ├── tools/
  │   └── sd_log_parser.py         # Parse SD logs into readable format
  └── README.md
```

---

## Notes

- **No beacons/probes:** Both boards RX-only. No active scanning. This reduces power, avoids triggering IDS, and keeps us quiet on shared spectrum.
- **UART simplicity:** UART is the easiest soldering path. 3 wires (TX, RX, GND). No pullups or complex sync needed.
- **S3 doesn't own storage:** C5 is the sole source-of-truth for logs. Simplifies S3 (no filesystem overhead), and if S3 reboots, scan history is safe on C5.
- **Meshtastic private channel:** Ensures bushnode alerts don't spam the default mesh. Can filter/archive separately.
- **Tuning leeway:** Scan interval (10s), duty-cycle %, target SSID list — all easy to tweak via firmware updates.
