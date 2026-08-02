/**
 * Bushnode C5 Firmware (ESP32-C5)
 * Role: Scan 5GHz WiFi, log to SD, alert S3 over UART
 *
 * Hardware:
 *   - XIAO C5 Mini or Waveshare C5 Dev (5GHz capable)
 *   - SD card slot (primary storage)
 *   - UART to S3 (send alerts)
 *
 * Wiring (simplest UART):
 *   C5 RX (GPIO X) ← S3 TX (GPIO 43)
 *   C5 TX (GPIO Y) → S3 RX (GPIO 44)
 *   C5 GND → S3 GND
 *   (Exact GPIO pins depend on your C5 board; check pinout diagram)
 */

#include <WiFi.h>
#include <HardwareSerial.h>
#include <SD.h>
#include <SPI.h>

// ============================================================================
// CONFIGURATION
// ============================================================================

// UART to S3 (adjust GPIO based on your C5 board)
#define C5_UART_RX 9    // TODO: Verify against C5 pinout
#define C5_UART_TX 8    // TODO: Verify against C5 pinout
#define C5_UART_BAUD 115200

// SD Card (SPI)
#define SD_CS_PIN 7     // TODO: Verify against C5 pinout
#define SD_MOSI_PIN 10
#define SD_MISO_PIN 11
#define SD_CLK_PIN 12

#define TARGET_SSIDS_COUNT 3
const char* TARGET_SSIDS[TARGET_SSIDS_COUNT] = {
    "Flock Cam",
    "Metta Glasses",
    "ParkedAP_2026"
};

#define SCAN_INTERVAL_MS 10000  // Scan every 10 seconds
#define LOG_FILE "/bushnode_scan.log"

// ============================================================================
// GLOBAL STATE
// ============================================================================

HardwareSerial uart_to_s3(1);  // Serial1 on C5 (adjust if needed)
File log_file;
uint32_t last_scan_ms = 0;

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    Serial.begin(115200);  // Debug console
    delay(500);

    Serial.println("\n[C5] Bushnode starting...");

    // UART to S3
    uart_to_s3.begin(C5_UART_BAUD, SERIAL_8N1, C5_UART_RX, C5_UART_TX);
    Serial.println("[C5] UART to S3 initialized");

    // WiFi init (5GHz scan mode)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);  // Turn off station mode
    Serial.println("[C5] WiFi initialized (scan mode)");

    // SD Card init
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("[C5] SD card init FAILED");
        // Continue anyway, log to Serial instead
    } else {
        Serial.println("[C5] SD card initialized");
        open_log_file();
    }

    Serial.println("[C5] Setup complete\n");
}

// ============================================================================
// LOOP
// ============================================================================

void loop() {
    // Perform 5GHz scan periodically
    handle_5ghz_scan();

    delay(100);  // Main loop tick rate
}

// ============================================================================
// 5GHz WiFi Scan
// ============================================================================

void handle_5ghz_scan() {
    uint32_t now = millis();
    if (now - last_scan_ms < SCAN_INTERVAL_MS) return;

    last_scan_ms = now;

    // Scan 5GHz
    // Note: WiFi.scanNetworks() on ESP32-C5 should support 5GHz
    // If not, use esp_wifi_scan_start() directly with channel mask
    int n = WiFi.scanNetworks(false, false);  // async=false, show_hidden=false

    if (n == 0) {
        Serial.println("[C5] No networks found in 5GHz scan");
        log_to_sd("5GHZ_SCAN,0 networks found\n");
        return;
    }

    uint32_t timestamp = millis();

    // Log all results to SD
    log_to_sd(String("[SCAN@") + timestamp + "] 5GHz networks found: " + n + "\n");

    // Check for targets
    for (int i = 0; i < n; ++i) {
        String ssid = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);
        String bssid = WiFi.BSSIDstr(i);
        uint8_t channel = WiFi.channel(i);

        // Log all to SD (verbose)
        log_to_sd(String("  ") + ssid + " | " + bssid + " | CH" + channel + " | RSSI " + rssi + "\n");

        // Check for target matches
        for (int t = 0; t < TARGET_SSIDS_COUNT; ++t) {
            if (ssid == TARGET_SSIDS[t]) {
                // Found a target! Alert S3 immediately
                alert_s3(ssid.c_str(), rssi);
                // Also log prominently to SD
                log_to_sd(String("  [ALERT] ") + ssid + " found at RSSI " + rssi + "\n");
                Serial.printf("[C5] Target found: %s (RSSI %d)\n", ssid.c_str(), rssi);
                break;
            }
        }
    }

    WiFi.scanDelete();
}

// ============================================================================
// Alert S3 via UART
// ============================================================================

void alert_s3(const char* ssid, int8_t rssi) {
    // Format: "ALERT:<ssid>:<rssi>\n"
    // Example: "ALERT:Flock Cam:-42\n"

    String msg = "ALERT:";
    msg += ssid;
    msg += ":";
    msg += (int)rssi;
    msg += "\n";

    uart_to_s3.print(msg);
    Serial.printf("[C5] Sent to S3: %s", msg.c_str());
}

// ============================================================================
// SD Logging
// ============================================================================

void open_log_file() {
    // Check if file already exists, if not create it
    if (!SD.exists(LOG_FILE)) {
        log_file = SD.open(LOG_FILE, FILE_WRITE);
        if (log_file) {
            log_file.println("Bushnode C5 Scan Log");
            log_file.println("===================");
            log_file.printf("Started: %u ms\n", millis());
            log_file.close();
            Serial.printf("[C5] Created log file: %s\n", LOG_FILE);
        } else {
            Serial.println("[C5] Failed to create log file");
        }
    } else {
        Serial.printf("[C5] Log file exists: %s\n", LOG_FILE);
    }
}

void log_to_sd(const String& msg) {
    if (!SD.exists(LOG_FILE)) {
        Serial.println("[C5] Log file missing, skipping");
        return;
    }

    log_file = SD.open(LOG_FILE, FILE_APPEND);
    if (log_file) {
        log_file.print(msg);
        log_file.close();
    } else {
        Serial.println("[C5] Failed to open log file for writing");
    }

    // Also echo to serial for debugging
    Serial.print("[LOG] ");
    Serial.print(msg);
}

// ============================================================================
// Utilities
// ============================================================================

void log_to_sd(const char* msg) {
    log_to_sd(String(msg));
}
