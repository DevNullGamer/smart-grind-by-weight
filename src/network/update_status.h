#pragma once

#include <stddef.h>
#include <stdint.h>

enum class UpdatePhase : uint8_t {
    IDLE,
    AWAITING_APPROVAL,  // the prompt is on the grinder, nothing received yet
    RECEIVING,
    VERIFYING,          // all bytes received, checking the image
    INSTALLED,          // done, the device restarts shortly
    FAILED              // aborted; reason() says why; stays until acknowledge()
};

enum class UpdateSource : uint8_t { NONE, WIFI_PUSH, WIFI_DOWNLOAD };

/**
 * UpdateStatus - progress of a WiFi update, shared between the writer (UpdateServer today, the pull job
 * later) and the UI. Writers may run on any task; the UI only reads. Nothing here touches LVGL or blocks.
 * revision() changes on every phase change so the UI can redraw only when needed.
 */
namespace UpdateStatus {
UpdatePhase phase();
UpdateSource source();
uint32_t received();
uint32_t total();
uint32_t revision();
// Copies the failure text (empty unless phase() == FAILED)
void reason(char* out, size_t out_len);
// True while an update is receiving, verifying or restarting (the UI must keep its progress screen)
bool active();

// Writers
void set_awaiting_approval();
void set_receiving(UpdateSource source, uint32_t total_bytes);
void set_received(uint32_t received_bytes);
void set_verifying();
void set_installed();
void set_failed(const char* reason);
// Back to IDLE from AWAITING_APPROVAL only (a declined or expired prompt); never overwrites a running update
void clear_awaiting();
// The UI has shown a FAILED result; back to IDLE (no effect in other phases)
void acknowledge();
}  // namespace UpdateStatus
