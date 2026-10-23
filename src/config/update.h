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
#define UPDATE_RESTART_DELAY_MS 800                                            // Pause between the "installed" reply and the restart

// Authentication failure lockout (see auth_lockout_*)
#define UPDATE_AUTH_MAX_FAILURES 3
#define UPDATE_AUTH_LOCK_INITIAL_MS 60000
#define UPDATE_AUTH_LOCK_MAX_MS 900000
