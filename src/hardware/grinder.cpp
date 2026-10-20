#include "grinder.h"
#include "../controllers/grind_events.h"
#include "../config/constants.h"
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_reg.h>
#include <soc/gpio_sig_map.h>
#include <soc/soc.h>
#if DEBUG_ENABLE_LOADCELL_MOCK
#include "mock_hx711_driver.h"
#endif

namespace {

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

    // Initialize RMT for all motor control (both continuous and pulse)
    rmt_tx_channel_config_t tx_chan_config = {
        .gpio_num = (gpio_num_t)motor_pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, // 1MHz resolution = 1µs per tick
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    
    if (rmt_new_tx_channel(&tx_chan_config, &rmt_channel) == ESP_OK) {
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
#if DEBUG_ENABLE_LOADCELL_MOCK
    if (!initialized) return;
    MockHX711Driver::notify_grinder_start();
    pulse_active = false;
    grinding = true;
    motor_start_time = millis();
    emit_background_change(true);
    return;
#endif
    if (!initialized || !rmt_initialized) return;

    // Reset any active pulse state when using continuous mode
    pulse_active = false;
    motor_start_time = millis();
    
    // Clean up any existing encoder
    if (current_encoder) {
        rmt_del_encoder(current_encoder);
        current_encoder = nullptr;
    }
    
    // Create copy encoder for raw symbol data
    rmt_copy_encoder_config_t encoder_config = {};
    
    if (rmt_new_copy_encoder(&encoder_config, &current_encoder) != ESP_OK) {
        return;
    }
    
    // Use RMT infinite loop for continuous grinding
    rmt_symbol_word_t continuous_data[1];
    continuous_data[0].duration0 = 32767; // Maximum 15-bit duration per symbol (~32ms at 1MHz)
    continuous_data[0].level0 = 1; // HIGH
    continuous_data[0].duration1 = 0;
    continuous_data[0].level1 = 0;
    
    rmt_transmit_config_t tx_config = {
        .loop_count = -1, // Infinite loop
    };
    
    rmt_transmit(rmt_channel, current_encoder, continuous_data, sizeof(continuous_data), &tx_config);
    grinding = true;
    emit_background_change(true);
}

void Grinder::stop() {
#if DEBUG_ENABLE_LOADCELL_MOCK
    if (!initialized) return;
    MockHX711Driver::notify_grinder_stop();
    grinding = false;
    pulse_active = false;
    emit_background_change(false);
    return;
#endif
    if (!initialized || !rmt_initialized) return;
    
    // Stop RMT transmission (works for both infinite loop and finite pulses)
    rmt_disable(rmt_channel);
    rmt_enable(rmt_channel); // Re-enable for next operation
    
    // Clean up current encoder
    if (current_encoder) {
        rmt_del_encoder(current_encoder);
        current_encoder = nullptr;
    }
    
    grinding = false;
    pulse_active = false;
    emit_background_change(false);
}

void Grinder::start_pulse_rmt(uint32_t duration_ms) {
#if DEBUG_ENABLE_LOADCELL_MOCK
    if (!initialized) return;
    MockHX711Driver::notify_pulse(duration_ms);
    pulse_active = true;
    grinding = true;
    motor_start_time = millis();
    emit_background_change(true);
    return;
#endif
    if (!initialized || !rmt_initialized) return;

    motor_start_time = millis();

    // Clean up any existing encoder
    if (current_encoder) {
        rmt_del_encoder(current_encoder);
        current_encoder = nullptr;
    }
    
    // Create copy encoder for raw symbol data
    rmt_copy_encoder_config_t encoder_config = {};
    
    if (rmt_new_copy_encoder(&encoder_config, &current_encoder) != ESP_OK) {
        return;
    }
    
    // Create RMT symbols for HIGH pulse + LOW end
    rmt_symbol_word_t pulse_symbols[2];
    uint32_t duration_us = duration_ms * 1000;
    
    // Handle long durations by using maximum duration and remainder
    if (duration_us <= 32767) {
        // Single symbol for short durations
        pulse_symbols[0].level0 = 1;
        pulse_symbols[0].duration0 = duration_us;
        pulse_symbols[0].level1 = 0;
        pulse_symbols[0].duration1 = 1; // Minimal LOW to end pulse
        
        rmt_transmit_config_t tx_config = {.loop_count = 0};
        pulse_active = true;
        grinding = true;
        
        rmt_transmit(rmt_channel, current_encoder, pulse_symbols, sizeof(rmt_symbol_word_t), &tx_config);
        emit_background_change(true);
    } else {
        // For longer durations, use loop_count to repeat
        uint32_t base_duration = 32767; // Max single symbol duration
        uint32_t loop_count = (duration_us / base_duration) - 1; // -1 because first isn't a loop
        uint32_t remainder = duration_us % base_duration;
        
        pulse_symbols[0].level0 = 1;
        pulse_symbols[0].duration0 = base_duration;
        pulse_symbols[0].level1 = 1;
        pulse_symbols[0].duration1 = remainder > 0 ? remainder : 1;
        
        pulse_symbols[1].level0 = 0;
        pulse_symbols[1].duration0 = 1; // Minimal LOW to end
        pulse_symbols[1].level1 = 0;
        pulse_symbols[1].duration1 = 0;
        
        rmt_transmit_config_t tx_config = {.loop_count = (int)loop_count};
        pulse_active = true;
        grinding = true;
        
        rmt_transmit(rmt_channel, current_encoder, pulse_symbols, sizeof(pulse_symbols), &tx_config);
        emit_background_change(true);
    }
}

bool Grinder::is_pulse_complete() {
#if DEBUG_ENABLE_LOADCELL_MOCK
    if (!pulse_active) return true;
    if (!MockHX711Driver::is_pulse_active()) {
        pulse_active = false;
        grinding = false;
        emit_background_change(false);
        return true;
    }
    return false;
#endif
    if (!pulse_active || !initialized || motor_pin < 0) return true;

    // For simplicity, we'll use a transmission done callback approach
    // Since RMT handles the pulse timing in hardware, we can check the GPIO state
    // as a simple completion indicator
    if (digitalRead(motor_pin) == LOW) {
        pulse_active = false;
        grinding = false;
        emit_background_change(false);
        return true;
    }
    
    return false;
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
