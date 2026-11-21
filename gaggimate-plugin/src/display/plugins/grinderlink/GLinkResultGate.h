#pragma once
#include <stdint.h>

// GLink result gate (chg025): the per-link half of the old dose save. Decides which `grindResult` may be reported as a
// finished grind. Pure: no Arduino/ESP/NimBLE dependencies, no heap; sequence numbers compare by signed difference
// (wrap-safe).
//
// Rules:
//  1. on_ready(result_seq) takes the grinder's current resultSeq as the floor, on every link (a rebooted grinder with a
//     lower seq is re-baselined too). Without a baseline nothing is reported.
//  2. on_result(): ignored when seq is not newer than the floor. Otherwise the floor becomes seq ALWAYS (settled or not),
//     so a STOPPED followed by the real outcome (higher seq) still reports the later one. Reported (true, once) only when
//     the weight settled (spec 5.6: OK, OVERSHOOT, MAX_PULSES).
//  3. reset() (link down) clears baseline and floor.

namespace glinkc {

class ResultGate {
public:
    ResultGate() = default;

    void reset();
    void on_ready(uint32_t result_seq);
    // true exactly when the result is newer than the floor and settled; *out_tenths receives the weight.
    bool on_result(uint32_t seq, bool weight_settled, int32_t final_tenths, int32_t* out_tenths);

    bool has_baseline() const { return baseline_; }
    uint32_t floor() const { return floor_; }

private:
    bool baseline_ = false;
    uint32_t floor_ = 0;
};

}  // namespace glinkc
