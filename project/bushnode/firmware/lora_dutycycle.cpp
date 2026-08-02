/**
 * LoRa Duty-Cycle Manager (SX1262 / Meshtastic-compatible)
 *
 * Token-bucket airtime limiter for US915 band. No FCC duty-cycle requirement,
 * but we self-impose ~1% to play nice with shared spectrum and protect against
 * TX-stuck firmware bugs.
 *
 * Airtime formula: time_on_air_ms = (preamble + payload) * symbol_time
 *   where symbol_time depends on spreading factor (SF) and bandwidth (BW).
 *
 * Usage:
 *   LoraDutyCycle duty(1.0);  // 1% max airtime per hour
 *   if (duty.can_transmit(payload_len, sf)) {
 *     duty.deduct(payload_len, sf);
 *     // actually send over SX1262
 *   } else {
 *     // queue or reject
 *   }
 */

#include <stdint.h>
#include <algorithm>

// LoRa SF/BW combinations. Only the ones we actually use on US915.
// US915: 64 channels (8 x 8-channel banks) + 8 uplink-only 500 kHz channels.
// We'll use SF7-SF10 with 125 kHz BW on the primary 64.
struct LoraParams {
    uint8_t spreading_factor;  // 7-12
    uint16_t bandwidth_hz;     // 125000, 250000, 500000
    uint8_t coding_rate;       // 4/5, 4/6, 4/7, 4/8 (denominator after the /)
};

class LoraDutyCycle {
private:
    // Token bucket state
    double max_airtime_percent;        // e.g., 1.0 for 1%
    uint32_t bucket_window_ms;         // How often we "reset" — typically 1 hour (3600000 ms)
    double tokens_available;           // Current tokens (in ms of airtime)
    double tokens_max;                 // Max capacity (airtime_ms per window)
    uint32_t last_refill_time;         // Timestamp of last refill

    // Metrics (optional, for logging)
    uint32_t tx_count;
    uint32_t tx_blocked_count;
    uint32_t total_airtime_ms;

public:
    LoraDutyCycle(double max_percent = 1.0, uint32_t window_ms = 1800000)
        : max_airtime_percent(max_percent),
          bucket_window_ms(window_ms),
          tokens_available(0),
          last_refill_time(0),
          tx_count(0),
          tx_blocked_count(0),
          total_airtime_ms(0)
    {
        // Pre-compute max tokens (capacity) based on window and duty cycle.
        // Default: 30 minutes (1800000 ms) window, 1% duty = 18000 ms airtime per 30 min.
        // Shorter window = more aggressive budget per cycle, but refills more often.
        tokens_max = (bucket_window_ms / 100.0) * max_airtime_percent;
        tokens_available = tokens_max;  // Start with full bucket
        last_refill_time = millis();
    }

    /**
     * Refill the token bucket based on elapsed time.
     * Called before every TX check.
     */
    void refill() {
        uint32_t now = millis();
        uint32_t elapsed = now - last_refill_time;

        if (elapsed == 0) return;  // No time passed, skip

        // Refill rate: tokens_max per bucket_window_ms
        double refill_rate = tokens_max / (double)bucket_window_ms;  // tokens/ms
        double tokens_to_add = refill_rate * elapsed;

        tokens_available = std::min(tokens_available + tokens_to_add, tokens_max);
        last_refill_time = now;
    }

    /**
     * Compute time-on-air for a LoRa packet.
     *
     * Simplified airtime formula (per Semtech appnote):
     *   T_sym = (2^SF) / BW  [symbol time in seconds]
     *   T_preamble = (n_preamble + 4.25) * T_sym
     *   T_payload = ceil(8*N_payload / (4*SF)) * (CR+4) * T_sym
     *   T_total = T_preamble + T_payload
     *
     * For Meshtastic/standard LoRa:
     *   - Preamble length: 8 symbols (standard)
     *   - CR: 4/5 (most common)
     */
    uint32_t compute_airtime_ms(uint16_t payload_len_bytes, uint8_t sf, uint16_t bw_hz = 125000) {
        if (sf < 7 || sf > 12) return 0;  // Invalid SF

        // Symbol time: T_sym = 2^SF / BW (in seconds)
        // For BW=125000, T_sym = 2^(SF-7) / 1000 (in ms)
        // Simplify for 125 kHz: T_sym_ms = 2^(SF-7) / 1.0 = 1 << (SF-7)
        double t_sym_ms;
        if (bw_hz == 125000) {
            t_sym_ms = 1 << (sf - 7);  // 1, 2, 4, 8, 16, 32 ms for SF7-12
        } else if (bw_hz == 250000) {
            t_sym_ms = (1 << (sf - 7)) / 2.0;  // Half for 250 kHz
        } else if (bw_hz == 500000) {
            t_sym_ms = (1 << (sf - 7)) / 4.0;  // Quarter for 500 kHz
        } else {
            return 0;  // Unsupported BW
        }

        // Preamble: 8 symbols (standard)
        uint32_t t_preamble_ms = (uint32_t)(8 * t_sym_ms);

        // Payload: ceil(8*N / (4*SF)) * (CR+4) * T_sym
        // Using CR=5 (code rate 4/5):
        uint32_t payload_symbols = (8 * payload_len_bytes + 4 * sf - 1) / (4 * sf);  // ceil division
        uint32_t t_payload_ms = (uint32_t)(payload_symbols * 9 * t_sym_ms);  // 9 = CR(5) + 4

        uint32_t total_ms = t_preamble_ms + t_payload_ms;
        return total_ms;
    }

    /**
     * Check if we have enough airtime budget to transmit this packet.
     */
    bool can_transmit(uint16_t payload_len_bytes, uint8_t sf, uint16_t bw_hz = 125000) {
        refill();  // Update bucket first

        uint32_t airtime_ms = compute_airtime_ms(payload_len_bytes, sf, bw_hz);
        if (airtime_ms == 0) return false;

        return tokens_available >= (double)airtime_ms;
    }

    /**
     * Deduct airtime from the bucket after a successful TX.
     * Call this AFTER the actual transmission completes.
     */
    void deduct(uint16_t payload_len_bytes, uint8_t sf, uint16_t bw_hz = 125000) {
        uint32_t airtime_ms = compute_airtime_ms(payload_len_bytes, sf, bw_hz);
        if (airtime_ms > 0) {
            tokens_available -= airtime_ms;
            total_airtime_ms += airtime_ms;
            tx_count++;
        }
    }

    /**
     * Manual reset: restore bucket to full after a burst of alerts.
     * Call this after you've logged several high-priority hits and want to
     * allow a fresh round of transmissions.
     *
     * Example: After detecting 3 target SSIDs, call reset() to refund the
     * remaining time and restart the 30-min window fresh.
     */
    void reset() {
        tokens_available = tokens_max;
        last_refill_time = millis();
        Serial.printf("[Duty] Manual reset: bucket restored to %.0f ms\n", tokens_max);
    }

    /**
     * Query remaining budget (for screen display or logging).
     */
    double get_remaining_percent() const {
        return (tokens_available / tokens_max) * 100.0;
    }

    uint32_t get_remaining_ms() const {
        return (uint32_t)tokens_available;
    }

    /**
     * Get current window elapsed time (for UI or logging).
     */
    uint32_t get_window_elapsed_ms() const {
        return millis() - last_refill_time;
    }

    uint32_t get_window_total_ms() const {
        return bucket_window_ms;
    }

    /**
     * Debug metrics.
     */
    void log_stats() {
        // Meant for Serial or HA logger.
        // Serial.printf("TX: %u sent, %u blocked | Airtime: %u ms / %u ms (%.1f%%)\n",
        //              tx_count, tx_blocked_count, total_airtime_ms, (uint32_t)tokens_max,
        //              get_remaining_percent());
    }

    // Accessors for metrics
    uint32_t get_tx_count() const { return tx_count; }
    uint32_t get_tx_blocked_count() const { return tx_blocked_count; }
    uint32_t get_total_airtime_ms() const { return total_airtime_ms; }
};

// ============================================================================
// Example usage in S3 main loop (pseudocode)
// ============================================================================
/*

LoraDutyCycle duty_cycle(1.0);  // 1% duty cycle, 30-min window (1800000 ms)
uint8_t tx_count_this_cycle = 0;

void setup() {
    // ... init SX1262, UART to C5, OLED, etc.
}

void loop() {
    // ... check UART for alerts from C5

    if (alert_queue_has_data()) {
        Alert alert = dequeue_alert();  // { ssid: "Flock", rssi: -42 }

        // Serialize alert to LoRa frame (e.g., protobuf or simple binary)
        uint8_t payload[64];
        uint16_t payload_len = serialize_alert(alert, payload);

        // Check duty cycle
        if (duty_cycle.can_transmit(payload_len, 7)) {  // SF7, 125 kHz
            // Transmit over SX1262
            bool tx_ok = lora.transmit(payload, payload_len);

            if (tx_ok) {
                duty_cycle.deduct(payload_len, 7);
                tx_count_this_cycle++;
                screen_update_tx_status("✓");

                // After N successful TXs, reset the bucket for a fresh burst
                if (tx_count_this_cycle >= 3) {  // e.g., after 3 alerts
                    duty_cycle.reset();
                    tx_count_this_cycle = 0;
                    Serial.println("[S3] Sent 3 alerts, resetting bucket");
                }
            }
        } else {
            // Queue for retry, or drop if it's low-priority
            re_queue_alert(alert);
            screen_update_tx_status(String("cooldown ") + duty_cycle.get_remaining_ms() + "ms");
        }
    }

    // ... S3's own 2.4G scan + LoRa RX, etc.
}

// Optional: Button handler to manually reset bucket
void on_button_press() {
    duty_cycle.reset();
    Serial.println("[S3] Manual reset triggered via button");
}

*/
