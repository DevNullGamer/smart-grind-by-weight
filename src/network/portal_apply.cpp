#include "network/portal_apply.h"

PortalApplyOutcome portal_apply_patch(const PortalPatch& patch, const PortalValues& current,
                                      PortalApplyTarget& target) {
    PortalApplyOutcome outcome;
    for (size_t i = 0; i < PF_DEVICE_NAME; i++) {
        if (!patch.has(i) || patch.v[i] == current.v[i]) continue;
        if (!target.write_field(i, patch.v[i])) {
            outcome.error = PortalError::STORE_FAILED;
            outcome.failed_field = static_cast<int>(i);
            return outcome;
        }
        outcome.written_mask |= 1u << i;
    }
    if (patch.has(PF_DEVICE_NAME)) {
        bool changed = false;
        if (!target.write_device_name(patch.device_name, &changed)) {
            outcome.error = PortalError::STORE_FAILED;
            outcome.failed_field = static_cast<int>(PF_DEVICE_NAME);
            return outcome;
        }
        if (changed) {
            outcome.written_mask |= 1u << PF_DEVICE_NAME;
            outcome.device_name_changed = true;
        }
    }
    return outcome;
}

PortalMailboxResult portal_result_from_outcome(const PortalApplyOutcome& outcome, uint32_t adjusted_mask,
                                               const PortalValues& applied) {
    PortalMailboxResult result;
    result.error = outcome.error;
    result.applied = applied;
    result.adjusted_mask = adjusted_mask;
    result.device_name_changed = outcome.device_name_changed;
    return result;
}

bool portal_encode_patch_result(const PortalMailboxResult& result, char* out, size_t cap) {
    return portal_encode_patch_reply(result.applied, result.adjusted_mask, out, cap, result.device_name_changed);
}
