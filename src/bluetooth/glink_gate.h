#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include "glink_mailbox.h"
#include "glink_snapshot.h"

/**
 * GlinkCommandGate - thread-safe hand-over between the Bluetooth task and the UI task for GLink
 * (docs/GLINK-PROTOCOL.md 4.1 step 8, 7.4). Modelled on PortalCommandGate: the pure state machine lives in
 * glink_mailbox.*, this wrapper adds the lock and the clock. Every critical section is a few struct copies
 * (spinlock, no blocking call inside).
 *
 *  - Reads: the UI task publishes a snapshot; the Bluetooth task copies it (periodInfo, grindProfile get).
 *    The Bluetooth task never calls LVGL, the controllers, the profiles or NVS.
 *  - Writes: the Bluetooth task files one request (submit) and polls for the answer on its next ticks; the UI task takes
 *    it, applies it and completes it. One request is in flight at a time.
 *
 * Until the UI controller calls attach_ui(true) nothing can be answered: reads and submits fail (UI_BUSY).
 */
class GlinkCommandGate {
public:
    // ---- Bluetooth task ----
    // False when no fresh snapshot exists (UI task not running or stuck)
    bool read_snapshot(GlinkSnapshot* out);
    // False when the UI task is not attached or the mailbox is still busy (an abandoned request)
    bool submit(const GlinkRequest& request);
    GlinkMailboxWait poll(GlinkMailboxResult* out);

    // ---- UI task ----
    bool take(GlinkRequest* out);
    // A request is filed and not yet taken (QUEUED); the UI task uses it to keep a pending stop for the lead-in it may start
    bool request_queued();
    void complete(const GlinkMailboxResult& result);
    void publish(const GlinkSnapshot& snapshot);
    void attach_ui(bool attached);

private:
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    GlinkMailbox mailbox_;
    GlinkSnapshot snapshot_;
    bool published_ = false;
    uint32_t published_ms_ = 0;
    volatile bool ui_attached_ = false;
};

extern GlinkCommandGate glink_command_gate;
