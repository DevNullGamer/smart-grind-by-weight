#pragma once
#include <Preferences.h>
#include <cstdint>
#include "statistics_snapshot.h"

// Manages lifetime statistics stored in NVS (Preferences namespace: "stats")
// Tracks usage, mode-specific, and quality/performance metrics across reboots.
class StatisticsManager {
public:
    void init(Preferences* prefs);

    // Update methods - called by various system components
    void update_grind_session(float final_weight, float error_grams, uint8_t pulse_count,
                              bool is_weight_mode, uint32_t motor_time_ms);
    void update_motor_test(uint32_t duration_ms);
    void update_time_pulse();
    void update_uptime(uint32_t minutes_to_add);

    // Retrieval methods
    uint32_t get_total_grinds() const;
    uint32_t get_single_shots() const;
    uint32_t get_double_shots() const;
    uint32_t get_custom_shots() const;
    uint32_t get_motor_runtime_sec() const;
    uint64_t get_motor_runtime_ms() const;
    uint32_t get_device_uptime_hrs() const;
    uint32_t get_device_uptime_min_remainder() const;
    float get_total_weight_kg() const;
    uint32_t get_weight_mode_grinds() const;
    uint32_t get_time_mode_grinds() const;
    uint32_t get_time_pulses() const;
    float get_avg_accuracy_g() const;
    uint32_t get_total_pulses() const;
    float get_avg_pulses() const;

    // All counters copied under ONE lock (no torn read across a persisting grind); no logging or allocation inside
    StatisticsSnapshot get_snapshot() const;

    // Reset operations
    void reset_all();             // Clear all statistics (factory reset)
    void reset_statistics_only(); // Clear statistics but keep device config

private:
    void load_from_storage();
    void migrate_from_legacy(Preferences& stats_prefs);
    void add_motor_runtime_ms_locked(uint32_t additional_ms);
    void add_uptime_minutes_locked(uint32_t minutes_to_add);
    bool persist_locked(bool force);
    void mark_dirty_locked();

    bool initialized_ = false;
    mutable StatisticsSnapshot snapshot_{};
    bool dirty_ = false;
    uint32_t last_flush_ms_ = 0;
    uint32_t pending_pulse_flush_counter_ = 0;
};

// Global instance
extern StatisticsManager statistics_manager;
