#include "update_accept_policy.h"
#include "update_approval_gate.h"
#include "update_status.h"
#include "wifi_manager.h"
#include "../config/constants.h"
#include "../bluetooth/manager.h"
#include <string.h>

#if DEBUG_UPDATE_PROMPT_AUTOREPLY && !DEBUG_WIFI_DEV_CREDS
#error "DEBUG_UPDATE_PROMPT_AUTOREPLY is for the wifi-dev env only"
#endif

namespace {

bool ble_busy() {
    return g_bluetooth_manager.is_connected() || g_bluetooth_manager.is_data_export_active();
}

class ProductionPolicy : public UpdateAcceptPolicy {
public:
    AcceptDecision decide(const UpdateBeginRequest& request, const char* peer_ip) override {
        // A Bluetooth client and a WiFi update must not overlap; this check also applies with Confirm off
        if (ble_busy()) return AcceptDecision::BLE_CONNECTED;
        if (!wifi_manager.accept_required()) {
            LOG_BLE("UPDATE: Confirm is off, update accepted without a prompt\n");
            return AcceptDecision::ACCEPT;
        }

#if !DEBUG_UPDATE_PROMPT_AUTOREPLY
        // Nobody can answer the prompt yet (no UI controller attached): refuse instead of waiting or accepting
        if (!update_approval.ui_attached()) return AcceptDecision::UI_BUSY;
#endif

        ApprovalPrompt prompt;
        prompt.size = request.size;
        prompt.build = request.build;
        strncpy(prompt.version, request.version, sizeof(prompt.version) - 1);
        strncpy(prompt.peer_ip, peer_ip ? peer_ip : "", sizeof(prompt.peer_ip) - 1);

        switch (update_approval.request(prompt, UPDATE_PROMPT_TIMEOUT_MS, UPDATE_DECLINE_BACKOFF_MS)) {
            case ApprovalRequestResult::BACKOFF: return AcceptDecision::BACKOFF;
            case ApprovalRequestResult::ALREADY_PENDING: return AcceptDecision::UI_BUSY;
            case ApprovalRequestResult::OK: break;
        }
        UpdateStatus::set_awaiting_approval();
        LOG_BLE("UPDATE: waiting for the user to answer the prompt (%u s)\n", (unsigned)(UPDATE_PROMPT_TIMEOUT_MS / 1000));

#if DEBUG_UPDATE_PROMPT_AUTOREPLY == 1
        LOG_BLE("UPDATE: AUTOREPLY accept\n");
        update_approval.resolve(true);
#elif DEBUG_UPDATE_PROMPT_AUTOREPLY == 2
        LOG_BLE("UPDATE: AUTOREPLY decline\n");
        update_approval.resolve(false);
#elif DEBUG_UPDATE_PROMPT_AUTOREPLY == 3
        LOG_BLE("UPDATE: AUTOREPLY no reply\n");
#endif

        const ApprovalState state = update_approval.wait();
        const uint32_t backoff_ms = update_approval.backoff_remaining_ms();
        update_approval.clear();
        LOG_BLE("UPDATE: prompt answered, state %u (back-off %lu ms)\n", (unsigned)state, (unsigned long)backoff_ms);

        switch (state) {
            case ApprovalState::ACCEPTED:
                // A Bluetooth client may have connected while the user decided
                if (ble_busy()) {
                    UpdateStatus::clear_awaiting();
                    return AcceptDecision::BLE_CONNECTED;
                }
                return AcceptDecision::ACCEPT;  // the caller sets the RECEIVING status once the sink runs
            case ApprovalState::DECLINED: UpdateStatus::clear_awaiting(); return AcceptDecision::DECLINED;
            case ApprovalState::EXPIRED: UpdateStatus::clear_awaiting(); return AcceptDecision::NO_RESPONSE;
            case ApprovalState::CANCELLED: UpdateStatus::clear_awaiting(); return AcceptDecision::CANCELLED;
            default: UpdateStatus::clear_awaiting(); return AcceptDecision::UI_BUSY;  // UI_BUSY or anything unexpected
        }
    }
};

ProductionPolicy g_policy;

}  // namespace

UpdateAcceptPolicy* production_accept_policy() {
    return &g_policy;
}
