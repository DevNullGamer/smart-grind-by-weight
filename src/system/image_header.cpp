#include "image_header.h"

namespace {
uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

bool entry_is_plausible(uint32_t entry) {
    return (entry >= UPDATE_IMAGE_ENTRY_IRAM_MIN && entry < UPDATE_IMAGE_ENTRY_IRAM_MAX) ||
           (entry >= UPDATE_IMAGE_ENTRY_FLASH_MIN && entry < UPDATE_IMAGE_ENTRY_FLASH_MAX);
}
}  // namespace

ImageHeaderResult image_header_validate(const uint8_t* data, size_t len, uint32_t total_size, uint32_t slot_size) {
    if (!data || len < UPDATE_IMAGE_HEADER_BYTES) return ImageHeaderResult::TOO_SHORT;
    if (data[0] != 0xE9) return ImageHeaderResult::BAD_MAGIC;
    if (data[1] < 1 || data[1] > UPDATE_IMAGE_MAX_SEGMENTS) return ImageHeaderResult::BAD_SEGMENTS;
    if (read_u16_le(data + 12) != UPDATE_IMAGE_CHIP_ID_ESP32S3) return ImageHeaderResult::WRONG_CHIP;
    if (!entry_is_plausible(read_u32_le(data + 4))) return ImageHeaderResult::BAD_ENTRY;
    if (read_u32_le(data + 32) != UPDATE_IMAGE_APP_DESC_MAGIC) return ImageHeaderResult::BAD_APP_DESC;
    if (total_size < UPDATE_IMAGE_MIN_SIZE) return ImageHeaderResult::TOO_SMALL;
    if (total_size > slot_size) return ImageHeaderResult::TOO_LARGE;
    return ImageHeaderResult::OK;
}

const char* image_header_result_text(ImageHeaderResult result) {
    switch (result) {
        case ImageHeaderResult::OK: return "ok";
        case ImageHeaderResult::TOO_SHORT: return "image header too short";
        case ImageHeaderResult::BAD_MAGIC: return "not a firmware image";
        case ImageHeaderResult::BAD_SEGMENTS: return "bad segment count";
        case ImageHeaderResult::WRONG_CHIP: return "image is not for ESP32-S3";
        case ImageHeaderResult::BAD_ENTRY: return "bad entry address";
        case ImageHeaderResult::BAD_APP_DESC: return "app descriptor missing";
        case ImageHeaderResult::TOO_SMALL: return "image too small";
        case ImageHeaderResult::TOO_LARGE: return "image too large";
    }
    return "unknown";
}
