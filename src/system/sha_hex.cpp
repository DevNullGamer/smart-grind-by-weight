#include "sha_hex.h"

namespace {
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

bool sha256_hex_parse(const char* hex, uint8_t out[SHA256_DIGEST_LEN]) {
    if (!hex || !out) return false;
    uint8_t tmp[SHA256_DIGEST_LEN];
    for (size_t i = 0; i < SHA256_DIGEST_LEN; i++) {
        const int hi = hex_value(hex[2 * i]);
        if (hi < 0) return false;  // a premature NUL is not a hex digit, so we never read past the terminator
        const int lo = hex_value(hex[2 * i + 1]);
        if (lo < 0) return false;
        tmp[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    if (hex[SHA256_HEX_LEN] != '\0') return false;  // exactly 64 characters
    for (size_t i = 0; i < SHA256_DIGEST_LEN; i++) out[i] = tmp[i];
    return true;
}

void sha256_to_hex(const uint8_t digest[SHA256_DIGEST_LEN], char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (!digest || out_len < SHA256_HEX_LEN + 1) {
        out[0] = '\0';
        return;
    }
    static const char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < SHA256_DIGEST_LEN; i++) {
        out[2 * i] = kDigits[digest[i] >> 4];
        out[2 * i + 1] = kDigits[digest[i] & 0x0F];
    }
    out[SHA256_HEX_LEN] = '\0';
}

bool sha256_equal(const uint8_t a[SHA256_DIGEST_LEN], const uint8_t b[SHA256_DIGEST_LEN]) {
    uint8_t diff = 0;
    for (size_t i = 0; i < SHA256_DIGEST_LEN; i++) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}
