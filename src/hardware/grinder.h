#pragma once
#include <Arduino.h>
#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>
#include <functional>
#include "../config/constants.h"

// Forward declarations
struct GrindEventData;
enum class UIGrindEvent;

class Grinder {
private:
    int motor_pin;
    int motor_pin_alt;  // Second output driven by the same RMT signal (-1 = none)
    bool grinding;
    bool initialized;

    // RMT pulse control
    rmt_channel_handle_t rmt_channel;
    rmt_encoder_handle_t current_encoder;
    bool pulse_active;
    bool rmt_initialized;

    // Motor settling tracking
    unsigned long motor_start_time;

    // Background indicator state (always compiled in)
    bool background_active;
    std::function<void(const GrindEventData&)> ui_event_callback;

    void emit_background_change(bool active);

#if DEBUG_RMT_PULSE_SELFTEST
    // Timestamp (esp_timer_get_time) and count of RMT transmission-done events, set from ISR
    static volatile int64_t selftest_done_us;   // Last callback
    static volatile int64_t selftest_first_us;  // First callback since reset to 0
    static volatile uint32_t selftest_done_count;
    static bool IRAM_ATTR selftest_on_trans_done(rmt_channel_handle_t channel,
                                                 const rmt_tx_done_event_data_t* edata, void* user_ctx);
#endif

public:
#if DEBUG_RMT_PULSE_SELFTEST
    // Pulses the motor output at several durations via start_pulse_rmt() and logs the measured
    // durations. Blocking; grinder must be disconnected from mains. See grinder_selftest.cpp.
    void run_pulse_selftest();
#endif

    // pin < 0 leaves the grinder uninitialized (motor never driven). alt_pin is mirrored
    // from the same RMT signal; see board_config.h for when it may be used.
    void init(int pin, int alt_pin = -1);
    void start();
    void stop();
    
    // RMT-based precise pulse control
    void start_pulse_rmt(uint32_t duration_ms);
    bool is_pulse_complete();
    
    bool is_grinding() const { return grinding; }
    bool is_initialized() const { return initialized; }
    bool is_motor_settled() const;

    // Background indicator setup (always compiled in)
    void set_ui_event_callback(const std::function<void(const GrindEventData&)>& callback);
};
