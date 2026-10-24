#include "boot_health.h"
#include "boot_health_logic.h"
#include "ota_marker.h"
#include "../config/constants.h"
#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <atomic>

namespace {
std::atomic<bool> g_pending{false};   // running image is PENDING_VERIFY
std::atomic<bool> g_setup_done{false};
bool g_finished = false;              // loop task only: nothing left to do
uint32_t g_retry_not_before_ms = 0;   // loop task only: earliest next confirmation attempt after a failure (0 = none)
constexpr uint32_t CONFIRM_RETRY_MS = 5000;

const char* state_name(esp_ota_img_states_t state) {
    switch (state) {
        case ESP_OTA_IMG_NEW: return "NEW";
        case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
        case ESP_OTA_IMG_VALID: return "VALID";
        case ESP_OTA_IMG_INVALID: return "INVALID";
        case ESP_OTA_IMG_ABORTED: return "ABORTED";
        case ESP_OTA_IMG_UNDEFINED: return "UNDEFINED";
    }
    return "?";
}
}  // namespace

// Overrides the weak function of the Arduino core (C linkage, defined in esp32-hal-misc.c): initArduino() then does
// NOT confirm a PENDING_VERIFY image on its own; boot_health_tick() does after the image proved itself.
extern "C" bool verifyRollbackLater() {
    return true;
}

void boot_health_begin() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t err = running ? esp_ota_get_state_partition(running, &state) : ESP_FAIL;
    if (err == ESP_OK) {
        g_pending = (state == ESP_OTA_IMG_PENDING_VERIFY);
        LOG_BLE("[BOOT] image state: %s (partition %s)\n", state_name(state), running->label);
    } else {
        // Factory slot (not an OTA slot) or no usable otadata: the bootloader never marked this image pending
        g_pending = false;
        LOG_BLE("[BOOT] image state: not available (error 0x%x, partition %s)\n", static_cast<unsigned>(err),
                running ? running->label : "?");
    }
}

void boot_health_setup_done() {
    g_setup_done = true;
}

bool boot_health_pending() {
    return g_pending;
}

void boot_health_tick() {
    if (g_finished || !g_setup_done) return;
    const uint32_t uptime_ms = millis();
    const bool pending = g_pending;

#if DEBUG_BOOT_FAIL_TEST
    if (boot_health_crash_due(DEBUG_BOOT_FAIL_TEST, pending, true, uptime_ms, DEBUG_BOOT_FAIL_DELAY_MS)) {
        LOG_BLE("[BOOT] boot-fail test: %s now, image is unconfirmed\n",
                DEBUG_BOOT_FAIL_TEST == 1 ? "abort()" : "esp_restart()");
        Serial.flush();
        if (DEBUG_BOOT_FAIL_TEST == 1) abort();
        esp_restart();
    }
#endif

    if (!pending) {
        g_finished = true;  // nothing to confirm: stop looking
        return;
    }
    if (!boot_health_should_confirm(pending, true, uptime_ms, BOOT_HEALTH_CONFIRM_MS)) return;
    if (g_retry_not_before_ms != 0 && static_cast<int32_t>(uptime_ms - g_retry_not_before_ms) < 0) return;

    // Re-read the state right before acting: only a still pending image needs the call
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            LOG_BLE("[BOOT] image confirmed after %lu ms\n", static_cast<unsigned long>(uptime_ms));
        } else {
            // Stay pending (and keep the marker) so a reset still rolls back and the failure is reported; retry later
            LOG_BLE("[BOOT] ERROR: image confirmation failed (0x%x), retrying in %lu s\n", static_cast<unsigned>(err),
                    static_cast<unsigned long>(CONFIRM_RETRY_MS / 1000));
            g_retry_not_before_ms = uptime_ms + CONFIRM_RETRY_MS;
            return;
        }
    }
    // The expected-version marker of the update stays until here, so that a rollback is reported by the previous
    // image on its next boot. Cleared before updates are allowed again.
    ota_marker_clear();
    g_pending = false;
    g_finished = true;
}

void boot_health_rollback_now(const char* reason) {
    if (!g_pending) return;
    LOG_BLE("[BOOT] FATAL: %s, rolling back to the previous image\n", reason);
    Serial.flush();
    const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();  // returns only on failure
    LOG_BLE("[BOOT] rollback not possible (0x%x)\n", static_cast<unsigned>(err));
}
