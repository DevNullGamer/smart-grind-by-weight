#pragma once
#include <cstdint>

// Represents the persistent snapshot of lifetime statistics (plain data, no Arduino dependencies, so the pure
// web-panel conversion in network/portal_snapshot.* can be unit tested on the host).
struct StatisticsSnapshot {
    static constexpr uint32_t kVersion = 2;

    uint32_t version = kVersion;
    uint32_t total_grinds = 0;
    uint32_t single_shots = 0;
    uint32_t double_shots = 0;
    uint32_t custom_shots = 0;
    uint32_t motor_runtime_sec = 0;
    uint32_t motor_runtime_ms_remainder = 0;
    uint32_t device_uptime_hrs = 0;
    uint32_t device_uptime_min_remainder = 0;
    float total_weight_kg = 0.0f;
    uint32_t weight_mode_grinds = 0;
    uint32_t time_mode_grinds = 0;
    uint32_t time_pulses = 0;
    uint32_t total_pulses = 0;
    uint32_t accuracy_sample_count = 0;
    float accuracy_sum = 0.0f;
    uint32_t pulse_sample_count = 0;
    float pulse_sum = 0.0f;
    uint32_t reserved0 = 0; // Reserved for future expansion / alignment
    uint32_t reserved1 = 0;
};
