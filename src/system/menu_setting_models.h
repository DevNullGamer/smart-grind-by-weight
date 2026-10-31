#pragma once
#include <stddef.h>

// Pure conversions and label texts for the menu settings (no Arduino/ESP/LVGL dependencies, unit tested on
// the host). Copied bit-for-bit from the code they replace; clamping lives here, not in UiSettings.
// Label functions write at most out_len bytes including the NUL (truncating) and return out.

// ---- Brightness (UI works in percent, storage in fractions 0..1) ----
int brightness_clamp_percent(int percent);                 // at least HW_DISPLAY_MINIMAL_BRIGHTNESS_PERCENT
int brightness_percent_from_fraction(float fraction);      // (int)(fraction * 100), then clamped; NaN -> minimum
float brightness_fraction_from_percent(int percent);       // clamped percent / 100.0f
float brightness_fraction_floor(float fraction);           // raises anything below 0.15f to 0.15f
// "Brightness: 57%" or, with dimmed, "Dimmed: 57%"
const char* brightness_label(char* out, size_t out_len, int percent, bool dimmed);

// ---- Purge amount (slider units of 0.1 g) ----
constexpr float kPurgeSliderScale = 10.0f;
int purge_slider_min();
int purge_slider_max();
float purge_amount_clamp(float amount_g);                  // into [MIN, MAX]; NaN -> MIN
int purge_slider_from_amount(float amount_g);              // clamped, rounded, within slider range
float purge_amount_from_slider(int units);                 // units / 10.0f clamped to [MIN, MAX]
const char* purge_label(char* out, size_t out_len, float amount_g);  // "Amount: 1.7g" (of the clamped amount)

// ---- Freshness (discrete slider steps) ----
constexpr int kFreshnessStepCount = 9;
constexpr int kFreshnessDefaultIndex = 5;                  // 8 h
extern const float kFreshnessStepsHours[kFreshnessStepCount];
int freshness_index_from_hours(float hours);               // first step within 0.1 h, else kFreshnessDefaultIndex
float freshness_hours_from_index(int index);               // index clamped to 0..8
const char* freshness_label(char* out, size_t out_len, float hours);  // "Freshness: 0.5h" / "Freshness: 8h"

// ---- Grind mode radio ----
int grind_mode_radio_index(int stored_mode);               // GrindMode::TIME -> 1, anything else -> 0
