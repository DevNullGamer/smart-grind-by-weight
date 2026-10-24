#pragma once

//==============================================================================
// FIRMWARE UPDATE (WiFi / raw image) CONFIGURATION
//==============================================================================
// Plain #defines only: this file is also included by the host-testable
// system/image_header module (no Arduino dependencies).

//------------------------------------------------------------------------------
// UPDATE GUARD (same safety rules as the BLE OTA)
//------------------------------------------------------------------------------
#define UPDATE_INACTIVITY_TIMEOUT_MS 15000                                     // Abort a started update when no data arrives for this long
#define UPDATE_WDT_TIMEOUT_MS 1800000                                          // Task watchdog timeout while an update runs (restored afterwards)

//------------------------------------------------------------------------------
// APP IMAGE VALIDATION (ESP32-S3)
//------------------------------------------------------------------------------
#define UPDATE_IMAGE_HEADER_BYTES 40                                           // 24 B image header + 8 B first segment header + app descriptor magic
#define UPDATE_IMAGE_MIN_SIZE (128 * 1024)                                     // Smaller than this cannot be a complete application image
#define UPDATE_IMAGE_MAX_SEGMENTS 16                                           // ESP_IMAGE_MAX_SEGMENTS
#define UPDATE_IMAGE_CHIP_ID_ESP32S3 0x0009
#define UPDATE_IMAGE_APP_DESC_MAGIC 0xABCD5432UL                               // esp_app_desc_t.magic_word
// Plausible entry addresses: internal IRAM or flash-mapped instruction bus
#define UPDATE_IMAGE_ENTRY_IRAM_MIN 0x40370000UL
#define UPDATE_IMAGE_ENTRY_IRAM_MAX 0x40400000UL                               // exclusive
#define UPDATE_IMAGE_ENTRY_FLASH_MIN 0x42000000UL
#define UPDATE_IMAGE_ENTRY_FLASH_MAX 0x44000000UL                              // exclusive

//------------------------------------------------------------------------------
// WIFI PUSH ENDPOINT (HTTP, port 80)
//------------------------------------------------------------------------------
#define UPDATE_HTTP_PORT 80
#define UPDATE_HTTP_MAX_OPEN_SOCKETS 2
#define UPDATE_HTTP_STACK_SIZE 6144
#define UPDATE_HTTP_RECV_TIMEOUT_S 10                                          // Socket receive timeout (a stalled upload aborts after this)
#define UPDATE_HTTP_CHUNK_BYTES 4096                                           // Receive buffer for the image body
#define UPDATE_BEGIN_BODY_MAX 512                                              // Largest accepted /api/update/begin body
#define UPDATE_VERSION_MAX_LEN 32                                              // Longest accepted "version" string
#define UPDATE_BEARER_HEADER_MAX 48                                            // Largest accepted Authorization header value
#define UPDATE_GATE_CHECK_INTERVAL_MS 2000                                     // How often tick() re-evaluates "WiFi up and push armed"
#define UPDATE_PROGRESS_LOG_STEP_BYTES (256 * 1024)                            // Log a progress line every this many received bytes
#define UPDATE_LINGER_MAX_BYTES (64 * 1024)                                    // After an early error reply: discard at most this much of the request body ...
#define UPDATE_LINGER_MAX_MS 3000                                              // ... for at most this long, so the client sees the status instead of a TCP reset
#define UPDATE_SESSION_ID_LEN 8                                                // Session id: 8 lowercase hex characters
#define UPDATE_RESTART_DELAY_MS 800                                            // Pause between the "installed" reply and the restart

// On-device Accept prompt (see network/update_approval)
#define UPDATE_PROMPT_TIMEOUT_MS 30000                                         // Unanswered prompt expires after this (HTTP 408)
#define UPDATE_PROMPT_POLL_MS 50                                               // The waiting HTTP task polls the decision this often
#define UPDATE_DECLINE_BACKOFF_MS 30000                                        // After a decline new requests are refused (HTTP 429) for this long
#define UPDATE_PROMPT_TEXT_MAX 32                                              // Longest client-supplied text (version) shown in the prompt
#define UPDATE_PEER_IP_BUF 16                                                  // "255.255.255.255" + NUL
#define UPDATE_PROMPT_MESSAGE_BUF 192                                          // Always fits the dialog message built by prompt_build_message()

// On-device progress screen (see ui/controllers/wifi_update_controller)
#define UPDATE_UI_FAILED_HOLD_MS 3000                                          // A failed update stays on screen this long, then the UI returns
#define UPDATE_UI_STATUS_BUF 64                                                // Status line of the progress screen ("Update failed: <reason>")
#define UPDATE_UI_REASON_MAX 16                                                // Longest failure reason shown (longer ones end in "..."); keeps the line to two rows
#define UPDATE_UI_TAP_GUARD_MS 400                                             // Taps on the Accept dialog this soon after it opened are ignored

// Boot-health rollback (see system/boot_health): a freshly installed image must run this long after setup()
// before it is confirmed; a reset or crash before that makes the bootloader return to the previous image.
#define BOOT_HEALTH_CONFIRM_MS 30000

// Dev/test only (boot-fail-test env): 0 off, 1 abort() (panic), 2 esp_restart() this long after boot, but ONLY while
// the image is unconfirmed (installed by an update); a USB-flashed test image never crash-loops.
#ifndef DEBUG_BOOT_FAIL_TEST
#define DEBUG_BOOT_FAIL_TEST 0
#endif
#define DEBUG_BOOT_FAIL_DELAY_MS 8000

// Dev/test only (wifi-dev env): the HTTP side plays the user at the screen. 0 off, 1 accept, 2 decline, 3 never reply.
#ifndef DEBUG_UPDATE_PROMPT_AUTOREPLY
#define DEBUG_UPDATE_PROMPT_AUTOREPLY 0
#endif

// Authentication failure lockout (see auth_lockout_*)
#define UPDATE_AUTH_MAX_FAILURES 3
#define UPDATE_AUTH_LOCK_INITIAL_MS 60000
#define UPDATE_AUTH_LOCK_MAX_MS 900000
