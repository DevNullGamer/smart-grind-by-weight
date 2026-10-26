#include "ui_format.h"
#include <stdio.h>

const char* format_int_with_unit(char* out, size_t out_len, int32_t value, const char* unit) {
    if (!out || out_len == 0) return out;
    if (unit && unit[0] != '\0') {
        snprintf(out, out_len, "%ld %s", static_cast<long>(value), unit);
    } else {
        snprintf(out, out_len, "%ld", static_cast<long>(value));
    }
    return out;
}

const char* format_float_with_unit(char* out, size_t out_len, float value, int decimals, const char* unit) {
    if (!out || out_len == 0) return out;
    if (decimals < 0) decimals = 0;
    if (decimals > 6) decimals = 6;
    if (unit && unit[0] != '\0') {
        snprintf(out, out_len, "%.*f%s", decimals, static_cast<double>(value), unit);
    } else {
        snprintf(out, out_len, "%.*f", decimals, static_cast<double>(value));
    }
    return out;
}
