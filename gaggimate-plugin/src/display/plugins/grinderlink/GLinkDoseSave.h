#pragma once
#include <stdint.h>

// GLink dose save policy (chg024 part B): decides WHICH grind results may be written to the selected GaggiMate profile as
// its dose. Pure: no Arduino/ESP/NimBLE dependencies, no heap, no pointers; the clock is injected (milliseconds, wrap-safe
// via static_cast<int32_t>(now - t)). The glue (GLinkPlugin) reports events and applies a taken value under its own locks.
//
// Rules:
//  1. State is per link. on_link_down() clears baseline, floor and pending (also used for select and forget). on_ready()
//     takes the grinder's current result_seq as the floor, on every link, so a rebooted grinder is handled too.
//  2. on_result(): ignored without a baseline or when seq <= floor (signed difference). Otherwise the floor becomes seq
//     ALWAYS, even when the feature is off, so switching it on later never writes an old result. Then the result is skipped
//     (reason kept) when the feature is off, when the weight did not settle (spec 5.6: true exactly for OK, OVERSHOOT,
//     MAX_PULSES; a later, higher seq for the same grind still writes), or when the dose is outside the valid range.
//     Out-of-range values are skipped, never clamped. Otherwise it becomes the pending value (latest wins).
//  3. take(): hands out the pending value once. A pending value older than PENDING_MS is dropped; while the machine cannot
//     apply it (can_apply false) it is kept.
//  4. cancel() drops the pending value (feature switched off).

namespace glinkc {

struct DoseSaveLimits {
    // Local copy of the GaggiMate range check (smartdose::validDose: 5..40 g); the plugin does not include upstream here.
    static constexpr int32_t DOSE_MIN_TENTHS = 50;
    static constexpr int32_t DOSE_MAX_TENTHS = 400;
    static constexpr uint32_t PENDING_MS = 300000;  // a pending dose waits at most 5 min for an idle machine
};

// FEATURE_OFF, not DISABLED: Arduino.h defines DISABLED as a macro.
enum class DoseSkip : uint8_t { NONE, FEATURE_OFF, NOT_SETTLED, OUT_OF_RANGE };

class DoseSave {
public:
    DoseSave() = default;

    void on_link_down();
    void on_ready(uint32_t result_seq);
    void on_result(uint32_t seq, bool weight_settled, int32_t final_tenths, bool enabled, uint32_t now);
    bool take(bool can_apply, uint32_t now, int32_t* tenths, uint32_t* seq);
    void cancel();

    bool has_pending() const { return pending_; }
    int32_t last_saved_tenths() const { return saved_tenths_; }   // 0 until something was taken
    uint32_t last_saved_seq() const { return saved_seq_; }
    DoseSkip last_skip() const { return skip_; }                  // reason of the latest skipped result (kept until the next write)

private:
    static bool after(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }

    bool baseline_ = false;
    uint32_t floor_ = 0;
    bool pending_ = false;
    int32_t pending_tenths_ = 0;
    uint32_t pending_seq_ = 0;
    uint32_t pending_since_ = 0;
    int32_t saved_tenths_ = 0;
    uint32_t saved_seq_ = 0;
    DoseSkip skip_ = DoseSkip::NONE;
};

}  // namespace glinkc
