#pragma once
#include <Arduino.h>
#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <atomic>
#include <functional>
#include "../config/constants.h"

// Forward declarations
struct GrindEventData;
enum class UIGrindEvent;

class Grinder {
private:
    int motor_pin;
    int motor_pin_alt;  // Second output driven by the same RMT signal (-1 = none)
    std::atomic<bool> grinding{false};
    std::atomic<bool> initialized{false};  // Cleared by the deadman when the motor output can't be stopped cleanly

    // RMT pulse control
    rmt_channel_handle_t rmt_channel;
    rmt_encoder_handle_t current_encoder;
    std::atomic<bool> pulse_active{false};
    std::atomic<bool> rmt_initialized{false};

    // Serialises the state-changing methods (control task, UI task, esp_timer task)
    SemaphoreHandle_t mutex_ = nullptr;

    // Safety: lock-out and continuous-run deadman
    std::atomic<bool> locked_out_{false};
    std::atomic<bool> deadman_tripped_{false};              // Consumed by the grind controller
    std::atomic<bool> deadman_tripped_since_boot_{false};   // Latched, read by diagnostics
    esp_timer_handle_t deadman_timer_ = nullptr;
    std::atomic<bool> continuous_armed_{false};     // A continuous run is covered by the deadman
    std::atomic<int64_t> deadman_armed_at_us_{0};   // esp_timer_get_time() when it was armed

    static void deadman_timer_callback(void* arg);
    void deadman_trip();
    void force_motor_pins_low();
    bool halt_output();

    // Hardware work for start()/stop()/start_pulse_rmt(); caller holds mutex_
#if !DEBUG_ENABLE_LOADCELL_MOCK
    bool start_locked();
    void stop_locked();
    bool start_pulse_locked(uint32_t duration_ms);
#endif

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

    // Stops the motor and refuses start()/start_pulse_rmt() until unlock()
    void lock_out();
    void unlock();
    bool is_locked_out() const { return locked_out_; }

    // The deadman force-stopped a continuous run. consume_ returns true once per trip
    // (grind controller); the since_boot flag stays set until reboot (diagnostics).
    bool consume_deadman_trip() { return deadman_tripped_.exchange(false); }
    bool deadman_tripped_since_boot() const { return deadman_tripped_since_boot_; }

    // Background indicator setup (always compiled in)
    void set_ui_event_callback(const std::function<void(const GrindEventData&)>& callback);
};
