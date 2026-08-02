/**
 * LoRa Duty-Cycle Manager (SX1262 / Meshtastic-compatible)
 * Header file for lora_dutycycle.cpp
 */

#ifndef LORA_DUTYCYCLE_H
#define LORA_DUTYCYCLE_H

#include <stdint.h>

class LoraDutyCycle {
private:
    double max_airtime_percent;
    uint32_t bucket_window_ms;
    double tokens_available;
    double tokens_max;
    uint32_t last_refill_time;
    uint32_t tx_count;
    uint32_t tx_blocked_count;
    uint32_t total_airtime_ms;

public:
    LoraDutyCycle(double max_percent = 1.0, uint32_t window_ms = 1800000);

    void refill();
    uint32_t compute_airtime_ms(uint16_t payload_len_bytes, uint8_t sf, uint16_t bw_hz = 125000);
    bool can_transmit(uint16_t payload_len_bytes, uint8_t sf, uint16_t bw_hz = 125000);
    void deduct(uint16_t payload_len_bytes, uint8_t sf, uint16_t bw_hz = 125000);
    void reset();

    double get_remaining_percent() const;
    uint32_t get_remaining_ms() const;
    uint32_t get_window_elapsed_ms() const;
    uint32_t get_window_total_ms() const;

    void log_stats();
    uint32_t get_tx_count() const;
    uint32_t get_tx_blocked_count() const;
    uint32_t get_total_airtime_ms() const;
};

#endif  // LORA_DUTYCYCLE_H
