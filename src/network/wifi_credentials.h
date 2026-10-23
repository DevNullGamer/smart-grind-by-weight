#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/wifi.h"

// Pure helpers for WiFi credentials. No Arduino/ESP dependencies so they can be unit tested on the host.

// SSID: 1-32 bytes, no NUL.
bool wifi_validate_ssid(const uint8_t* ssid, size_t len);

// Password: empty (open network) or 8-63 bytes, no NUL.
bool wifi_validate_password(const uint8_t* pass, size_t len);

// mDNS hostname: 1-63 characters of [a-z0-9-], not starting or ending with '-'.
bool wifi_validate_hostname(const char* name);

// Token stored/compared as a C string: exactly WIFI_TOKEN_LEN lowercase hex characters, then NUL.
bool wifi_token_is_valid(const char* token);

// Token as raw payload bytes (not NUL-terminated): length must be exactly WIFI_TOKEN_LEN, lowercase hex only.
bool wifi_validate_token_bytes(const uint8_t* token, size_t len);

// Constant-time comparison of two valid token strings. False if either is invalid.
bool wifi_token_equal(const char* a, const char* b);

// BLE credential commands are allowed only for first-time setup (no credentials) or while the WiFi menu is open.
bool wifi_provisioning_allowed(bool has_credentials, bool window_open);

// True for disconnect reasons that mean the password/authentication was refused (retry slowly).
bool wifi_disconnect_is_auth_failure(int reason);

// Next reconnect delay: 0 (or below the initial delay) -> WIFI_BACKOFF_INITIAL_MS, then doubles,
// capped at WIFI_BACKOFF_MAX_MS. Never overflows.
uint32_t wifi_backoff_next(uint32_t current_ms);

enum class WifiProvisionResult : uint8_t {
    OK,
    TRUNCATED,      // payload ends before a length byte, the announced bytes, or the full token
    BAD_SSID,       // SSID length 0 / > 32 or contains NUL
    BAD_PASSWORD,   // password length 1-7 / > 63 or contains NUL
    BAD_TOKEN,      // 16 token bytes present but not lowercase hex
    TRAILING_BYTES  // bytes left over after the token
};

struct WifiProvisionFields {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char password[WIFI_PASSWORD_MAX_LEN + 1];
    char token[WIFI_TOKEN_LEN + 1];
};

// Parses [ssid_len][ssid][pass_len][pass][token16] (the command byte already stripped; the token has no length byte). The payload must match exactly.
// Fields are NUL-terminated copies; the whole struct is zeroed on any failure.
WifiProvisionResult wifi_parse_provisioning(const uint8_t* payload, size_t len, WifiProvisionFields* out);
