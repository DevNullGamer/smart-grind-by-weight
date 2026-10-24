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

// Release index published on GitHub Pages (see network/release_index and tools/update_index.py).
// The base URL is fixed in the firmware; the "url" in an index file must start with it.
#define UPDATE_PULL_BASE_URL "https://devnullgamer.github.io/smart-grind-by-weight/"
#define UPDATE_PULL_STABLE_FILE "update-stable.json"
#define UPDATE_PULL_BETA_FILE "update-beta.json"
#define UPDATE_PULL_INDEX_MAX_BYTES 2048                                       // Largest accepted index file
#define UPDATE_PULL_INDEX_SCHEMA 1                                             // Only schema accepted by the parser
#define UPDATE_PULL_STRING_MAX 128                                             // Longest string value in an index file

// Pull engine (see network/update_pull, network/release_client, system/mbedtls_psram_override)
#define UPDATE_PULL_TASK_STACK 10240                                           // Worker task stack (internal RAM); high-water is logged
#define UPDATE_PULL_TASK_PRIORITY 1                                            // Below the UI task (2)
#define UPDATE_PULL_TASK_CORE 0                                                // With the WiFi/network stack
#define UPDATE_PULL_CHUNK_BYTES 4096                                           // Download buffer (PSRAM, internal fallback)
#define UPDATE_PULL_READ_TIMEOUT_MS 15000                                      // Socket timeout: no data for this long aborts the job
#define UPDATE_PULL_MIN_FREE_INTERNAL_BYTES (42 * 1024)                        // Refuse to start below this much free internal heap (measured on COM15: start 47.7-56.5 KB, TLS dips ~9 KB, lowest 44 KB)
#define UPDATE_PULL_URL_MAX (UPDATE_PULL_STRING_MAX + 32)                      // Index URL buffer; the image URL comes from the (<= 128 char) index
#define UPDATE_PULL_DOWNLOAD_MAX_MS (10UL * 60UL * 1000UL)                    // Overall download deadline (a trickling server cannot hold the grinder locked forever)
#define UPDATE_PULL_PROGRESS_LOG_STEP_BYTES (256 * 1024)                       // Log a progress line every this many downloaded bytes

// Pull UI in the WiFi menu page (see network/pull_ui_text, ui/screens/menu_screen_wifi)
#define UPDATE_PULL_RESULT_STALE_MS (10UL * 60UL * 1000UL)                     // A check result older than this no longer offers Install (re-check required)
#define UPDATE_PULL_NOTICE_MS 6000                                             // A refusal message ("Disconnect Bluetooth first") stays on the page this long

// Dev/test only, wifi-dev envs (they #error without DEBUG_WIFI_DEV_CREDS):
//   DEBUG_PULL_ALLOW_HTTP  accept http:// URLs (PC test server tools/wifi/pull_test_server.py)
//   DEBUG_PULL_BASE_URL    replaces UPDATE_PULL_BASE_URL, e.g. "http://192.168.1.20:8080/". Optional SPIKE_PULL_BASE_URL in
//                          the gitignored include/spike_secrets.h does the same without editing platformio.ini
//   DEBUG_PULL_AUTORUN     1: Check + Install ~25 s after boot (log only), 2: HTTPS smoke test against the real Pages site
#ifndef DEBUG_PULL_ALLOW_HTTP
#define DEBUG_PULL_ALLOW_HTTP 0
#endif
#ifndef DEBUG_PULL_AUTORUN
#define DEBUG_PULL_AUTORUN 0
#endif
#ifndef DEBUG_PULL_AUTORUN_BETA
#define DEBUG_PULL_AUTORUN_BETA 1                                              // AUTORUN 1 checks the beta channel (test images are release candidates)
#endif
#define DEBUG_PULL_AUTORUN_DELAY_MS 25000
// (not for the smoke test, DEBUG_PULL_AUTORUN == 2, which must use the real Pages URL)
#if DEBUG_PULL_ALLOW_HTTP && DEBUG_PULL_AUTORUN != 2 && !defined(DEBUG_PULL_BASE_URL) && __has_include("spike_secrets.h")
#include "spike_secrets.h"
#ifdef SPIKE_PULL_BASE_URL
#define DEBUG_PULL_BASE_URL SPIKE_PULL_BASE_URL
#endif
#endif
#if (DEBUG_PULL_ALLOW_HTTP || DEBUG_PULL_AUTORUN || defined(DEBUG_PULL_BASE_URL)) && !DEBUG_WIFI_DEV_CREDS
#error "DEBUG_PULL_* is for the wifi-dev envs only (they define DEBUG_WIFI_DEV_CREDS)"
#endif
#ifdef DEBUG_PULL_BASE_URL
#undef UPDATE_PULL_BASE_URL
#define UPDATE_PULL_BASE_URL DEBUG_PULL_BASE_URL
#endif

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
