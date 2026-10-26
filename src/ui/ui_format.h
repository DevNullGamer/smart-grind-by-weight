#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure number-to-text formatting for UI labels (no Arduino/ESP/LVGL dependencies, unit tested on the host).
// All functions write at most out_len bytes including the NUL; the result is truncated if it does not fit.
// out_len == 0 or out == nullptr writes nothing. Return out.

// "134" or, with a non-empty unit, "134 kB" (a space separates the unit).
const char* format_int_with_unit(char* out, size_t out_len, int32_t value, const char* unit);

// "12.35" or, with a non-empty unit, "12.35g" (the unit is attached directly, e.g. "g"). decimals is clamped to 0..6.
const char* format_float_with_unit(char* out, size_t out_len, float value, int decimals, const char* unit);
