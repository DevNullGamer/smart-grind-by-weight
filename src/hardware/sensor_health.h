#pragma once
#include <stdint.h>

// Pure helpers for load cell sample freshness and the settled check. No Arduino/ESP dependencies so
// they can be unit tested on the host. A reading counts as settled only when it is backed by enough
// fresh samples; an empty, one-sample or stale buffer is never settled.

// Milliseconds since the newest sample, safe across millis() wrap-around.
uint32_t sensor_sample_age_ms(uint32_t now_ms, uint32_t newest_sample_ms);

// A sensor with no sample at all is stale; otherwise stale when strictly older than stale_after_ms.
bool sensor_sample_is_stale(bool has_sample, uint32_t age_ms, uint32_t stale_after_ms);

// Samples a window must hold before its spread means anything: min_percent of the samples expected
// at sample_rate_sps, never below min_floor.
uint32_t settle_min_samples(uint32_t window_ms, uint32_t sample_rate_sps,
                            uint32_t min_percent, uint32_t min_floor);

// True when the window holds enough samples, the newest one is fresh and the spread is within threshold.
bool settle_check(uint32_t samples_in_window, uint32_t min_samples, bool stale,
                  float std_dev, float threshold);
