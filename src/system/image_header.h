#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/update.h"

// Pure validation of the first bytes of an ESP32-S3 application image (no Arduino/ESP dependencies,
// unit tested on the host). Layout (esp_image_header_t, 24 B, little endian) followed by the first
// esp_image_segment_header_t (8 B) and then esp_app_desc_t, which starts with its magic word:
//   0 magic 0xE9 | 1 segment count | 4..7 entry address | 12..13 chip id | 32..35 app descriptor magic

enum class ImageHeaderResult : uint8_t {
    OK,
    TOO_SHORT,      // fewer than UPDATE_IMAGE_HEADER_BYTES bytes supplied
    BAD_MAGIC,      // first byte is not 0xE9
    BAD_SEGMENTS,   // segment count 0 or > 16
    WRONG_CHIP,     // chip id is not ESP32-S3
    BAD_ENTRY,      // entry address outside the IRAM / flash instruction ranges
    BAD_APP_DESC,   // app descriptor magic missing at offset 32
    TOO_SMALL,      // total image size below UPDATE_IMAGE_MIN_SIZE (or 0)
    TOO_LARGE       // total image size exceeds the slot
};

// data/len: the first bytes of the image (>= UPDATE_IMAGE_HEADER_BYTES); total_size: announced image size;
// slot_size: size of the target OTA partition. Checks run in the order of the enum.
ImageHeaderResult image_header_validate(const uint8_t* data, size_t len, uint32_t total_size, uint32_t slot_size);

// Short human readable text for a result (static string)
const char* image_header_result_text(ImageHeaderResult result);
