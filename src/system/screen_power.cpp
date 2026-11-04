#include "system/screen_power.h"
#include <math.h>
#include <stdio.h>

const int kDimMinutesSteps[kDimMinutesStepCount] = {1, 2, 5, 10, 15, 30, 60, 0};
const int kOffMinutesSteps[kOffMinutesStepCount] = {10, 30, 60, 120, 240, 480, 0};

ScreenLevel screen_power_decide(const ScreenPowerInput& in) {
    if (in.keep_awake) return ScreenLevel::AWAKE;

    // While OFF only a strong weight change counts, so a small drift does not wake the screen.
    uint32_t weight_idle = (in.current == ScreenLevel::OFF) ? in.ms_since_strong_weight_activity
                                                            : in.ms_since_weight_activity;
    uint32_t idle = in.ms_since_touch < weight_idle ? in.ms_since_touch : weight_idle;

    if (in.off_after_ms != 0 && idle >= in.off_after_ms && !in.keep_on) return ScreenLevel::OFF;
    if (in.dim_after_ms != 0 && idle >= in.dim_after_ms) return ScreenLevel::DIMMED;
    return ScreenLevel::AWAKE;
}

bool screen_state_allows_off(UIState state) {
    switch (state) {
        case UIState::READY:
        case UIState::EDIT:
        case UIState::MENU:
            return true;
        default:
            return false;
    }
}

bool screen_error_blocks_off(uint32_t active_mask, uint32_t advisory_mask) {
    return (active_mask & ~advisory_mask) != 0;
}

bool screen_input_block_next(bool blocked, ScreenLevel prev, ScreenLevel now, bool touch_pressed) {
    if (now == ScreenLevel::OFF) return true;
    // Leaving OFF (or still swallowing the waking touch): blocked until the finger is lifted.
    if (prev == ScreenLevel::OFF || blocked) return touch_pressed;
    return false;
}

int screen_minutes_normalise_storage(int stored, const int* steps, int count, int default_minutes) {
    for (int i = 0; i < count; i++) {
        if (steps[i] == stored) return stored;
    }
    return default_minutes;
}

int screen_minutes_normalise_api(double value, const int* steps, int count) {
    if (isnan(value) || value <= 0.0) return 0;

    // Largest non-Never step; the list is ascending with Never (0) last.
    int largest = 0;
    for (int i = 0; i < count; i++) {
        if (steps[i] > largest) largest = steps[i];
    }
    if (value >= (double)largest) return largest;

    int whole = (int)floor(value + 0.5);  // below largest, so fits int
    if (whole <= 0) return 0;

    int best = 0;
    int best_dist = 0;
    for (int i = 0; i < count; i++) {
        if (steps[i] <= 0) continue;
        int dist = steps[i] > whole ? steps[i] - whole : whole - steps[i];
        // Strict '<' keeps the lower step on a tie (ascending order).
        if (best == 0 || dist < best_dist) {
            best = steps[i];
            best_dist = dist;
        }
    }
    return best;
}

int screen_minutes_from_index(int index, const int* steps, int count) {
    if (index < 0) index = 0;
    if (index > count - 1) index = count - 1;
    return steps[index];
}

int screen_minutes_to_index(int minutes, const int* steps, int count) {
    int snapped = screen_minutes_normalise_api((double)minutes, steps, count);
    for (int i = 0; i < count; i++) {
        if (steps[i] == snapped) return i;
    }
    return count - 1;
}

uint32_t screen_minutes_to_ms(int minutes) {
    if (minutes <= 0) return 0;
    return (uint32_t)minutes * 60000u;
}

static const char* screen_label(char* out, size_t out_len, const char* prefix, int minutes) {
    if (out_len == 0) return out;
    if (minutes <= 0) {
        snprintf(out, out_len, "%s: Never", prefix);
    } else if (minutes % 60 == 0) {
        snprintf(out, out_len, "%s: %d h", prefix, minutes / 60);
    } else {
        snprintf(out, out_len, "%s: %d min", prefix, minutes);
    }
    return out;
}

const char* screen_dim_label(char* out, size_t out_len, int minutes) {
    return screen_label(out, out_len, "Dim after", minutes);
}

const char* screen_off_label(char* out, size_t out_len, int minutes) {
    return screen_label(out, out_len, "Screen off", minutes);
}
