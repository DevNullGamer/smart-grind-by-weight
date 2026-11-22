#pragma once
#include <stdint.h>

// GLink dose save policy (chg024 part B, trimmed in chg025): the consumer half. Decides which finished-grind weights may be
// written to the selected GaggiMate profile as its dose. The per-link half (baseline, floor, settled test) is GLinkResultGate.
// Pure: no Arduino/ESP/NimBLE dependencies, no heap; the clock is injected (milliseconds, wrap-safe via the unsigned
// difference). The consumer (GLinkPlugin) reports events and applies a taken value under its own locks.
//
// Rules:
//  1. on_finished(): a weight outside DOSE_MIN..DOSE_MAX is skipped (reason kept, an older pending value is kept, never
//     clamped). Otherwise it becomes the pending value (latest wins).
//  2. take(): hands out the pending value once. A pending value older than PENDING_MS is dropped; while the machine cannot
//     apply it (can_apply false) it is kept.
//  3. cancel() drops the pending value.

namespace glinkc {

struct DoseSaveLimits {
    // Local copy of the GaggiMate range check (smartdose::validDose: 5..40 g); the plugin does not include upstream here.
    static constexpr int32_t DOSE_MIN_TENTHS = 50;
    static constexpr int32_t DOSE_MAX_TENTHS = 400;
    static constexpr uint32_t PENDING_MS = 300000;  // a pending dose waits at most 5 min for an idle machine
};

enum class DoseSkip : uint8_t { NONE, OUT_OF_RANGE };

class DoseSave {
public:
    DoseSave() = default;

    void on_finished(int32_t final_tenths, uint32_t now);
    bool take(bool can_apply, uint32_t now, int32_t* tenths);
    void cancel();

    bool has_pending() const { return pending_; }
    int32_t last_saved_tenths() const { return saved_tenths_; }   // 0 until something was taken
    DoseSkip last_skip() const { return skip_; }                  // reason of the latest skipped weight (kept until the next take)

private:
    bool pending_ = false;
    int32_t pending_tenths_ = 0;
    uint32_t pending_since_ = 0;
    int32_t saved_tenths_ = 0;
    DoseSkip skip_ = DoseSkip::NONE;
};

}  // namespace glinkc
