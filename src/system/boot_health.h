#pragma once

// Boot-health rollback. A freshly installed image (WiFi or BLE update, both end in esp_ota_set_boot_partition) is
// PENDING_VERIFY on its first boot. This module keeps it unconfirmed until setup() finished and
// BOOT_HEALTH_CONFIRM_MS passed; a reset or crash before that makes the bootloader boot the previous image.
// Images without a pending state (USB-flashed, factory slot, already confirmed) are left alone.
// The decisions are pure and live in boot_health_logic.*.

// First thing in setup(): reads and logs the image state.
void boot_health_begin();
// Last thing in setup(): from now on the confirmation timer may run.
void boot_health_setup_done();
// Called from loop(): confirms the image once it proved itself (and runs the DEBUG_BOOT_FAIL_TEST crash).
void boot_health_tick();
// True while the running image is still unconfirmed. Any update is refused meanwhile (esp_ota_begin fails with
// ESP_ERR_OTA_ROLLBACK_INVALID_STATE), and the expected-version marker is kept until the confirmation.
bool boot_health_pending();
// Fatal start-up failure (e.g. tasks cannot start): roll back to the previous image at once. Returns only if the
// image is not pending or there is nothing to roll back to.
void boot_health_rollback_now(const char* reason);
