#include "GLinkSettingsSync.h"

namespace glinkc {

constexpr int32_t SyncLimits::SIZE_MAX_TENTHS;
constexpr uint32_t SyncLimits::STORE_SPACING_MS;
constexpr uint32_t SyncLimits::WAIT_FALLBACK_MS;
constexpr int SyncLimits::MAX_ATTEMPTS;

static bool sync_same(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

static bool due(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }

// Codes the spec says to retry once the grinder is back on its READY screen (or shortly).
static bool is_wait_code(const char* code) {
    return sync_same(code, "NOT_READY") || sync_same(code, "EDIT_IN_PROGRESS") || sync_same(code, "COMMAND_IN_FLIGHT") ||
           sync_same(code, "UPDATING") || sync_same(code, "UI_BUSY") || sync_same(code, "BUSY") ||
           sync_same(code, "TIMEOUT");
}

void SettingsSync::set_wanted(int32_t dose_tenths, bool has_size, int32_t size_tenths, bool has_rpm, int32_t rpm) {
    if (!dose_ok(dose_tenths)) {
        clear_wanted();
        note_ = SyncNote::OUT_OF_RANGE_DOSE;
        return;
    }
    const bool size_valid = has_size && size_tenths >= 0 && size_tenths <= SyncLimits::SIZE_MAX_TENTHS;
    const int32_t size = size_valid ? size_tenths : 0;
    if (has_rpm && !rpm_ok(rpm)) note_ = SyncNote::RPM_OUT_OF_RANGE;
    const bool rpm_valid = has_rpm && rpm_ok(rpm);
    const int32_t rpm_value = rpm_valid ? rpm : 0;
    const bool identical =
        wanted_ && dose_ == dose_tenths && has_size_ == size_valid && size_ == size && has_rpm_ == rpm_valid && rpm_ == rpm_value;
    if (identical && (stage_ == SyncStage::UPDATE_PENDING || stage_ == SyncStage::SELECT_PENDING)) return;
    if (inflight_ != Inflight::NONE) dirty_ = true;
    if (has_rpm_ != rpm_valid || rpm_ != rpm_value) rpm_noted_ = false;
    wanted_ = true;
    dose_ = dose_tenths;
    has_size_ = size_valid;
    size_ = size;
    has_rpm_ = rpm_valid;
    rpm_ = rpm_value;
    stage_ = SyncStage::UPDATE_PENDING;
    attempts_ = 0;
    retry_set_ = false;
    wait_state_ = false;
    gave_up_code_[0] = '\0';
}

void SettingsSync::clear_wanted() {
    if (inflight_ != Inflight::NONE) dirty_ = true;
    wanted_ = false;
    stage_ = SyncStage::IDLE;
    attempts_ = 0;
    retry_set_ = false;
    wait_state_ = false;
    gave_up_code_[0] = '\0';
}

SyncNote SettingsSync::take_note() {
    const SyncNote n = note_;
    note_ = SyncNote::NONE;
    return n;
}

SyncAction SettingsSync::next(const SyncView& view, uint32_t now, ProfileUpdate* out) {
    if (!wanted_ || inflight_ != Inflight::NONE) return SyncAction::NONE;
    if (stage_ != SyncStage::UPDATE_PENDING && stage_ != SyncStage::SELECT_PENDING) return SyncAction::NONE;

    // rpm goes out only to a grinder that advertises it; the drop is noted once per wanted rpm.
    const bool rpm_send = has_rpm_ && view.rpm_supported;
    if (view.ready && has_rpm_ && !view.rpm_supported && !rpm_noted_) {
        rpm_noted_ = true;
        note_ = SyncNote::RPM_NOT_SUPPORTED;
    }
    // Mirror: a size or rpm the GaggiMate profile does not have is cleared on the grinder, but only when it holds one.
    const bool size_clear = !has_size_ && view.has_entry3 && view.entry_has_size;
    const bool rpm_clear = !has_rpm_ && view.rpm_supported && view.has_entry3 && view.entry_has_rpm;

    if (stage_ == SyncStage::UPDATE_PENDING && view.ready && view.has_entry3 && view.entry_dose_tenths == dose_ &&
        (!has_size_ || (view.entry_has_size && view.entry_size_tenths == size_)) &&
        (!rpm_send || (view.entry_has_rpm && view.entry_rpm == rpm_)) && !size_clear && !rpm_clear) {
        stage_ = SyncStage::IDLE;  // compare-first: already stored, no request and no select
        return SyncAction::NONE;
    }

    if (!view.ready || !view.client_idle) return SyncAction::NONE;
    if (!view.writes_ok) {
        if (wait_state_) seen_blocked_ = true;
        return SyncAction::NONE;
    }
    if (retry_set_ && !due(now, retry_at_)) return SyncAction::NONE;
    if (wait_state_ && !seen_blocked_ && !due(now, wait_since_ + SyncLimits::WAIT_FALLBACK_MS)) return SyncAction::NONE;

    if (stage_ == SyncStage::SELECT_PENDING) return SyncAction::SELECT;
    if (out) {
        ProfileUpdate u;
        u.has_dose = true;
        u.dose_tenths = dose_;
        if (has_size_) {
            u.has_size = true;
            u.size_tenths = size_;
        } else if (size_clear) {
            u.has_size = true;
            u.size_null = true;
        }
        if (rpm_send) {
            u.has_rpm = true;
            u.rpm = rpm_;
        } else if (rpm_clear) {
            u.has_rpm = true;
            u.rpm_null = true;
        }
        *out = u;
    }
    return SyncAction::UPDATE;
}

void SettingsSync::on_sent(uint32_t /*now*/) {
    if (stage_ == SyncStage::UPDATE_PENDING) {
        inflight_ = Inflight::UPDATE;
    } else if (stage_ == SyncStage::SELECT_PENDING) {
        inflight_ = Inflight::SELECT;
    } else {
        return;
    }
    dirty_ = false;
    ++attempts_;
    retry_set_ = false;
    wait_state_ = false;
}

void SettingsSync::on_response(bool success, const char* code, bool changed, uint32_t now) {
    if (inflight_ == Inflight::NONE) return;
    const Inflight kind = inflight_;
    inflight_ = Inflight::NONE;
    if (dirty_ || !wanted_) {  // the wanted value changed meanwhile: this answer is about an older request
        dirty_ = false;
        return;
    }
    if (success) {
        if (kind == Inflight::UPDATE && changed) {
            stage_ = SyncStage::SELECT_PENDING;
            attempts_ = 0;
            retry_set_ = true;
            retry_at_ = now + SyncLimits::STORE_SPACING_MS;
        } else {
            stage_ = SyncStage::IDLE;
        }
        return;
    }
    fail(code, now);
}

void SettingsSync::on_timeout(uint32_t now) {
    if (inflight_ == Inflight::NONE) return;
    inflight_ = Inflight::NONE;
    if (dirty_ || !wanted_) {
        dirty_ = false;
        return;
    }
    fail("TIMEOUT", now);
}

void SettingsSync::fail(const char* code, uint32_t now) {
    if (sync_same(code, "RATE_LIMITED")) {
        retry_set_ = true;
        retry_at_ = now + SyncLimits::STORE_SPACING_MS;
    } else if (is_wait_code(code)) {
        wait_for_state(now);
    } else {
        give_up(code);
        return;
    }
    if (attempts_ >= SyncLimits::MAX_ATTEMPTS) give_up(code);
}

void SettingsSync::wait_for_state(uint32_t now) {
    wait_state_ = true;
    seen_blocked_ = false;
    wait_since_ = now;
}

void SettingsSync::give_up(const char* code) {
    stage_ = SyncStage::GAVE_UP;
    size_t i = 0;
    if (code) {
        for (; code[i] && i + 1 < STR; ++i) gave_up_code_[i] = code[i];
    }
    gave_up_code_[i] = '\0';
}

void SettingsSync::on_link_down() {
    inflight_ = Inflight::NONE;
    dirty_ = false;
    attempts_ = 0;
    retry_set_ = false;
    wait_state_ = false;
    rpm_noted_ = false;  // the next session may be another grinder
    if (stage_ == SyncStage::SELECT_PENDING) stage_ = SyncStage::IDLE;
    if (stage_ == SyncStage::GAVE_UP && wanted_) {
        stage_ = SyncStage::UPDATE_PENDING;
        gave_up_code_[0] = '\0';
    }
}

}  // namespace glinkc
