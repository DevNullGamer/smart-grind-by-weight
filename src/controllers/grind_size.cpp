#include "grind_size.h"
#include <math.h>
#include <cmath>
#include <stdio.h>
#include <string.h>
#include "../system/ui_settings.h"

static_assert(GRIND_SIZE_MAX_TENTHS == 9999 && GLINK_GRIND_SIZE_MAX == 999.9f, "grind size range differs from config/bluetooth.h");
static_assert(GRIND_SIZE_BLOB_BYTES == sizeof(float) + 8, "blob layout: float + 8 unit bytes");
static_assert(GRIND_UNIT_BUF <= 8, "the unit must fit its 8 stored bytes");

namespace {

const GrindSize kNoSize;

bool unit_char_ok(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
           c == '/' || c == '%' || c == '-' || c == '_';
}

// Characters of a unit (up to max); false for a character outside the charset, a space at either end or a missing NUL
bool unit_valid(const char* unit, size_t buf) {
    size_t n = 0;
    while (n < buf && unit[n] != '\0') {
        if (!unit_char_ok(unit[n])) return false;
        n++;
    }
    if (n == buf) return false;  // not terminated inside the buffer
    if (n > 0 && (unit[0] == ' ' || unit[n - 1] == ' ')) return false;
    return true;
}

}  // namespace

bool grind_size_valid(const GrindSize& size) {
    if (!size.present) return size.tenths == 0 && size.unit[0] == '\0';
    if (size.tenths < 0 || size.tenths > GRIND_SIZE_MAX_TENTHS) return false;
    return unit_valid(size.unit, GRIND_UNIT_BUF);
}

bool grind_size_equal(const GrindSize& a, const GrindSize& b) {
    if (!a.present || !b.present) return a.present == b.present;
    return a.tenths == b.tenths && strncmp(a.unit, b.unit, GRIND_UNIT_BUF) == 0;
}

size_t grind_size_format(char* out, size_t cap, const GrindSize& size) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!size.present || !grind_size_valid(size)) return 0;
    const bool has_unit = size.unit[0] != '\0';
    int n = snprintf(out, cap, "Grind: %d.%d%s%s", static_cast<int>(size.tenths / 10), static_cast<int>(size.tenths % 10),
                     has_unit ? " " : "", has_unit ? size.unit : "");
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

size_t grind_size_encode(const GrindSize& size, uint8_t out[GRIND_SIZE_BLOB_BYTES]) {
    memset(out, 0, GRIND_SIZE_BLOB_BYTES);
    const float value = static_cast<float>(size.tenths) / 10.0f;
    memcpy(out, &value, sizeof(float));
    for (size_t i = 0; i < GRIND_UNIT_BUF - 1 && size.unit[i] != '\0'; i++) out[sizeof(float) + i] = static_cast<uint8_t>(size.unit[i]);
    return GRIND_SIZE_BLOB_BYTES;
}

bool grind_size_decode(const uint8_t* data, size_t len, GrindSize* out) {
    if (!out) return false;
    *out = GrindSize();
    if (!data || len != GRIND_SIZE_BLOB_BYTES) return false;
    float value = 0.0f;
    memcpy(&value, data, sizeof(float));
    if (!std::isfinite(value) || value < 0.0f || value > static_cast<float>(GRIND_SIZE_MAX_TENTHS) / 10.0f + 0.05f) return false;
    const int32_t tenths = static_cast<int32_t>(lroundf(value * 10.0f));
    if (tenths < 0 || tenths > GRIND_SIZE_MAX_TENTHS) return false;
    const uint8_t* unit_bytes = data + sizeof(float);
    // 8 stored bytes: at most GRIND_UNIT_BUF-1 characters, the rest zero
    for (size_t i = GRIND_UNIT_BUF - 1; i < 8; i++) {
        if (unit_bytes[i] != 0) return false;
    }
    GrindSize decoded;
    decoded.present = true;
    decoded.tenths = tenths;
    memcpy(decoded.unit, unit_bytes, GRIND_UNIT_BUF - 1);
    decoded.unit[GRIND_UNIT_BUF - 1] = '\0';
    if (!grind_size_valid(decoded)) return false;
    *out = decoded;
    return true;
}

void GrindSizeStore::load() {
    for (int slot = 0; slot < GRIND_SIZE_SLOT_COUNT; slot++) {
        slots_[slot] = GrindSize();
        if (!store_) continue;
        uint8_t blob[GRIND_SIZE_BLOB_BYTES + 1];  // one spare byte: a longer entry reads as 0 bytes (never truncated)
        const size_t got = store_->get_blob(ui_settings_keys::kGrinderNs, ui_settings_keys::kGrindSize[slot], blob, sizeof(blob));
        GrindSize decoded;
        if (grind_size_decode(blob, got, &decoded)) slots_[slot] = decoded;
    }
}

const GrindSize& GrindSizeStore::get(int slot) const {
    return (slot >= 0 && slot < GRIND_SIZE_SLOT_COUNT) ? slots_[slot] : kNoSize;
}

GrindSizeStoreResult GrindSizeStore::store(int slot, const GrindSize& size) {
    if (!store_ || slot < 0 || slot >= GRIND_SIZE_SLOT_COUNT || !grind_size_valid(size)) return GrindSizeStoreResult::FAILED;
    if (grind_size_equal(slots_[slot], size)) return GrindSizeStoreResult::UNCHANGED;
    if (!size.present) return clear(slot) ? GrindSizeStoreResult::STORED : GrindSizeStoreResult::FAILED;
    uint8_t blob[GRIND_SIZE_BLOB_BYTES];
    grind_size_encode(size, blob);
    if (!store_->put_blob(ui_settings_keys::kGrinderNs, ui_settings_keys::kGrindSize[slot], blob, sizeof(blob))) {
        return GrindSizeStoreResult::FAILED;
    }
    slots_[slot] = size;
    return GrindSizeStoreResult::STORED;
}

bool GrindSizeStore::clear(int slot) {
    if (!store_ || slot < 0 || slot >= GRIND_SIZE_SLOT_COUNT) return false;
    if (!store_->erase_blob(ui_settings_keys::kGrinderNs, ui_settings_keys::kGrindSize[slot])) return false;
    slots_[slot] = GrindSize();
    return true;
}
