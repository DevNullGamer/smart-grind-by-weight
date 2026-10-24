#include "grinder.h"
#include "../controllers/grind_events.h"
#include "../config/constants.h"
#include "rmt_pulse_math.h"
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <soc/gpio_reg.h>
#include <soc/gpio_sig_map.h>
#include <soc/soc.h>
#if DEBUG_ENABLE_LOADCELL_MOCK
#include "mock_hx711_driver.h"
#endif

namespace {

constexpr uint32_t kMutexWaitMs = HW_GRINDER_MUTEX_WAIT_MS;
constexpr uint32_t kDeadmanMutexWaitMs = HW_GRINDER_DEADMAN_MUTEX_WAIT_MS;
// A timer callback firing earlier than this before the full timeout belongs to a previous run
constexpr int64_t kDeadmanStaleSlackUs = 200000;

// Logs a refused command at most once per second (PRIME re-issues start() every control cycle)
void log_refused(const char* what) {
    static uint32_t last_log_ms = 0;
    const uint32_t now = millis();
    if (last_log_ms == 0 || now - last_log_ms >= 1000) {
        last_log_ms = now;
        LOG_BLE("[Grinder] %s\n", what);
    }
}

// Takes the mutex for the lifetime of the object; held() is false if it could not be taken in time.
class MutexGuard {
public:
    MutexGuard(SemaphoreHandle_t mutex, uint32_t timeout_ms)
        : mutex_(mutex), held_(mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {}
    ~MutexGuard() {
        if (held_) xSemaphoreGive(mutex_);
    }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;
    bool held() const { return held_; }

private:
    SemaphoreHandle_t mutex_;
    bool held_;
};

uint32_t continuous_deadman_timeout_ms() {
#if DEBUG_GRINDER_DEADMAN_TEST
    return 3000;
#else
    return continuous_deadman_ms(GRIND_TIMEOUT_SEC, HW_GRINDER_DEADMAN_MARGIN_MS);
#endif
}

// Routes the RMT signal that drives `source_pin` to `alt_pin` as well through the GPIO
// matrix, so both pins switch together (idle level included). Returns false when
// `source_pin` is not driven by a peripheral signal.
bool mirror_output_signal(int source_pin, int alt_pin) {
    const uint32_t signal = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * source_pin) & GPIO_FUNC0_OUT_SEL_M;
    if (signal >= SIG_GPIO_OUT_IDX) {
        return false;
    }
    // Pin is still the parked input (pull-down): preload LOW, then make it an output and
    // connect it, so it never glitches high. No gpio_reset_pin (it would enable the pull-up).
    gpio_set_level((gpio_num_t)alt_pin, 0);
    esp_rom_gpio_pad_select_gpio(alt_pin);
    gpio_set_direction((gpio_num_t)alt_pin, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(alt_pin, signal, false, false);
    return true;
}

}  // namespace

void Grinder::init(int pin, int alt_pin) {
    motor_pin = pin;
    motor_pin_alt = alt_pin;
    grinding = false;
    pulse_active = false;
    rmt_initialized = false;
    current_encoder = nullptr;
    motor_start_time = 0;
    initialized = false;
    locked_out_ = false;
    deadman_tripped_ = false;
    deadman_tripped_since_boot_ = false;
    if (!mutex_) {
        mutex_ = xSemaphoreCreateMutex();
        if (!mutex_) {
            LOG_BLE("[Grinder] ERROR: mutex creation failed - motor disabled\n");
            return;
        }
    }

    // Initialize background indicator
    background_active = false;
    ui_event_callback = nullptr;

#if DEBUG_ENABLE_LOADCELL_MOCK
    initialized = true;
    return;
#endif

    // No motor pin (board unknown or not confirmed): stay uninitialized, motor never driven
    if (motor_pin < 0) {
        motor_pin_alt = -1;
        return;
    }

    // Motor chokepoint: independent of board_config_for(), the board must have been
    // established by probe or user and every pin must pass the allow-list.
    const BoardConfig& active = board();
    const bool established = active.source == BoardSource::PROBE || active.source == BoardSource::USER;
    if (!established || !board_motor_pin_allowed(active.variant, motor_pin)) {
        LOG_BLE("[Grinder] Motor pin %d rejected for board %s - motor disabled\n", motor_pin, active.name());
        motor_pin = -1;
        motor_pin_alt = -1;
        return;
    }
    if (motor_pin_alt >= 0 && !board_motor_pin_allowed(active.variant, motor_pin_alt)) {
        LOG_BLE("[Grinder] Alt motor pin %d rejected for board %s\n", motor_pin_alt, active.name());
        motor_pin_alt = -1;
    }

    if (!deadman_timer_) {
        esp_timer_create_args_t timer_args = {
            .callback = &Grinder::deadman_timer_callback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "motor_deadman",
            .skip_unhandled_events = false,
        };
        if (esp_timer_create(&timer_args, &deadman_timer_) != ESP_OK) {
            LOG_BLE("[Grinder] ERROR: deadman timer creation failed - motor disabled\n");
            return;
        }
    }

    // Initialize RMT for all motor control (both continuous and pulse)
    rmt_tx_channel_config_t tx_chan_config = {
        .gpio_num = (gpio_num_t)motor_pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, // 1MHz resolution = 1µs per tick
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    
    if (rmt_new_tx_channel(&tx_chan_config, &rmt_channel) == ESP_OK) {
#if DEBUG_RMT_PULSE_SELFTEST
        // Must be registered before rmt_enable
        rmt_tx_event_callbacks_t selftest_callbacks = {};
        selftest_callbacks.on_trans_done = selftest_on_trans_done;
        const esp_err_t cb_err = rmt_tx_register_event_callbacks(rmt_channel, &selftest_callbacks, nullptr);
        if (cb_err != ESP_OK) {
            LOG_BLE("[Grinder] Self-test callback registration failed: %d (%s)\n", (int)cb_err, esp_err_to_name(cb_err));
        }
#endif
        rmt_enable(rmt_channel);
        rmt_initialized = true;
        initialized = true;

        if (motor_pin_alt >= 0) {
            if (mirror_output_signal(motor_pin, motor_pin_alt)) {
                LOG_BLE("[Grinder] Motor on GPIO%d + GPIO%d\n", motor_pin, motor_pin_alt);
            } else {
                LOG_BLE("[Grinder] Alt motor pin %d not connected (no RMT signal)\n", motor_pin_alt);
                motor_pin_alt = -1;
            }
        } else {
            LOG_BLE("[Grinder] Motor on GPIO%d\n", motor_pin);
        }
    }
}

void Grinder::start() {
    bool started = false;
    const char* refused = nullptr;
    {
        MutexGuard lock(mutex_, kMutexWaitMs);
        if (!lock.held()) {
            refused = "start refused: mutex busy";
        } else if (locked_out_) {
            refused = "start refused: locked out";
        } else {
#if DEBUG_ENABLE_LOADCELL_MOCK
            if (!initialized) return;
            MockHX711Driver::notify_grinder_start();
            pulse_active = false;
            grinding = true;
            motor_start_time = millis();
            started = true;
#else
            started = start_locked();
#endif
        }
    }
    if (refused) {
        log_refused(refused);
    }
    if (started) {
        emit_background_change(true);
    }
}

#if !DEBUG_ENABLE_LOADCELL_MOCK
bool Grinder::start_locked() {
    if (!initialized || !rmt_initialized) return false;

    // Never delete the encoder or queue a continuous transaction under a live pulse
    if (pulse_active && !halt_output()) {
        return false;
    }

    motor_start_time = millis();

    // Clean up any existing encoder
    if (current_encoder) {
        rmt_del_encoder(current_encoder);
        current_encoder = nullptr;
    }

    // Create copy encoder for raw symbol data
    rmt_copy_encoder_config_t encoder_config = {};

    if (rmt_new_copy_encoder(&encoder_config, &current_encoder) != ESP_OK) {
        return false;
    }

    // Arm the deadman before the motor can run; no timer means no start
    const uint32_t deadman_ms = continuous_deadman_timeout_ms();
    if (!deadman_timer_) {
        LOG_BLE("[Grinder] start refused: no deadman timer\n");
        return false;
    }
    esp_timer_stop(deadman_timer_);
    if (esp_timer_start_once(deadman_timer_, (uint64_t)deadman_ms * 1000) != ESP_OK) {
        LOG_BLE("[Grinder] start refused: deadman arm failed\n");
        return false;
    }
    deadman_armed_at_us_ = esp_timer_get_time();
    continuous_armed_ = true;

    // Use RMT infinite loop for continuous grinding
    continuous_symbol_.duration0 = 32767; // Maximum 15-bit duration per symbol (~32ms at 1MHz)
    continuous_symbol_.level0 = 1; // HIGH
    continuous_symbol_.duration1 = 0;
    continuous_symbol_.level1 = 0;

    rmt_transmit_config_t tx_config = {
        .loop_count = -1, // Infinite loop
    };

    pulse_active = false;  // Continuous mode replaces any pulse state
    const esp_err_t tx_err = rmt_transmit(rmt_channel, current_encoder, &continuous_symbol_,
                                          sizeof(continuous_symbol_), &tx_config);
    if (tx_err != ESP_OK) {
        LOG_BLE("[Grinder] start refused: rmt_transmit failed: %d (%s)\n", (int)tx_err, esp_err_to_name(tx_err));
        grinding = false;
        halt_output();  // Disarms the deadman, idles the output (forces the pin low if that fails)
        return false;
    }
    grinding = true;
    return true;
}
#endif

void Grinder::stop() {
    bool unlocked_stop = false;
    {
        // The stop path must always run: continue without the lock if it can't be taken
        MutexGuard lock(mutex_, kMutexWaitMs);
        unlocked_stop = !lock.held();
#if DEBUG_ENABLE_LOADCELL_MOCK
        if (!initialized) return;
        MockHX711Driver::notify_grinder_stop();
#else
        stop_locked();
#endif
        grinding = false;
        pulse_active = false;
    }
    if (unlocked_stop) {
        LOG_BLE("[Grinder] stop without lock (mutex busy)\n");
    }
    emit_background_change(false);
}

// Stops the RMT output and verifies it; only then is the deadman cancelled. If the output cannot
// be stopped it is cut at the pin and the motor stays dead until reboot (flags latched for the
// controller and diagnostics). Uses esp_rom_printf only so it is usable from any context.
bool Grinder::halt_output() {
    if (initialized && rmt_initialized) {
        // INVALID_STATE on disable: channel already stopped (e.g. racing the deadman)
        const esp_err_t disable_err = rmt_disable(rmt_channel);
        const esp_err_t enable_err = rmt_enable(rmt_channel); // Re-enable for next operation
        if ((disable_err != ESP_OK && disable_err != ESP_ERR_INVALID_STATE) || enable_err != ESP_OK) {
            esp_rom_printf("[Grinder] RMT stop failed: disable=%d enable=%d - motor disabled until reboot\n",
                           (int)disable_err, (int)enable_err);
            force_motor_pins_low();
            initialized = false;
            rmt_initialized = false;
            deadman_tripped_since_boot_ = true;
            deadman_tripped_ = true;
            return false;
        }
    }
    if (deadman_timer_) {
        esp_timer_stop(deadman_timer_);  // ESP_ERR_INVALID_STATE when not armed is expected
    }
    continuous_armed_ = false;
    return true;
}

#if !DEBUG_ENABLE_LOADCELL_MOCK
void Grinder::stop_locked() {
    halt_output();

    // Clean up current encoder
    if (current_encoder) {
        rmt_del_encoder(current_encoder);
        current_encoder = nullptr;
    }
}
#endif

void Grinder::start_pulse_rmt(uint32_t duration_ms) {
    if (duration_ms == 0 || duration_ms > HW_GRINDER_MAX_PULSE_MS) {
        LOG_BLE("[Grinder] Pulse refused: %lu ms outside 1-%d ms\n", (unsigned long)duration_ms, HW_GRINDER_MAX_PULSE_MS);
        return;
    }

    bool started = false;
    const char* refused = nullptr;
    {
        MutexGuard lock(mutex_, kMutexWaitMs);
        if (!lock.held()) {
            refused = "Pulse refused: mutex busy";
        } else if (locked_out_) {
            refused = "Pulse refused: locked out";
        } else {
#if DEBUG_ENABLE_LOADCELL_MOCK
            if (!initialized) return;
            MockHX711Driver::notify_pulse(duration_ms);
            pulse_active = true;
            grinding = true;
            motor_start_time = millis();
            started = true;
#else
            started = start_pulse_locked(duration_ms);
#endif
        }
    }
    if (refused) {
        log_refused(refused);
    }
    if (started) {
        emit_background_change(true);
    }
}

#if !DEBUG_ENABLE_LOADCELL_MOCK
bool Grinder::start_pulse_locked(uint32_t duration_ms) {
    if (!initialized || !rmt_initialized) return false;

    // A pulse must not run on top of an armed continuous run: stop that first (cancels the deadman)
    if (continuous_armed_ && !halt_output()) {
        return false;
    }

    // The payload buffer is shared: never rewrite it under a pulse that may still be transmitting
    if (pulse_active && !halt_output()) {
        return false;
    }

    motor_start_time = millis();

    // Clean up any existing encoder
    if (current_encoder) {
        rmt_del_encoder(current_encoder);
        current_encoder = nullptr;
    }

    // Create copy encoder for raw symbol data
    rmt_copy_encoder_config_t encoder_config = {};

    if (rmt_new_copy_encoder(&encoder_config, &current_encoder) != ESP_OK) {
        return false;
    }

    // Exact payload: HIGH for duration_ms then a 1 us LOW tail, sent as one plain transmission
    // (no loop_count, which repeated the remainder and tail on every pass). The buffer is a
    // member because rmt_transmit() reads it after returning.
    RmtSymbol symbols[RMT_PULSE_SYMBOL_CAPACITY];
    const size_t symbol_count = rmt_build_pulse_symbols(duration_ms * 1000, symbols, RMT_PULSE_SYMBOL_CAPACITY);
    if (symbol_count == 0) {
        LOG_BLE("[Grinder] Pulse refused: no RMT payload for %lu ms\n", (unsigned long)duration_ms);
        return false;
    }
    // Field by field: rmt_symbol_word_t is a bitfield union (15-bit durations, 1-bit levels), so
    // the pure RmtSymbol cannot be copied or cast into it
    for (size_t i = 0; i < symbol_count; i++) {
        pulse_symbols_[i].level0 = symbols[i].level0;
        pulse_symbols_[i].duration0 = symbols[i].duration0;
        pulse_symbols_[i].level1 = symbols[i].level1;
        pulse_symbols_[i].duration1 = symbols[i].duration1;
    }

    rmt_transmit_config_t tx_config = {.loop_count = 0};
    pulse_active = true;
    grinding = true;

    const esp_err_t tx_err = rmt_transmit(rmt_channel, current_encoder, pulse_symbols_,
                                          symbol_count * sizeof(rmt_symbol_word_t), &tx_config);
    if (tx_err != ESP_OK) {
        LOG_BLE("[Grinder] Pulse refused: rmt_transmit failed: %d (%s)\n", (int)tx_err, esp_err_to_name(tx_err));
        pulse_active = false;
        grinding = false;
        halt_output();  // Idles the output (forces the pin low if that fails)
        return false;
    }
    return true;
}
#endif

bool Grinder::is_pulse_complete() {
    bool complete = false;
    {
        MutexGuard lock(mutex_, kMutexWaitMs);
        if (!lock.held()) {
            return false;  // Retry next cycle
        }
#if DEBUG_ENABLE_LOADCELL_MOCK
        if (!pulse_active) return true;
        if (MockHX711Driver::is_pulse_active()) return false;
        complete = true;
#else
        if (!pulse_active || !initialized || motor_pin < 0) return true;

        // RMT handles the pulse timing in hardware, so the GPIO state is the completion indicator
        if (digitalRead(motor_pin) != LOW) return false;
        complete = true;
#endif
        pulse_active = false;
        grinding = false;
    }
    if (complete) {
        emit_background_change(false);
    }
    return true;
}

void Grinder::lock_out() {
    locked_out_ = true;
    stop();
    LOG_BLE("[Grinder] Locked out\n");
}

void Grinder::unlock() {
    locked_out_ = false;
    LOG_BLE("[Grinder] Unlocked\n");
}

void Grinder::force_motor_pins_low() {
    const int pins[] = {motor_pin, motor_pin_alt};
    for (int pin : pins) {
        if (pin < 0) continue;
        esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)pin, 0);
    }
}

void Grinder::deadman_timer_callback(void* arg) {
    static_cast<Grinder*>(arg)->deadman_trip();
}

// Runs in the esp_timer task. Uses esp_rom_printf only (no LOG_*), and proceeds without the
// mutex when it cannot be taken quickly: stopping the motor outranks consistency.
void Grinder::deadman_trip() {
    MutexGuard lock(mutex_, kDeadmanMutexWaitMs);

    // Not armed: stop() got in between the timer firing and us running. Output dead: nothing to do.
    if (!continuous_armed_ || !rmt_initialized) {
        return;
    }
    // Armed, but too recently: this callback belongs to a previous run that was stopped and restarted
    const int64_t timeout_us = (int64_t)continuous_deadman_timeout_ms() * 1000;
    if (esp_timer_get_time() - deadman_armed_at_us_ < timeout_us - kDeadmanStaleSlackUs) {
        return;
    }

    esp_rom_printf("[Grinder] DEADMAN: continuous run exceeded %d ms, forcing motor off\n",
                   (int)continuous_deadman_timeout_ms());

    halt_output();  // Escalates to pins low + motor dead on failure

    grinding = false;
    pulse_active = false;
    deadman_tripped_since_boot_ = true;
    deadman_tripped_ = true;
}

bool Grinder::is_motor_settled() const {
    // Return true if sufficient time has passed since motor start
    if (motor_start_time == 0) {
        return false;  // Motor has never started
    }
    return (millis() - motor_start_time) >= HW_GRINDER_SETTLING_TIME_MS;
}

void Grinder::set_ui_event_callback(const std::function<void(const GrindEventData&)>& callback) {
    ui_event_callback = callback;
}

void Grinder::emit_background_change(bool active) {
    if (background_active == active) {
        return; // No change
    }
    
    background_active = active;
    
    if (ui_event_callback) {
        // Properly initialize all required fields to prevent null pointer crashes
        GrindEventData event_data = {};
        event_data.event = UIGrindEvent::BACKGROUND_CHANGE;
        event_data.phase = GrindPhase::IDLE;  // Safe default
        event_data.current_weight = 0.0f;
        event_data.progress_percent = 0;
        event_data.phase_display_text = "BACKGROUND";  // Safe string for logging
        event_data.show_taring_text = false;
        event_data.background_active = active;
        
        ui_event_callback(event_data);
        
        LOG_BLE("[Grinder] Background change: %s\n", active ? "ACTIVE" : "INACTIVE");
    }
}
