#pragma once
#include <stddef.h>
#include <stdint.h>
#include "system/state_machine.h"

// Pure screen power logic (dim / off decision, input swallow rule, timeout settings model). No
// Arduino/ESP/LVGL dependencies, unit tested on the host. ScreenTimeoutController gathers the inputs.

enum class ScreenLevel : uint8_t { AWAKE, DIMMED, OFF };

struct ScreenPowerInput {
    uint32_t ms_since_touch;                  // since the last touch
    uint32_t ms_since_weight_activity;        // since a weight change of at least 1 g
    uint32_t ms_since_strong_weight_activity; // since a weight change of at least 2 g (wakes from OFF)
    uint32_t dim_after_ms;                    // 0 = never dim
    uint32_t off_after_ms;                    // 0 = never turn off
    bool keep_awake;                          // grinding, update hold, remote-start lead-in, motor test: blocks dim and off
    bool keep_on;                             // blocks OFF only (never-off state, WiFi page, error diagnostic)
    ScreenLevel current;                      // level in force now
};

// Stateless: touch or weight activity drops the idle time below every threshold, so wake is immediate.
// While OFF only a strong weight change counts as weight activity.
ScreenLevel screen_power_decide(const ScreenPowerInput& in);

// True only for READY, EDIT and MENU; every other state needs the user's eyes.
bool screen_state_allows_off(UIState state);

// True when an active diagnostic outside the advisory set exists: (active & ~advisory) != 0.
bool screen_error_blocks_off(uint32_t active_mask, uint32_t advisory_mask);

// Next value of the touch input block. Blocked while OFF; on OFF -> not OFF it stays blocked until the
// touch that woke the screen is released (so that touch does not reach the UI). Never blocks otherwise.
bool screen_input_block_next(bool blocked, ScreenLevel prev, ScreenLevel now, bool touch_pressed);

// ---- Timeout settings (minutes; 0 = Never, always the last step) ----
constexpr int kDimMinutesStepCount = 8;
constexpr int kOffMinutesStepCount = 7;
extern const int kDimMinutesSteps[kDimMinutesStepCount];   // {1, 2, 5, 10, 15, 30, 60, 0}
extern const int kOffMinutesSteps[kOffMinutesStepCount];   // {10, 30, 60, 120, 240, 480, 0}

// Stored value: listed -> itself, anything else (negative, unlisted, huge) -> default_minutes.
int screen_minutes_normalise_storage(int stored, const int* steps, int count, int default_minutes);
// Web value: rounded to whole; <= 0 (and NaN) -> 0 (Never); otherwise the nearest listed non-Never step
// (tie goes to the lower step); above the largest -> the largest (not Never).
int screen_minutes_normalise_api(double value, const int* steps, int count);
// Slider position <-> minutes. Index clamped to 0..count-1; unlisted minutes map to the nearest step.
int screen_minutes_from_index(int index, const int* steps, int count);
int screen_minutes_to_index(int minutes, const int* steps, int count);
// 0 -> 0, else minutes * 60000 (8 h fits uint32).
uint32_t screen_minutes_to_ms(int minutes);

// "Dim after: 5 min", "Dim after: 1 h", "Dim after: Never" / "Screen off: 30 min", ... Writes at most
// out_len bytes including the NUL (truncating) and returns out.
const char* screen_dim_label(char* out, size_t out_len, int minutes);
const char* screen_off_label(char* out, size_t out_len, int minutes);
