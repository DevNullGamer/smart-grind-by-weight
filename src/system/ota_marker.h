#pragma once

#include <stdint.h>

// Expected-version marker for the post-reboot check (NVS namespace "grinder", keys new_build_nr / new_fw_ver),
// the same keys the BLE OTA writes. check_ota_failure_after_boot() compares them with the running firmware and
// shows the update-failed screen on a mismatch (e.g. after a rollback), then removes them.
// Uses its own short-lived Preferences handle; call it only while no update owns the flash/tasks
// (store before the UpdateGuard is taken, clear after it was released).

// Stores build (if >= 0) and version (if not null/empty). Returns false if nothing was stored.
bool ota_marker_store(int32_t build, const char* version);
// Removes both keys (no-op when absent).
void ota_marker_clear();
