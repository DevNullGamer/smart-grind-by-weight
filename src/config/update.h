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
