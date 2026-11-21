#include "GLinkDoseSave.h"

namespace glinkc {

constexpr int32_t DoseSaveLimits::DOSE_MIN_TENTHS;
constexpr int32_t DoseSaveLimits::DOSE_MAX_TENTHS;
constexpr uint32_t DoseSaveLimits::PENDING_MS;

void DoseSave::on_finished(int32_t final_tenths, uint32_t now) {
    if (final_tenths < DoseSaveLimits::DOSE_MIN_TENTHS || final_tenths > DoseSaveLimits::DOSE_MAX_TENTHS) {
        skip_ = DoseSkip::OUT_OF_RANGE;
        return;
    }
    pending_ = true;
    pending_tenths_ = final_tenths;
    pending_since_ = now;
}

bool DoseSave::take(bool can_apply, uint32_t now, int32_t* tenths) {
    if (!pending_) return false;
    if (now - pending_since_ >= DoseSaveLimits::PENDING_MS) {  // unsigned difference is wrap-safe
        pending_ = false;  // too old
        return false;
    }
    if (!can_apply) return false;
    pending_ = false;
    saved_tenths_ = pending_tenths_;
    skip_ = DoseSkip::NONE;
    if (tenths) *tenths = pending_tenths_;
    return true;
}

void DoseSave::cancel() { pending_ = false; }

}  // namespace glinkc
