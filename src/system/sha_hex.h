#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure SHA-256 hex helpers (no Arduino/ESP dependencies, unit tested on the host).
#define SHA256_DIGEST_LEN 32
#define SHA256_HEX_LEN 64

// Parses exactly 64 hex characters (either case) into 32 bytes. False on any other input; out is untouched then.
bool sha256_hex_parse(const char* hex, uint8_t out[SHA256_DIGEST_LEN]);

// Writes 64 lowercase hex characters plus NUL; needs out_len >= SHA256_HEX_LEN + 1 (otherwise writes an empty string).
void sha256_to_hex(const uint8_t digest[SHA256_DIGEST_LEN], char* out, size_t out_len);

// Constant-time comparison of two digests
bool sha256_equal(const uint8_t a[SHA256_DIGEST_LEN], const uint8_t b[SHA256_DIGEST_LEN]);
