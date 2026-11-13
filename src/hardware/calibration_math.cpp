#include "calibration_math.h"
#include <math.h>

CalibrationResult calibration_compute_factor(int32_t raw_reading, int32_t tare_offset, float known_weight_g,
                                             int32_t min_raw_delta, float* factor_out) {
    if (!(known_weight_g > 0.0f) || !isfinite(known_weight_g)) {
        return CalibrationResult::INVALID_WEIGHT;
    }

    // 64-bit so an extreme raw/tare pair cannot overflow the subtraction
    const int64_t delta = static_cast<int64_t>(raw_reading) - static_cast<int64_t>(tare_offset);
    const int64_t abs_delta = delta < 0 ? -delta : delta;
    if (abs_delta < min_raw_delta) {
        return CalibrationResult::SIGNAL_TOO_SMALL;
    }

    const float factor = static_cast<float>(delta) / known_weight_g;
    if (!isfinite(factor) || factor == 0.0f) {
        return CalibrationResult::INVALID_FACTOR;
    }

    if (factor_out) {
        *factor_out = factor;
    }
    return CalibrationResult::OK;
}

CalibrationResult calibration_tare_result(bool tare_ok) {
    return tare_ok ? CalibrationResult::OK : CalibrationResult::TARE_FAILED;
}

const char* calibration_result_title(CalibrationResult result) {
    switch (result) {
        case CalibrationResult::TARE_FAILED: return "TARE FAILED";
        case CalibrationResult::INVALID_WEIGHT: return "CALIBRATION FAILED";
        case CalibrationResult::HARDWARE_FAULT: return "LOAD CELL FAULT";
        case CalibrationResult::SETTLE_TIMEOUT: return "CALIBRATION FAILED";
        case CalibrationResult::SIGNAL_TOO_SMALL: return "CALIBRATION FAILED";
        case CalibrationResult::INVALID_FACTOR: return "CALIBRATION FAILED";
        case CalibrationResult::OK: break;
    }
    return "";
}

const char* calibration_result_message(CalibrationResult result) {
    switch (result) {
        case CalibrationResult::TARE_FAILED:
            return "The scale could not be zeroed.\n\nKeep the scale empty and still, then try again.";
        case CalibrationResult::INVALID_WEIGHT:
            return "The calibration weight is not valid.\n\nThe previous calibration is kept.";
        case CalibrationResult::HARDWARE_FAULT:
            return "The load cell is not responding.\n\nThe previous calibration is kept.";
        case CalibrationResult::SETTLE_TIMEOUT:
            return "The reading did not settle.\n\nKeep the scale still and try again. The previous calibration is kept.";
        case CalibrationResult::SIGNAL_TOO_SMALL:
            return "No weight detected on the scale.\n\nPlace the calibration weight and try again. The previous calibration is kept.";
        case CalibrationResult::INVALID_FACTOR:
            return "The measured value is not usable.\n\nTry again. The previous calibration is kept.";
        case CalibrationResult::OK: break;
    }
    return "";
}
