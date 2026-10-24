#include "sensor_health.h"

uint32_t sensor_sample_age_ms(uint32_t now_ms, uint32_t newest_sample_ms) {
    // Wrap-safe; a timestamp slightly ahead of now (ordering race) counts as age 0
    const int32_t diff = (int32_t)(now_ms - newest_sample_ms);
    return diff < 0 ? 0 : (uint32_t)diff;
}

bool sensor_sample_is_stale(bool has_sample, uint32_t age_ms, uint32_t stale_after_ms) {
    return !has_sample || age_ms > stale_after_ms;
}

uint32_t settle_min_samples(uint32_t window_ms, uint32_t sample_rate_sps,
                            uint32_t min_percent, uint32_t min_floor) {
    const uint64_t expected = (uint64_t)window_ms * sample_rate_sps / 1000;
    const uint32_t required = (uint32_t)(expected * min_percent / 100);
    return required > min_floor ? required : min_floor;
}

bool settle_check(uint32_t samples_in_window, uint32_t min_samples, bool stale,
                  float std_dev, float threshold) {
    if (stale || samples_in_window < min_samples) return false;
    return std_dev <= threshold;
}
