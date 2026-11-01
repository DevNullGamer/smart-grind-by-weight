#pragma once
#include <stddef.h>
#include "portal_api.h"
#include "portal_mailbox.h"

// Pure "apply a parsed PATCH" logic of the web panel (no Arduino/ESP dependencies, unit tested on the host):
// the order the fields are written in, which ones are skipped, and what a failed write leaves behind.
// The firmware target (ui/controllers/portal_ui_controller.cpp) writes through UiSettings, ProfileController
// and WiFiManager, the same handlers as the touchscreen.

class PortalApplyTarget {
public:
    virtual ~PortalApplyTarget() = default;
    // Stores one setting, grind mode or profile target (field index PF_BLE_STARTUP .. PF_CUSTOM_TIME_S, value
    // normalised like PortalValues). false = the value could not be persisted.
    virtual bool write_field(size_t index, float value) = 0;
    // Stores the device name and applies it to mDNS. *changed = false when it already was the name.
    // false = it could not be persisted (then *changed stays false).
    virtual bool write_device_name(const char* name, bool* changed) = 0;
};

struct PortalApplyOutcome {
    PortalError error = PortalError::NONE;  // NONE or STORE_FAILED
    bool device_name_changed = false;
    uint32_t written_mask = 0;              // fields actually written (bit per field index), also after a failure
    int failed_field = -1;                  // the field whose write failed, -1 when none
};

// Applies the fields named in the patch, in this fixed order: ascending field index (the settings, then the
// grind mode, then the six profile targets) and the device name LAST. A field whose value equals current is
// skipped (no write, no flash wear). The first failed write stops everything: fields before it stay applied
// (written_mask), the failed one and all after it are not touched, the outcome is STORE_FAILED. The device
// name is the disruptive change (the page must be reopened at the new address), so it only happens when every
// other field of the patch was stored.
PortalApplyOutcome portal_apply_patch(const PortalPatch& patch, const PortalValues& current,
                                      PortalApplyTarget& target);

// The mailbox answer for an applied patch: the error and the device-name flag of the outcome, the adjusted mask of
// the patch and the stored state read back after the write. Both hops of the `address` reply flag (outcome ->
// result here, result -> JSON below) are pure so they are host-tested.
PortalMailboxResult portal_result_from_outcome(const PortalApplyOutcome& outcome, uint32_t adjusted_mask,
                                               const PortalValues& applied);
// JSON of a successful PATCH reply from the mailbox answer (adds "address" when the device name changed)
bool portal_encode_patch_result(const PortalMailboxResult& result, char* out, size_t cap);
