/**
 * Bushnode S3 Firmware (ESP32-S3)
 * Role: Scan 2.4GHz, handle LoRa TX, listen to Meshtastic mesh
 *
 * Hardware:
 *   - Heltec V3 Wireless Stick (SX1262 LoRa radio, OLED display)
 *   - UART to C5 (RX alerts from 5GHz scanner)
 *   - No SD card (C5 owns storage)
 *
 * Wiring (simplest UART):
 *   S3 TX (GPIO 43) → C5 RX
 *   S3 RX (GPIO 44) → C5 TX
 *   S3 GND → C5 GND
 */

#include <WiFi.h>
#include <HardwareSerial.h>
#include "lora_dutycycle.h"  // Duty-cycle manager we just wrote

// ============================================================================
// CONFIGURATION
// ============================================================================

#define S3_UART_RX 44
#define S3_UART_TX 43
#define S3_UART_BAUD 115200

#define TARGET_SSIDS_COUNT 3
const char* TARGET_SSIDS[TARGET_SSIDS_COUNT] = {
    "Flock Cam",
    "Metta Glasses",
    "ParkedAP_2026"
};

#define SCAN_INTERVAL_MS 10000  // Scan every 10 seconds
#define MESHTASTIC_PRIVATE_CH 1  // Private channel ID (adjust to match your setup)

// ============================================================================
// GLOBAL STATE
// ============================================================================

HardwareSerial uart_to_c5(1);  // Serial1 on S3
LoraDutyCycle duty_cycle(1.0);  // 1% duty cycle

struct Alert {
    char ssid[32];
    int8_t rssi;
    uint32_t timestamp;
};

// Simple queue for alerts (to be relayed over LoRa)
#define ALERT_QUEUE_SIZE 16
Alert alert_queue[ALERT_QUEUE_SIZE];
uint8_t alert_queue_head = 0;
uint8_t alert_queue_tail = 0;

uint32_t last_scan_ms = 0;
int battery_percent = 100;
bool mesh_connected = false;
uint8_t mesh_neighbors = 0;

// Pairing code (displayed once on first boot)
const char* pairing_code = "ABC123";
bool pairing_shown = false;

// Manual duty-cycle reset tracking
uint8_t alerts_sent_this_burst = 0;
#define ALERTS_PER_BURST 3  // Reset bucket after 3 alerts

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);  // Debug console
    delay(500);

    Serial.println("\n[S3] Bushnode starting...");

    // UART to C5
    uart_to_c5.begin(S3_UART_BAUD, SERIAL_8N1, S3_UART_RX, S3_UART_TX);
    Serial.println("[S3] UART to C5 initialized");

    // WiFi init (for 2.4GHz scanning)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);  // Turn off station mode
    Serial.println("[S3] WiFi initialized (scan mode)");

    // TODO: Initialize SX1262 (depends on Heltec's radiolib wrapper or similar)
    // lora.begin();
    // lora.setFrequency(915000000);  // US915 center freq (one of the 64)
    // lora.setSpreadingFactor(7);
    // Serial.println("[S3] SX1262 LoRa initialized");

    // TODO: Initialize OLED display
    // display.init();
    // display_pairing_code(pairing_code);

    Serial.println("[S3] Setup complete\n");
}

// ============================================================================
// LOOP
// ============================================================================

void loop() {
    // Handle incoming alerts from C5 over UART
    handle_uart_from_c5();

    // Perform 2.4GHz scan periodically
    handle_2_4ghz_scan();

    // Process alert queue, respect duty cycle
    process_alert_queue();

    // Receive from Meshtastic mesh (TODO)
    // handle_mesh_rx();

    // Update display
    update_display();

    // Housekeeping
    update_battery();

    delay(100);  // Main loop tick rate
}

// ============================================================================
// UART: Receive alerts from C5
// ============================================================================

void handle_uart_from_c5() {
    while (uart_to_c5.available()) {
        // Expected format from C5:
        //   ALERT:<ssid>:<rssi>\n
        // Example: "ALERT:Flock Cam:-42\n"

        String line = uart_to_c5.readStringUntil('\n');
        if (line.startsWith("ALERT:")) {
            parse_alert_from_c5(line);
        } else {
            Serial.printf("[S3] Unknown UART msg: %s\n", line.c_str());
        }
    }
}

void parse_alert_from_c5(const String& line) {
    // Format: "ALERT:Flock Cam:-42"
    int colon1 = line.indexOf(':', 0);
    int colon2 = line.indexOf(':', colon1 + 1);
    int colon3 = line.indexOf(':', colon2 + 1);

    if (colon1 < 0 || colon2 < 0 || colon3 < 0) {
        Serial.println("[S3] Parse error: malformed alert");
        return;
    }

    String ssid = line.substring(colon1 + 1, colon2);
    int8_t rssi = (int8_t)line.substring(colon2 + 1, colon3).toInt();

    enqueue_alert(ssid.c_str(), rssi);
    Serial.printf("[S3] Alert from C5: %s (RSSI %d)\n", ssid.c_str(), rssi);
}

void enqueue_alert(const char* ssid, int8_t rssi) {
    if (((alert_queue_tail + 1) % ALERT_QUEUE_SIZE) == alert_queue_head) {
        // Queue full, drop oldest
        alert_queue_head = (alert_queue_head + 1) % ALERT_QUEUE_SIZE;
    }

    Alert& a = alert_queue[alert_queue_tail];
    strncpy(a.ssid, ssid, sizeof(a.ssid) - 1);
    a.rssi = rssi;
    a.timestamp = millis();

    alert_queue_tail = (alert_queue_tail + 1) % ALERT_QUEUE_SIZE;
}

bool dequeue_alert(Alert& out) {
    if (alert_queue_head == alert_queue_tail) {
        return false;  // Empty
    }

    out = alert_queue[alert_queue_head];
    alert_queue_head = (alert_queue_head + 1) % ALERT_QUEUE_SIZE;
    return true;
}

// ============================================================================
// 2.4GHz WiFi Scan
// ============================================================================

void handle_2_4ghz_scan() {
    uint32_t now = millis();
    if (now - last_scan_ms < SCAN_INTERVAL_MS) return;

    last_scan_ms = now;

    // Scan 2.4GHz
    int n = WiFi.scanNetworks(false, false);  // async=false, show_hidden=false
    if (n == 0) {
        Serial.println("[S3] No networks found in 2.4GHz scan");
        return;
    }

    // Check for targets
    for (int i = 0; i < n; ++i) {
        String ssid = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);

        for (int t = 0; t < TARGET_SSIDS_COUNT; ++t) {
            if (ssid == TARGET_SSIDS[t]) {
                // Found a target!
                enqueue_alert(ssid.c_str(), rssi);
                Serial.printf("[S3] 2.4GHz scan found: %s (RSSI %d)\n", ssid.c_str(), rssi);
                break;
            }
        }
    }

    WiFi.scanDelete();
}

// ============================================================================
// Alert Queue Processing (LoRa TX with Duty Cycle)
// ============================================================================

void process_alert_queue() {
    Alert alert;
    if (!dequeue_alert(alert)) {
        return;  // No alerts to send
    }

    // Serialize alert to LoRa payload (simple format: ALERT + ssid + rssi)
    uint8_t payload[64];
    uint16_t payload_len = serialize_alert_payload(alert, payload);

    // Check duty cycle
    if (!duty_cycle.can_transmit(payload_len, 7)) {  // SF7, 125 kHz
        // Re-queue for next cycle
        enqueue_alert(alert.ssid, alert.rssi);
        Serial.printf("[S3] Duty cycle limit hit. Remaining: %.1f%%\n",
                      duty_cycle.get_remaining_percent());
        return;
    }

    // Transmit over LoRa/Meshtastic
    bool tx_ok = transmit_lora_meshtastic(payload, payload_len);

    if (tx_ok) {
        duty_cycle.deduct(payload_len, 7);
        alerts_sent_this_burst++;
        Serial.printf("[S3] Sent alert for %s. Duty cycle: %.1f%% remaining\n",
                      alert.ssid, duty_cycle.get_remaining_percent());

        // After N alerts, reset bucket for another burst
        if (alerts_sent_this_burst >= ALERTS_PER_BURST) {
            duty_cycle.reset();
            alerts_sent_this_burst = 0;
            Serial.printf("[S3] Sent %d alerts, duty cycle reset. Ready for next burst.\n",
                          ALERTS_PER_BURST);
        }
    } else {
        // TX failed, re-queue
        enqueue_alert(alert.ssid, alert.rssi);
        Serial.println("[S3] LoRa TX failed, re-queued");
    }
}

uint16_t serialize_alert_payload(const Alert& alert, uint8_t* out) {
    // Simple format: 1 byte type + SSID (null-terminated) + 1 byte RSSI
    out[0] = 0x01;  // Alert message type
    uint16_t pos = 1;

    strncpy((char*)(out + pos), alert.ssid, 32);
    pos += strlen(alert.ssid) + 1;

    out[pos] = (uint8_t)(alert.rssi & 0xFF);
    pos++;

    return pos;
}

bool transmit_lora_meshtastic(const uint8_t* payload, uint16_t len) {
    // TODO: Integrate with actual LoRa/Meshtastic library
    // For now, stub returns true if we have a "connection"
    if (!mesh_connected) {
        Serial.println("[S3] Mesh not connected, dropping TX");
        return false;
    }

    // Pseudo-code:
    // meshtastic.sendData(payload, len, toAddress=BROADCAST, channel=MESHTASTIC_PRIVATE_CH);
    // return true;

    Serial.printf("[S3] TODO: Transmit %u bytes over Meshtastic\n", len);
    return true;
}

// ============================================================================
// Display Update
// ============================================================================

void update_display() {
    // TODO: Wire to actual OLED library
    // Roughly:
    // display.clearDisplay();
    // display.setTextSize(1);
    // display.setCursor(0, 0);
    //
    // if (!pairing_shown) {
    //     display.println("Pair: " + String(pairing_code));
    //     pairing_shown = true;
    // } else {
    //     display.printf("S:2.4  B:%d%%\n", battery_percent);
    //     // Show recent alerts
    //     // Show mesh status, LoRa TX status, etc.
    // }
    // display.display();
}

// ============================================================================
// Battery & Housekeeping
// ============================================================================

void update_battery() {
    // TODO: Read actual battery ADC (depends on Heltec's circuit)
    // battery_percent = read_battery_adc();
}

void handle_mesh_rx() {
    // TODO: Listen for incoming Meshtastic messages
    // Store neighbor count, connection status, etc.
}
