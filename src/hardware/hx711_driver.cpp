#include "hx711_driver.h"
#include "hx711_clock_mask.h"
#include "../config/constants.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>

/**
 * HX711 Driver Implementation
 * 
 * HX711 hardware communication algorithms derived from:
 * HX711_ADC library by Olav Kallhovd (olkal)
 * https://github.com/olkal/HX711_ADC
 * 
 * Original HX711_ADC library:
 * Copyright (c) 2018 Olav Kallhovd
 * Licensed under MIT License
 * 
 * Core conversion algorithms, and hardware timing
 * adapted from the original implementation for ESP32-S3 integration.
 */

HX711Driver::HX711Driver(uint8_t sck_pin, uint8_t dout_pin, int8_t sck_pin_alt)
    : sck_pin(sck_pin), sck_pin_alt((sck_pin_alt == (int8_t)sck_pin) ? (int8_t)-1 : sck_pin_alt),
      sck_mask(0), dout_pin(dout_pin), gain(1), last_raw_data(0),
      data_ready_flag(false), conversion_start_time(0), conversion_time(0),
      estimated_sample_rate_sps(HW_LOADCELL_SAMPLE_RATE_SPS) {
    // Both clock pins in GPIO0..31 can be switched with one register write, so they
    // change level at the same instant. Anything else falls back to two digitalWrite calls.
    sck_mask = hx711_clock_mask(sck_pin, this->sck_pin_alt);
}

void HX711Driver::set_sck(bool high) {
    if (sck_mask != 0) {
        // Single-cycle set/clear of all clock pins (also faster than digitalWrite)
        REG_WRITE(high ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, sck_mask);
        return;
    }
    digitalWrite(sck_pin, high ? HIGH : LOW);
    if (sck_pin_alt >= 0) {
        digitalWrite(sck_pin_alt, high ? HIGH : LOW);
    }
}

bool HX711Driver::begin() {
    return begin(128);  // Default gain
}

bool HX711Driver::begin(uint8_t gain_value) {
    // Ensure GPIO pins are properly configured for ESP32-S3
    // GPIO3 (DOUT) is a strapping pin that needs explicit configuration
    gpio_reset_pin((gpio_num_t)sck_pin);
    gpio_reset_pin((gpio_num_t)dout_pin);

    pinMode(sck_pin, OUTPUT);
    if (sck_pin_alt >= 0) {
        gpio_reset_pin((gpio_num_t)sck_pin_alt);
        pinMode(sck_pin_alt, OUTPUT);
    }
    pinMode(dout_pin, INPUT_PULLDOWN);
    set_gain(gain_value);
    power_up();
    
    // Wait for HX711 to stabilize - use sample rate appropriate delay
    uint32_t sample_interval_ms = HW_LOADCELL_SAMPLE_INTERVAL_MS;
    delay(sample_interval_ms * 2); // Wait 2 sample intervals for stabilization
    
    // Initial conversion to establish communication - use dynamic timeout
    uint32_t comm_timeout = sample_interval_ms * 2 + 200; // 2 sample intervals + margin
    LOG_BLE("HX711Driver: Waiting for first sample (timeout: %lums)\n", comm_timeout);
    
    unsigned long start_time = millis();
    while (!data_waiting_async() && millis() - start_time < comm_timeout) {
        delay(sample_interval_ms / 4); // Poll at 4x the sample rate
    }
    
    if (data_waiting_async()) {
        if (!update_async()) {
            LOG_BLE("HX711Driver: Failed to read first sample\n");
            return false;
        }

        // After a successful read the HX711 should release DOUT HIGH until the next conversion.
        // If it remains LOW we likely don't have a real HX711 connected (pulldown holding the line).
        delayMicroseconds(10);
        if (digitalRead(dout_pin) == LOW) {
            LOG_BLE("HX711Driver: DOUT stuck LOW after first read - HX711 not connected?\n");
            return false;
        }

        data_ready_flag = true;
        LOG_BLE("HX711Driver: First sample acquired successfully\n");
        return true;
    }
    
    LOG_BLE("HX711Driver: Timeout waiting for first sample\n");
    return false;
}

void HX711Driver::set_gain(uint8_t gain_value) {
    if (gain_value <= 32) {
        gain = 2;      // 32 gain, channel B
    } else if (gain_value <= 64) {
        gain = 3;      // 64 gain, channel A  
    } else {
        gain = 1;      // 128 gain, channel A
    }
}

void HX711Driver::power_up() {
    power_up_sequence();
}

void HX711Driver::power_down() {
    power_down_sequence();
}

void HX711Driver::power_up_sequence() {
    // Ensure SCK is configured as GPIO output before toggling (may be called before begin())
    pinMode(sck_pin, OUTPUT);
    if (sck_pin_alt >= 0) pinMode(sck_pin_alt, OUTPUT);
    set_sck(false);
    delayMicroseconds(100);  // Ensure clean power up
}

void HX711Driver::power_down_sequence() {
    // Ensure SCK is configured as GPIO output before toggling (may be called before begin())
    pinMode(sck_pin, OUTPUT);
    if (sck_pin_alt >= 0) pinMode(sck_pin_alt, OUTPUT);
    set_sck(false);
    set_sck(true);
    delayMicroseconds(100);  // Hold high for >60μs to enter power down
}

bool HX711Driver::is_ready() {
    return digitalRead(dout_pin) == LOW;
}

bool HX711Driver::data_waiting_async() {
    return is_ready();
}

bool HX711Driver::update_async() {
    if (!is_ready()) {
        return false;
    }
    
    conversion_24bit();
    return true;
}

void HX711Driver::conversion_24bit() {
    // Record conversion timing
    unsigned long now = micros();
    if (conversion_start_time == 0) {
        conversion_time = 0;
    } else {
        conversion_time = now - conversion_start_time;
    }
    conversion_start_time = now;
    
    uint32_t raw_data = 0;  // Use explicit 32-bit unsigned for ESP32 consistency
    
    // HX711_ADC interrupt protection: Disable interrupts during critical bit-bang conversion
    // This prevents BLE and other interrupts from disrupting the precise HX711 timing
    noInterrupts();
    
    // Read 24 bits of data + gain bits
    for (uint8_t i = 0; i < (24 + gain); i++) {
        set_sck(true);
        if (SCK_DELAY) delayMicroseconds(1);
        set_sck(false);
        
        if (i < 24) {
            raw_data = (raw_data << 1) | digitalRead(dout_pin);
        }
    }
    
    // Re-enable interrupts immediately after conversion
    interrupts();
    
    // HX711_ADC exact data processing: normalize HX711's offset binary output
    // HX711 natural range: 0x800000 to 0x7FFFFF
    // XOR converts to:     0x000000 to 0xFFFFFF
    raw_data = raw_data ^ 0x800000;
    
    // HX711_ADC data validation
    if (raw_data > 0xFFFFFF) {
        // Data out of range - this shouldn't happen with proper 24-bit data
        LOG_BLE("HX711Driver: Data out of range - raw=0x%08lx\n", raw_data);
        return; // Skip this invalid reading
    }
    
    last_raw_data = (int32_t)raw_data;  // Explicit cast to int32_t for consistency
    data_ready_flag = true;
}

int32_t HX711Driver::get_raw_data() const {
    return last_raw_data;
}

bool HX711Driver::validate_hardware() {
    // Simple hardware validation - check if we can get readings
    // Calculate timeout based on sample rate: need time for 3 samples plus margin
    uint32_t sample_interval_ms = HW_LOADCELL_SAMPLE_INTERVAL_MS;
    uint32_t validation_timeout = (sample_interval_ms * 4) + 500; // 4 sample intervals + 500ms margin
    
    LOG_BLE("HX711Driver: Hardware validation timeout = %lums (sample rate: %d SPS)\n", 
           validation_timeout, HW_LOADCELL_SAMPLE_RATE_SPS);
    
    unsigned long start_time = millis();
    uint64_t conversion_time_sum = 0;
    int conversion_time_samples = 0;
    int successful_reads = 0;
    
    while (millis() - start_time < validation_timeout && successful_reads < 3) {
        if (data_waiting_async()) {
            if (update_async()) {
                if (conversion_time > 0) {
                    conversion_time_sum += conversion_time;
                    conversion_time_samples++;
                }
                successful_reads++;
                LOG_BLE("HX711Driver: Validation read %d/3 successful\n", successful_reads);
            }
        }
        delay(sample_interval_ms / 4); // Poll at 4x the sample rate
    }
    
    if (conversion_time_samples > 0) {
        double avg_conversion_us = static_cast<double>(conversion_time_sum) / conversion_time_samples;
        if (avg_conversion_us > 0.0) {
            estimated_sample_rate_sps = static_cast<float>(1'000'000.0 / avg_conversion_us);
        }
    } else {
        estimated_sample_rate_sps = HW_LOADCELL_SAMPLE_RATE_SPS;
    }
    
    LOG_BLE("HX711Driver: Hardware validation completed - %d/3 successful reads in %lums (rate ≈ %.1f SPS)\n", 
           successful_reads, millis() - start_time, estimated_sample_rate_sps);
    
    return successful_reads >= 3;
}

uint8_t HX711Driver::get_current_gain() const {
    switch (gain) {
        case 1: return 128;
        case 2: return 32;
        case 3: return 64;
        default: return 128;
    }
}
