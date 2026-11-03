#pragma once

#include <stdint.h>

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
using CbmLock = portMUX_TYPE;
#define CBM_LOCK_INITIALIZER portMUX_INITIALIZER_UNLOCKED
#else
// Host builds are single-threaded: the lock is an empty shim. Host tests therefore cannot prove
// thread safety; they only pin the single-threaded behaviour of the locked code paths.
struct CbmLock {};
#define CBM_LOCK_INITIALIZER {}
#endif

/**
 * CircularBufferMath - Generic time-based mathematical operations on raw ADC data
 * 
 * This class provides time-based filtering and analysis operations on raw integer
 * data (typically ADC readings) using a circular buffer approach. It replaces the
 * weight-specific filter to provide:
 * 
 * - Full raw ADC resolution preservation (24-bit signed integers)
 * - Generic time-based mathematical operations
 * - Foundation for advanced filtering algorithms  
 * - Clear separation between raw data processing and unit conversion
 * 
 * Key Features:
 * - Large fixed circular buffer (no data loss during window changes)
 * - Time-based smoothing windows (millisecond-specified, not sample count)
 * - Outlier rejection using min/max removal
 * - Statistical analysis capabilities
 * - Ready for 10 SPS to 80+ SPS operation without code changes
 *
 * Threading: one writer (the Core 0 sampling task calls add_sample), many readers on both cores,
 * and clear_all_samples()/reset_display_filter() from the UI task. A portMUX spinlock (interrupts
 * off on the local core while held) guards the ring, its indices and the display filter state.
 * Every critical section is short and bounded: no logging, no allocation, no sorting, no calls
 * into other locked methods. Readers copy what they need (at most CBM_MAX_WINDOW_SAMPLES samples)
 * out under the lock and do the sort / statistics outside it. The only passes done in place under
 * the lock are the bounded walks (at most MAX_BUFFER_SIZE = 1024 entries: min/max, flow rate,
 * window delta, sample count), a few microseconds up to ~0.15 ms in the worst case.
 */
class CircularBufferMath {
public:
    // Millisecond clock for window and age calculations. Firmware uses millis();
    // host tests inject their own clock.
    using ClockFn = uint32_t (*)();
    static uint32_t default_millis();

    struct RawDataReading {
        int32_t raw_value;
        float confidence;        // 0.0-1.0 stability confidence  
        uint32_t settle_time_ms; // How long settling took
        bool timeout_occurred;   // True if settled due to timeout
    };
    
private:
    struct AdcSample {
        int32_t raw_value;       // Raw signed ADC reading (e.g., 24-bit HX711)
        uint32_t timestamp_ms;   // Sample timestamp
    };
    
    // Large fixed buffer - sized for 10+ seconds at 80 SPS = 800+ samples
    // Using 1024 for power-of-2 efficiency and future headroom
    static const uint16_t MAX_BUFFER_SIZE = 1024;

    static_assert((MAX_BUFFER_SIZE & (MAX_BUFFER_SIZE - 1)) == 0, "ring index math needs a power of two");

    // Scoped critical section over lock_ (portENTER_CRITICAL on firmware, no-op on host)
    class Guard {
    public:
        explicit Guard(CbmLock& lock);
        ~Guard();
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    private:
        CbmLock& lock_;
    };

    ClockFn clock_;
    mutable CbmLock lock_ = CBM_LOCK_INITIALIZER;  // guards everything below
    AdcSample circular_buffer[MAX_BUFFER_SIZE];
    uint16_t write_index;
    uint16_t samples_count;

    // Asymmetric display filter (fast up, slow down) on raw values. Writer-owned: only add_sample()
    // advances it (once per new sample); readers just load display_filtered_raw.
    int32_t display_filtered_raw;
    bool display_filter_initialized;
    // Bumped by clear_all_samples()/reset_display_filter() so an add_sample() that raced with a
    // reset does not re-seed the filter with a value computed before the reset.
    uint32_t reset_epoch;

    // Flow rate stability tracking (raw units per second)
    mutable uint32_t flow_stable_since_ms;  // When flow rate first became stable
    mutable bool flow_stability_initialized;

    // Helper methods. All callers use fixed stack arrays of CBM_MAX_WINDOW_SAMPLES (no alloca).
    // Methods ending in _locked require the caller to hold lock_.
    // Index of the i-th newest sample (0 = newest)
    uint16_t index_back_locked(int i) const;
    int32_t latest_locked() const;
    // Copies the newest in-window samples (newest first) as of `now_ms`, at most `capacity`;
    // times_out may be null; *truncated is set when more samples were in the window.
    int collect_window_locked(uint32_t now_ms, uint32_t window_ms, int32_t* values_out,
                              uint32_t* times_out, int capacity, bool* truncated) const;
    // Locking snapshot of collect_window_locked() with the clock read first. latest_out (may be
    // null) gets the newest sample (0 when empty), now_out (may be null) the clock used. Logs the
    // first truncation (outside the lock).
    int collect_window(uint32_t window_ms, int32_t* values_out, uint32_t* times_out, int capacity,
                       int32_t* latest_out = nullptr, uint32_t* now_out = nullptr) const;
    static void log_truncation_once(uint32_t window_ms, int capacity);
    void advance_display_filter_locked(int32_t smoothed_raw);
    // Trimmed mean; count must be <= CBM_MAX_WINDOW_SAMPLES (sorts a fixed-size copy)
    int32_t apply_outlier_rejection(const int32_t* samples, int count) const;
    float calculate_standard_deviation(const int32_t* samples, int count) const;
    // Wrap-safe window membership: age <= window_ms; a timestamp ahead of now has age 0
    static bool in_window(uint32_t now_ms, uint32_t timestamp_ms, uint32_t window_ms);

public:
    // Newest in-window samples, newest first, at most `capacity` (the newest are kept when the
    // window holds more; the first truncation logs one line on firmware). Returns the count.
    int get_samples_in_window(uint32_t window_ms, int32_t* samples_out, int capacity) const;

    explicit CircularBufferMath(ClockFn clock = default_millis);

    // Core data input - called from WeightSensor::sample_and_feed_filter() (Core 0 sampling task,
    // the single writer). Also advances the display filter once for this new sample.
    void add_sample(int32_t raw_adc_value, uint32_t timestamp_ms);
    
    // Unified smoothing method on raw data
    int32_t get_smoothed_raw(uint32_t window_ms) const;
    
    // Specialized raw readings with different time windows
    int32_t get_instant_raw() const;           // Latest single sample
    int32_t get_raw_low_latency() const;       // 100ms window - for real-time control
    int32_t get_display_raw() const;           // Stored asymmetric-filter value (300ms window, fed per sample) - for UI
    int32_t get_raw_high_latency() const;      // 300ms window - for final measurements
    bool get_window_delta(uint32_t window_ms, int32_t* delta_out,
                          uint32_t* span_ms_out = nullptr, int* samples_out = nullptr) const;
    
    // Raw access for diagnostics
    uint16_t get_sample_count() const;
    uint32_t get_buffer_time_span_ms() const;
    int get_sample_count_in_window(uint32_t window_ms) const;           // Samples newer than window_ms
    bool get_newest_sample_age_ms(uint32_t* age_out) const;             // False when the buffer is empty
    
    // Settling analysis - window_ms based with raw value threshold. Never settled on an empty,
    // sparse or stale buffer (see sensor_health.h)
    bool is_settled(uint32_t window_ms, int32_t threshold_raw_units) const;
    float get_settling_confidence(uint32_t window_ms) const;
    
    // Flow rate calculation in raw units per second with configurable time window
    float get_raw_flow_rate(uint32_t window_ms = 200) const;      // Default 200ms window
    float get_raw_flow_rate_95th_percentile(uint32_t window_ms = 200) const; // 95th percentile
    bool raw_flowrate_is_stable(uint32_t window_ms = 100) const;  // Check if flow rate stable
    
    // Statistical operations on raw data
    float get_standard_deviation_raw(uint32_t window_ms) const;
    // Min/max walk the ring in place (no copy, no sample cap). Every window is limited by the
    // buffer: 1024 samples = ~100 s at 10 SPS, so a longer window (e.g. the 300 s screen-dim
    // window) effectively covers only what the buffer holds.
    int32_t get_min_raw(uint32_t window_ms) const;
    int32_t get_max_raw(uint32_t window_ms) const;
    // Both in one pass; false (outputs 0) when no sample is in the window
    bool get_min_max_raw(uint32_t window_ms, int32_t* min_out, int32_t* max_out) const;
    
    // Reset functions (UI thread safe). reset_display_filter(): the next add_sample() re-seeds the
    // filter and get_display_raw() reads 0 until then. clear_all_samples() is O(1) (indices and
    // count only; stale entries are never read) and also resets the display filter.
    void reset_display_filter();
    void clear_all_samples();
};
