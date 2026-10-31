#include "system/menu_setting_models.h"
#include <math.h>
#include <stdio.h>
#include "config/grind_control.h"
#include "config/hardware.h"
#include "controllers/grind_mode.h"

// ---- Brightness ----

int brightness_clamp_percent(int percent) {
    return percent < HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT ? HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT : percent;
}

int brightness_percent_from_fraction(float fraction) {
    if (isnan(fraction)) return HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT;  // the cast below would be undefined
    return brightness_clamp_percent((int)(fraction * 100));
}

float brightness_fraction_from_percent(int percent) {
    return brightness_clamp_percent(percent) / 100.0f;
}

float brightness_fraction_floor(float fraction) {
    return fraction < 0.15f ? 0.15f : fraction;
}

const char* brightness_label(char* out, size_t out_len, int percent, bool dimmed) {
    if (!out || out_len == 0) return out;
    snprintf(out, out_len, dimmed ? "Dimmed: %d%%" : "Brightness: %d%%", percent);
    return out;
}

// ---- Purge amount ----

int purge_slider_min() { return (int)(GRIND_PURGE_AMOUNT_MIN_G * kPurgeSliderScale + 0.5f); }
int purge_slider_max() { return (int)(GRIND_PURGE_AMOUNT_MAX_G * kPurgeSliderScale + 0.5f); }

float purge_amount_clamp(float amount_g) {
    if (isnan(amount_g) || amount_g < GRIND_PURGE_AMOUNT_MIN_G) return GRIND_PURGE_AMOUNT_MIN_G;
    if (amount_g > GRIND_PURGE_AMOUNT_MAX_G) return GRIND_PURGE_AMOUNT_MAX_G;
    return amount_g;
}

int purge_slider_from_amount(float amount_g) {
    int units = (int)(purge_amount_clamp(amount_g) * kPurgeSliderScale + 0.5f);
    if (units < purge_slider_min()) return purge_slider_min();
    if (units > purge_slider_max()) return purge_slider_max();
    return units;
}

float purge_amount_from_slider(int units) {
    return purge_amount_clamp(units / kPurgeSliderScale);
}

const char* purge_label(char* out, size_t out_len, float amount_g) {
    if (!out || out_len == 0) return out;
    snprintf(out, out_len, "Amount: %.1fg", (double)purge_amount_clamp(amount_g));
    return out;
}

// ---- Freshness ----

const float kFreshnessStepsHours[kFreshnessStepCount] = {0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 8.0f, 12.0f, 24.0f, 48.0f};

int freshness_index_from_hours(float hours) {
    for (int i = 0; i < kFreshnessStepCount; i++) {
        if (fabsf(hours - kFreshnessStepsHours[i]) < 0.1f) return i;
    }
    return kFreshnessDefaultIndex;
}

float freshness_hours_from_index(int index) {
    if (index < 0) index = 0;
    if (index > kFreshnessStepCount - 1) index = kFreshnessStepCount - 1;
    return kFreshnessStepsHours[index];
}

const char* freshness_label(char* out, size_t out_len, float hours) {
    if (!out || out_len == 0) return out;
    snprintf(out, out_len, hours < 1.0f ? "Freshness: %.1fh" : "Freshness: %.0fh", (double)hours);
    return out;
}

// ---- Grind mode radio ----

int grind_mode_radio_index(int stored_mode) {
    return stored_mode == static_cast<int>(GrindMode::TIME) ? 1 : 0;
}
