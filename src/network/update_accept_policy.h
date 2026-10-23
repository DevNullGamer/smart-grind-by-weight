#pragma once

#include "update_http_logic.h"

/**
 * Decides whether an announced update may start (called from POST /api/update/begin after the token and the
 * request were validated and the grinder was found idle, before the grinder is locked). Runs on the HTTP task
 * and may block while the user decides.
 */
class UpdateAcceptPolicy {
public:
    virtual ~UpdateAcceptPolicy() = default;
    // peer_ip: dotted IPv4 of the client, empty if unknown
    virtual AcceptDecision decide(const UpdateBeginRequest& request, const char* peer_ip) = 0;
};

// The production policy: Bluetooth exclusion, the "Confirm" setting, and the on-device prompt through
// update_approval. Fails closed: without an attached UI the prompt cannot be answered, so it answers UI_BUSY.
UpdateAcceptPolicy* production_accept_policy();
