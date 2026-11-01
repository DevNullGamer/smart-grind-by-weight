#include "circular_buffer_math.h"
#include "../../config/debug.h"
#include "../../config/hardware.h"
#include "../../config/system.h"
#include "../sensor_health.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

#ifdef ARDUINO
#include <Arduino.h>
#include "../../config/logging.h"
#endif

#if DEBUG_LOAD_CELL
#include "../../config/constants.h"
#include <stdio.h>
#endif

uint32_t CircularBufferMath::default_millis() {
#ifdef ARDUINO
    return millis();
#else
    return 0; // Host builds have no millis(); tests inject a clock
#endif
}

CircularBufferMath::CircularBufferMath(ClockFn clock) : clock_(clock) {
    write_index = 0;
    samples_count = 0;
    display_filtered_raw = 0;
    display_filter_initialized = false;
    flow_stable_since_ms = 0;
    flow_stability_initialized = false;
    
    // Initialize buffer
    for (uint16_t i = 0; i < MAX_BUFFER_SIZE; i++) {
        circular_buffer[i].raw_value = 0;
        circular_buffer[i].timestamp_ms = 0;
    }
}

void CircularBufferMath::add_sample(int32_t raw_adc_value, uint32_t timestamp_ms) {
    // Raw ADC values should be valid 24-bit signed integers
    // We don't validate range here as different ADCs have different ranges
    
    // Add raw value directly to circular buffer (no IIR filtering)
    circular_buffer[write_index].raw_value = raw_adc_value;
    circular_buffer[write_index].timestamp_ms = timestamp_ms;
    
    // Advance write index (circular)
    write_index = (write_index + 1) % MAX_BUFFER_SIZE;
    
    // Track sample count (up to buffer size)
    if (samples_count < MAX_BUFFER_SIZE) {
        samples_count++;
    }
}

int32_t CircularBufferMath::get_instant_raw() const {
    if (samples_count == 0) return 0;
    
    // Return most recent sample
    return get_latest_sample();
}

int32_t CircularBufferMath::get_latest_sample() const {
    if (samples_count == 0) return 0;
    
    // Most recent sample is at (write_index - 1) % MAX_BUFFER_SIZE
    uint16_t latest_index = (write_index - 1 + MAX_BUFFER_SIZE) % MAX_BUFFER_SIZE;
    return circular_buffer[latest_index].raw_value;
}

// Unified smoothing method with outlier rejection on raw data
int32_t CircularBufferMath::get_smoothed_raw(uint32_t window_ms) const {
    if (samples_count == 0) return 0;
    
    int32_t samples[CBM_MAX_WINDOW_SAMPLES];
    int actual_samples = get_samples_in_window(window_ms, samples, CBM_MAX_WINDOW_SAMPLES);

    if (actual_samples == 0) {
        return get_latest_sample(); // Fallback to latest sample
    }

    // Apply outlier rejection and return smoothed result
    return apply_outlier_rejection(samples, actual_samples);
}

bool CircularBufferMath::in_window(uint32_t now_ms, uint32_t timestamp_ms, uint32_t window_ms) {
    return sensor_sample_age_ms(now_ms, timestamp_ms) <= window_ms;
}

int CircularBufferMath::get_samples_in_window(uint32_t window_ms, int32_t* samples_out, int capacity) const {
    return collect_window(window_ms, samples_out, nullptr, capacity);
}

int CircularBufferMath::collect_window(uint32_t window_ms, int32_t* values_out, uint32_t* times_out,
                                       int capacity) const {
    if (samples_count == 0 || capacity <= 0) return 0;

    uint32_t current_time = clock_();
    int collected_samples = 0;

    // Walk backwards from most recent sample, so a full array holds the newest samples
    for (int i = 0; i < samples_count; i++) {
        uint16_t index = (write_index - 1 - i + MAX_BUFFER_SIZE) % MAX_BUFFER_SIZE;

        if (!in_window(current_time, circular_buffer[index].timestamp_ms, window_ms)) {
            break; // Samples are time-ordered, so we can stop here
        }
        if (collected_samples >= capacity) {
#ifdef ARDUINO
            static bool truncation_logged = false;
            if (!truncation_logged) {
                truncation_logged = true;
                LOG_DEBUG_PRINTF("[CBM] window %lums holds more than %d samples; using the newest %d\n",
                                 (unsigned long)window_ms, capacity, capacity);
            }
#endif
            break;
        }
        values_out[collected_samples] = circular_buffer[index].raw_value;
        if (times_out) times_out[collected_samples] = circular_buffer[index].timestamp_ms;
        collected_samples++;
    }

    return collected_samples;
}

int32_t CircularBufferMath::apply_outlier_rejection(const int32_t* samples, int count) const {
    if (count == 0) return 0;
    if (count == 1) return samples[0];
    if (count == 2) return (samples[0] + samples[1]) / 2;

    if (count > CBM_MAX_WINDOW_SAMPLES) count = CBM_MAX_WINDOW_SAMPLES; // callers never exceed it

    // Decide how many to reject from each side
    int reject_each_side = 1; // (HW_LOADCELL_SAMPLE_RATE_SPS == 80) ? 8 : 1;

    int32_t sorted_samples[CBM_MAX_WINDOW_SAMPLES];

    // Not enough samples left -> fallback to median
    if (count <= 2 * reject_each_side) {
        memcpy(sorted_samples, samples, count * sizeof(int32_t));
        std::sort(sorted_samples, sorted_samples + count);
        return sorted_samples[count / 2];
    }

    // Copy and sort
    memcpy(sorted_samples, samples, count * sizeof(int32_t));
    std::sort(sorted_samples, sorted_samples + count);

    // Average trimmed values
    int samples_to_average = count - 2 * reject_each_side;
    int64_t sum = 0;
    for (int i = reject_each_side; i < count - reject_each_side; i++) {
        sum += sorted_samples[i];
    }

    return static_cast<int32_t>(sum / samples_to_average);
}

int32_t CircularBufferMath::get_raw_low_latency() const {
    return get_smoothed_raw(100); // 100ms window for real-time control
}

int32_t CircularBufferMath::get_display_raw() {
    // Asymmetric display filter on raw values (fast up, slow down)
    int32_t current_raw = get_smoothed_raw(300); // 300ms base window
    
    if (!display_filter_initialized) {
        // Do not latch the filter on an empty buffer: it would start at 0 instead of the first sample
        if (samples_count == 0) return current_raw;
        display_filtered_raw = current_raw;
        display_filter_initialized = true;
        return display_filtered_raw;
    }
    
    // Apply asymmetric filter (fast up, slow down) - adapted for raw values
    // Use deadband equivalent to ~0.01g in raw units (approximate)
    int32_t raw_deadband = 100; // This should be configurable based on calibration
    
    if (abs(current_raw - display_filtered_raw) < raw_deadband) {
        return display_filtered_raw; // No change within deadband
    }
    
    if (current_raw > display_filtered_raw) {
        // Fast response for increases
        display_filtered_raw = current_raw;
    } else {
        // Slow response for decreases
        float alpha = SYS_DISPLAY_FILTER_ALPHA_DOWN; // From constants.h
        display_filtered_raw = (int32_t)(alpha * current_raw + (1.0f - alpha) * display_filtered_raw);
    }
    
    return display_filtered_raw;
}

int32_t CircularBufferMath::get_raw_high_latency() const {
    return get_smoothed_raw(300); // 300ms window for final measurements
}

uint32_t CircularBufferMath::get_buffer_time_span_ms() const {
    if (samples_count < 2) return 0;
    
    // Time span from oldest to newest sample
    uint16_t oldest_index = (samples_count < MAX_BUFFER_SIZE) ? 0 : write_index;
    uint16_t newest_index = (write_index - 1 + MAX_BUFFER_SIZE) % MAX_BUFFER_SIZE;
    
    return circular_buffer[newest_index].timestamp_ms - circular_buffer[oldest_index].timestamp_ms;
}

int CircularBufferMath::get_sample_count_in_window(uint32_t window_ms) const {
    uint32_t current_time = clock_();
    int count = 0;
    for (int i = 0; i < samples_count; i++) {
        uint16_t index = (write_index - 1 - i + MAX_BUFFER_SIZE) % MAX_BUFFER_SIZE;
        if (!in_window(current_time, circular_buffer[index].timestamp_ms, window_ms)) break;  // Time-ordered
        count++;
    }
    return count;
}

bool CircularBufferMath::get_newest_sample_age_ms(uint32_t* age_out) const {
    if (samples_count == 0) return false;
    uint16_t latest_index = (write_index - 1 + MAX_BUFFER_SIZE) % MAX_BUFFER_SIZE;
    if (age_out) *age_out = sensor_sample_age_ms(clock_(), circular_buffer[latest_index].timestamp_ms);
    return true;
}

bool CircularBufferMath::get_window_delta(uint32_t window_ms, int32_t* delta_out,
                                          uint32_t* span_ms_out, int* samples_out) const {
    if (!delta_out || samples_count < 2) {
        if (delta_out) {
            *delta_out = 0;
        }
        if (span_ms_out) {
            *span_ms_out = 0;
        }
        if (samples_out) {
            *samples_out = 0;
        }
        return false;
    }

    uint32_t current_time = clock_();

    int collected = 0;
    int32_t newest_raw = 0;
    int32_t oldest_raw = 0;
    uint32_t newest_ts = 0;
    uint32_t oldest_ts = 0;

    for (int i = 0; i < samples_count; ++i) {
        uint16_t index = (write_index - 1 - i + MAX_BUFFER_SIZE) % MAX_BUFFER_SIZE;
        const AdcSample& sample = circular_buffer[index];

        if (!in_window(current_time, sample.timestamp_ms, window_ms)) {
            break;
        }

        if (collected == 0) {
            newest_raw = sample.raw_value;
            newest_ts = sample.timestamp_ms;
        }

        oldest_raw = sample.raw_value;
        oldest_ts = sample.timestamp_ms;
        ++collected;
    }

    if (samples_out) {
        *samples_out = collected;
    }

    if (collected < 2) {
        *delta_out = 0;
        if (span_ms_out) {
            *span_ms_out = 0;
        }
        return false;
    }

    *delta_out = newest_raw - oldest_raw;

    if (span_ms_out) {
        if (newest_ts >= oldest_ts) {
            *span_ms_out = newest_ts - oldest_ts;
        } else {
            *span_ms_out = (UINT32_MAX - oldest_ts) + newest_ts + 1;
        }
    }

    return true;
}

bool CircularBufferMath::is_settled(uint32_t window_ms, int32_t threshold_raw_units) const {
    float std_dev = get_standard_deviation_raw(window_ms);
    uint32_t newest_age_ms = 0;
    bool has_sample = get_newest_sample_age_ms(&newest_age_ms);
    bool stale = sensor_sample_is_stale(has_sample, newest_age_ms, HW_LOADCELL_STALE_SAMPLE_MS);
    uint32_t min_samples = settle_min_samples(window_ms, HW_LOADCELL_SAMPLE_RATE_SPS,
                                              HW_LOADCELL_SETTLE_MIN_SAMPLES_PERCENT,
                                              HW_LOADCELL_SETTLE_MIN_SAMPLES_FLOOR);
    bool settled = settle_check((uint32_t)get_sample_count_in_window(window_ms), min_samples, stale,
                                std_dev, (float)threshold_raw_units);
    
#if DEBUG_LOAD_CELL
    // Debug output every 1s during settling checks
    static uint32_t last_debug_time = 0;
    if (clock_() - last_debug_time > 1000) {
        // Get raw samples for display
        {
            int32_t samples[CBM_MAX_WINDOW_SAMPLES];
            int actual_samples = get_samples_in_window(window_ms, samples, CBM_MAX_WINDOW_SAMPLES);

            // Format raw samples on one line (limit to first 10 samples to avoid spam)
            char sample_str[256] = {0};
            int offset = 0;
            int samples_to_show = std::min(actual_samples, 10);
            for (int i = 0; i < samples_to_show; i++) {
                offset += snprintf(sample_str + offset, sizeof(sample_str) - offset,
                                 "%ld%s", (long)samples[i], (i < samples_to_show - 1) ? "," : "");
            }
            if (actual_samples > 10) {
                offset += snprintf(sample_str + offset, sizeof(sample_str) - offset, "...");
            }

            LOG_LOADCELL_DEBUG("[SETTLING] Window:%lums Samples:%d Raw:[%s] StdDev:%.2f Threshold:%ld Settled:%s\n",
                             window_ms, actual_samples, sample_str, std_dev, (long)threshold_raw_units,
                             settled ? "YES" : "NO");
        }
        last_debug_time = clock_();
    }
#endif

    return settled;
}

float CircularBufferMath::get_settling_confidence(uint32_t window_ms) const {
    // Calculate confidence based on standard deviation
    float std_dev = get_standard_deviation_raw(window_ms);
    
    // Confidence inversely related to standard deviation
    // This is a heuristic - may need tuning based on ADC characteristics
    float max_expected_std = 1000.0f; // Raw units
    float confidence = 1.0f - (std_dev / max_expected_std);
    
    return std::max(0.0f, std::min(1.0f, confidence));
}

float CircularBufferMath::get_standard_deviation_raw(uint32_t window_ms) const {
    int32_t samples[CBM_MAX_WINDOW_SAMPLES];
    int actual_samples = get_samples_in_window(window_ms, samples, CBM_MAX_WINDOW_SAMPLES);

    return calculate_standard_deviation(samples, actual_samples);
}

float CircularBufferMath::calculate_standard_deviation(const int32_t* samples, int count) const {
    if (count <= 1) return 0.0f;
    
    // Calculate mean
    int64_t sum = 0;
    for (int i = 0; i < count; i++) {
        sum += samples[i];
    }
    float mean = (float)sum / count;
    
    // Calculate variance
    float variance_sum = 0.0f;
    for (int i = 0; i < count; i++) {
        float diff = samples[i] - mean;
        variance_sum += diff * diff;
    }
    
    float variance = variance_sum / (count - 1);
    return sqrt(variance);
}

float CircularBufferMath::get_raw_flow_rate(uint32_t window_ms) const {
    // Only the newest and oldest in-window samples matter, so walk the ring in place (no copy)
    int collected = 0;
    uint32_t current_time = clock_();
    int32_t newest_raw = 0, oldest_raw = 0;
    uint32_t newest_ts = 0, oldest_ts = 0;

    for (int i = 0; i < (int)samples_count; i++) {
        uint16_t index = (write_index - 1 - i + MAX_BUFFER_SIZE) % MAX_BUFFER_SIZE;
        const AdcSample& sample = circular_buffer[index];

        if (!in_window(current_time, sample.timestamp_ms, window_ms)) break;
        if (collected == 0) {
            newest_raw = sample.raw_value;
            newest_ts = sample.timestamp_ms;
        }
        oldest_raw = sample.raw_value;
        oldest_ts = sample.timestamp_ms;
        collected++;
    }

    if (collected < 2) return 0.0f;

    // Simple linear regression for flow rate
    int32_t raw_change = newest_raw - oldest_raw; // Most recent - oldest
    uint32_t time_change = newest_ts - oldest_ts;

    if (time_change == 0) return 0.0f;
    
    // Return raw units per second
    return (float)raw_change * 1000.0f / time_change;
}

float CircularBufferMath::get_raw_flow_rate_95th_percentile(uint32_t window_ms) const {
    // Define parameters for the sub-window analysis
    const uint32_t MIN_SAMPLES_FOR_PERCENTILE = 10;
    const uint32_t SUB_WINDOW_MS = 300;
    const uint32_t STEP_MS = 100;
    const int MIN_SUB_WINDOWS = 4;
    const int MAX_SUB_WINDOWS = 32;
    const int MIN_SAMPLES_PER_SUB_WINDOW = 3;

    if (samples_count < MIN_SAMPLES_FOR_PERCENTILE) {
        return get_raw_flow_rate(window_ms); // Fallback for insufficient data
    }

    // Ensure the window is large enough to contain a minimum number of samples
    uint32_t min_window_for_samples = (MIN_SAMPLES_FOR_PERCENTILE * 1000) / HW_LOADCELL_SAMPLE_RATE_SPS;
    uint32_t effective_window_ms = std::max(window_ms, min_window_for_samples);

    // 1. Collect the newest in-window samples and timestamps (newest first), at most the cap.
    //    Stack use: 64 * (4 + 4) = 512 B here plus up to 32 floats below.
    int32_t sample_values[CBM_MAX_WINDOW_SAMPLES];
    uint32_t sample_times[CBM_MAX_WINDOW_SAMPLES];
    uint32_t current_time = clock_();
    int collected_samples = collect_window(effective_window_ms, sample_values, sample_times,
                                           CBM_MAX_WINDOW_SAMPLES);

    if (collected_samples < (int)MIN_SAMPLES_FOR_PERCENTILE) {
        return get_raw_flow_rate(effective_window_ms);
    }

    // 2. Calculate the number of sub-windows and the space for their flow rates.
    int num_sub_windows = (effective_window_ms > SUB_WINDOW_MS) ? 1 + (effective_window_ms - SUB_WINDOW_MS) / STEP_MS : 1;
    num_sub_windows = std::max(MIN_SUB_WINDOWS, std::min(MAX_SUB_WINDOWS, num_sub_windows));
    float flow_rates[MAX_SUB_WINDOWS];
    int valid_flow_rates_count = 0;

    // 3. Iterate through sub-windows and calculate flow rate for each. Sub-window i covers sample
    //    ages [i * STEP_MS, i * STEP_MS + SUB_WINDOW_MS] (wrap-safe, same age rule as the window).
    for (int i = 0; i < num_sub_windows; ++i) {
        uint32_t sub_window_min_age = i * STEP_MS;
        uint32_t sub_window_max_age = sub_window_min_age + SUB_WINDOW_MS;

        // Find the newest and oldest samples within this sub-window from our collected arrays
        int newest_idx = -1, oldest_idx = -1;
        for (int j = 0; j < collected_samples; ++j) {
            uint32_t age = sensor_sample_age_ms(current_time, sample_times[j]);
            if (age >= sub_window_min_age) {
                if (newest_idx == -1) newest_idx = j;
                if (age <= sub_window_max_age) {
                    oldest_idx = j;
                } else {
                    break; // Past the start of the sub-window
                }
            }
        }

        if (newest_idx != -1 && oldest_idx != -1 && (oldest_idx - newest_idx + 1) >= MIN_SAMPLES_PER_SUB_WINDOW) {
            uint32_t time_delta = sample_times[newest_idx] - sample_times[oldest_idx];
            if (time_delta > 0) {
                int32_t raw_delta = sample_values[newest_idx] - sample_values[oldest_idx];
                flow_rates[valid_flow_rates_count++] = (float)raw_delta * 1000.0f / time_delta;
            }
        }
    }

    // 4. Calculate the 95th percentile from the collected flow rates.
    if (valid_flow_rates_count >= MIN_SAMPLES_PER_SUB_WINDOW) {
        std::sort(flow_rates, flow_rates + valid_flow_rates_count);
        int percentile_95_index = static_cast<int>(valid_flow_rates_count * 0.95f);
        percentile_95_index = std::min(percentile_95_index, valid_flow_rates_count - 1);
        return flow_rates[percentile_95_index];
    }

    // Fallback if we couldn't get enough valid sub-window rates
    return get_raw_flow_rate(effective_window_ms);
}

bool CircularBufferMath::raw_flowrate_is_stable(uint32_t window_ms) const {
    // Simple stability check - compare recent flow rates
    float current_flow = get_raw_flow_rate(window_ms);
    float recent_flow = get_raw_flow_rate(window_ms / 2); // Half window
    
    // Consider stable if flow rates are within 10% of each other
    float threshold = abs(current_flow) * 0.1f;
    return abs(current_flow - recent_flow) <= threshold;
}

bool CircularBufferMath::get_min_max_raw(uint32_t window_ms, int32_t* min_out, int32_t* max_out) const {
    // Single pass over the ring, newest to oldest, no copy and no sample cap. The walk is bounded
    // by samples_count (<= MAX_BUFFER_SIZE), so the effective window is at most the buffer (~100 s
    // at 10 SPS) even when window_ms asks for more.
    int32_t min_val = 0;
    int32_t max_val = 0;
    int found = 0;
    uint32_t current_time = clock_();

    for (int i = 0; i < samples_count; i++) {
        uint16_t index = (write_index - 1 - i + MAX_BUFFER_SIZE) % MAX_BUFFER_SIZE;
        const AdcSample& sample = circular_buffer[index];
        if (!in_window(current_time, sample.timestamp_ms, window_ms)) break;  // Time-ordered

        if (found == 0 || sample.raw_value < min_val) min_val = sample.raw_value;
        if (found == 0 || sample.raw_value > max_val) max_val = sample.raw_value;
        found++;
    }

    if (min_out) *min_out = min_val;
    if (max_out) *max_out = max_val;
    return found > 0;
}

int32_t CircularBufferMath::get_min_raw(uint32_t window_ms) const {
    int32_t min_val = 0;
    int32_t max_val = 0;
    get_min_max_raw(window_ms, &min_val, &max_val);
    return min_val;
}

int32_t CircularBufferMath::get_max_raw(uint32_t window_ms) const {
    int32_t min_val = 0;
    int32_t max_val = 0;
    get_min_max_raw(window_ms, &min_val, &max_val);
    return max_val;
}

void CircularBufferMath::reset_display_filter() {
    display_filter_initialized = false;
    display_filtered_raw = 0;
}

void CircularBufferMath::clear_all_samples() {
    write_index = 0;
    samples_count = 0;
    display_filter_initialized = false;
    flow_stability_initialized = false;
    
    // Clear buffer
    for (uint16_t i = 0; i < MAX_BUFFER_SIZE; i++) {
        circular_buffer[i].raw_value = 0;
        circular_buffer[i].timestamp_ms = 0;
    }
}
