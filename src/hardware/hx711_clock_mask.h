#pragma once

#include <stdint.h>

// GPIO0..31 bit mask covering the HX711 clock pin(s), so all of them can be switched
// with one register write. Returns 0 when a pin is outside GPIO0..31 (the caller then
// falls back to one digitalWrite per pin). alt_pin < 0 means no second clock pin;
// alt_pin equal to sck_pin is the same pin and adds nothing.
// Pure function (no hardware access) so it can be unit tested on the host.
inline uint32_t hx711_clock_mask(int sck_pin, int alt_pin) {
    if (sck_pin < 0 || sck_pin >= 32 || alt_pin >= 32) {
        return 0;
    }
    uint32_t mask = (1UL << sck_pin);
    if (alt_pin >= 0) {
        mask |= (1UL << alt_pin);
    }
    return mask;
}
