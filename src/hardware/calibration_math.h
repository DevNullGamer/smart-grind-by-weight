#pragma once
#include <stdint.h>

// Pure helpers for the load cell calibration result. No Arduino/ESP dependencies so they can be unit
// tested on the host. A calibration factor is only accepted when the weight really moved the raw reading.

enum class CalibrationResult {
    OK,
    INVALID_WEIGHT,   // known weight not > 0 (or not finite)
    HARDWARE_FAULT,   // HX711 fault active, no usable samples
    SETTLE_TIMEOUT,   // reading never settled, so no trustworthy raw value
    SIGNAL_TOO_SMALL, // |raw - tare_offset| below the minimum: no weight on the scale
    INVALID_FACTOR,   // computed factor is not finite or zero
    TARE_FAILED       // the empty-scale tare failed or timed out, so the zero point is not trustworthy
};

// Result of the calibration's empty-scale tare step: the flow may only go on to the known-weight step on OK.
CalibrationResult calibration_tare_result(bool tare_ok);

// Computes the factor (raw counts per gram) from a settled raw reading. Either sign is allowed (a load
// cell wired the other way round gives a negative factor). On anything but OK, *factor_out is untouched
// so the caller keeps the old factor.
CalibrationResult calibration_compute_factor(int32_t raw_reading, int32_t tare_offset, float known_weight_g,
                                             int32_t min_raw_delta, float* factor_out);

// Notice shown to the user when a calibration fails (never called for OK).
const char* calibration_result_title(CalibrationResult result);
const char* calibration_result_message(CalibrationResult result);
