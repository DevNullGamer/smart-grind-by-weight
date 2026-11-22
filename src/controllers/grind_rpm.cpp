#include "grind_rpm.h"
#include <stdio.h>
#include "../system/ui_settings.h"

static_assert(GRIND_RPM_MAX == GLINK_RPM_MAX, "rpm range differs from config/bluetooth.h");

namespace {

const GrindRpm kNoRpm;
constexpr int32_t kMissing = -1;  // "no rpm" in NVS (a stored rpm is never negative)

}  // namespace

bool grind_rpm_valid(const GrindRpm& rpm) {
    if (!rpm.present) return rpm.value == 0;
    return rpm.value >= 0 && rpm.value <= GRIND_RPM_MAX;
}

bool grind_rpm_equal(const GrindRpm& a, const GrindRpm& b) {
    if (!a.present || !b.present) return a.present == b.present;
    return a.value == b.value;
}

size_t grind_info_format(char* out, size_t cap, const GrindSize& size, const GrindRpm& rpm) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    size_t len = grind_size_format(out, cap, size);
    if (!rpm.present || !grind_rpm_valid(rpm)) return len;
    if (len + 1 >= cap) return len;  // no room for even the separator
    int n = snprintf(out + len, cap - len, "%s%d rpm", len > 0 ? "\n" : "", static_cast<int>(rpm.value));
    if (n < 0) {
        out[len] = '\0';
        return len;
    }
    const size_t room = cap - len;
    return len + (static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room - 1);
}

void GrindRpmStore::load() {
    for (int slot = 0; slot < GRIND_RPM_SLOT_COUNT; slot++) {
        slots_[slot] = GrindRpm();
        if (!store_) continue;
        const int32_t value = store_->get_int(ui_settings_keys::kGrinderNs, ui_settings_keys::kGrindRpm[slot], kMissing);
        if (value >= 0 && value <= GRIND_RPM_MAX) {
            slots_[slot].present = true;
            slots_[slot].value = value;
        }
    }
}

const GrindRpm& GrindRpmStore::get(int slot) const {
    return (slot >= 0 && slot < GRIND_RPM_SLOT_COUNT) ? slots_[slot] : kNoRpm;
}

GrindRpmStoreResult GrindRpmStore::store(int slot, const GrindRpm& rpm) {
    if (!store_ || slot < 0 || slot >= GRIND_RPM_SLOT_COUNT || !grind_rpm_valid(rpm)) return GrindRpmStoreResult::FAILED;
    if (grind_rpm_equal(slots_[slot], rpm)) return GrindRpmStoreResult::UNCHANGED;
    if (!rpm.present) return clear(slot) ? GrindRpmStoreResult::STORED : GrindRpmStoreResult::FAILED;
    if (!store_->put_int(ui_settings_keys::kGrinderNs, ui_settings_keys::kGrindRpm[slot], rpm.value)) return GrindRpmStoreResult::FAILED;
    slots_[slot] = rpm;
    return GrindRpmStoreResult::STORED;
}

bool GrindRpmStore::clear(int slot) {
    if (!store_ || slot < 0 || slot >= GRIND_RPM_SLOT_COUNT) return false;
    if (!store_->erase_blob(ui_settings_keys::kGrinderNs, ui_settings_keys::kGrindRpm[slot])) return false;
    slots_[slot] = GrindRpm();
    return true;
}
