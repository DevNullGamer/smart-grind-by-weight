#include "GLinkDoseSave.h"

namespace glinkc {

constexpr int32_t DoseSaveLimits::DOSE_MIN_TENTHS;
constexpr int32_t DoseSaveLimits::DOSE_MAX_TENTHS;
constexpr uint32_t DoseSaveLimits::PENDING_MS;

void DoseSave::on_link_down() {
    baseline_ = false;
    floor_ = 0;
    pending_ = false;
}

void DoseSave::on_ready(uint32_t result_seq) {
    baseline_ = true;
    floor_ = result_seq;
    pending_ = false;
}

void DoseSave::on_result(uint32_t seq, bool weight_settled, int32_t final_tenths, bool enabled, uint32_t now) {
    if (!baseline_ || !after(seq, floor_)) return;
    floor_ = seq;
    if (!enabled) {
        skip_ = DoseSkip::FEATURE_OFF;
        return;
    }
    if (!weight_settled) {
        skip_ = DoseSkip::NOT_SETTLED;
        return;
    }
    if (final_tenths < DoseSaveLimits::DOSE_MIN_TENTHS || final_tenths > DoseSaveLimits::DOSE_MAX_TENTHS) {
        skip_ = DoseSkip::OUT_OF_RANGE;
        return;
    }
    pending_ = true;
    pending_tenths_ = final_tenths;
    pending_seq_ = seq;
    pending_since_ = now;
}

bool DoseSave::take(bool can_apply, uint32_t now, int32_t* tenths, uint32_t* seq) {
    if (!pending_) return false;
    if (now - pending_since_ >= DoseSaveLimits::PENDING_MS) {  // unsigned difference is wrap-safe
        pending_ = false;  // too old
        return false;
    }
    if (!can_apply) return false;
    pending_ = false;
    saved_tenths_ = pending_tenths_;
    saved_seq_ = pending_seq_;
    skip_ = DoseSkip::NONE;
    if (tenths) *tenths = pending_tenths_;
    if (seq) *seq = pending_seq_;
    return true;
}

void DoseSave::cancel() { pending_ = false; }

}  // namespace glinkc
