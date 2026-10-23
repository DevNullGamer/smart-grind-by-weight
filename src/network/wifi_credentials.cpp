#include "wifi_credentials.h"
#include <string.h>

static bool has_nul(const uint8_t* data, size_t len) {
    return len > 0 && memchr(data, 0, len) != nullptr;
}

bool wifi_validate_ssid(const uint8_t* ssid, size_t len) {
    if (len < 1 || len > WIFI_SSID_MAX_LEN) return false;
    if (!ssid) return false;
    return !has_nul(ssid, len);
}

bool wifi_validate_password(const uint8_t* pass, size_t len) {
    if (len == 0) return true;  // open network
    if (len < WIFI_PASSWORD_MIN_LEN || len > WIFI_PASSWORD_MAX_LEN) return false;
    if (!pass) return false;
    return !has_nul(pass, len);
}

bool wifi_validate_hostname(const char* name) {
    if (!name) return false;
    const size_t len = strlen(name);
    if (len < 1 || len > WIFI_HOSTNAME_MAX_LEN) return false;
    if (name[0] == '-' || name[len - 1] == '-') return false;
    for (size_t i = 0; i < len; i++) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool wifi_token_is_valid(const char* token) {
    if (!token) return false;
    for (size_t i = 0; i < WIFI_TOKEN_LEN; i++) {
        const char c = token[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;  // also rejects a short string (NUL)
    }
    return token[WIFI_TOKEN_LEN] == '\0';
}

bool wifi_validate_token_bytes(const uint8_t* token, size_t len) {
    if (len != WIFI_TOKEN_LEN || !token) return false;
    for (size_t i = 0; i < len; i++) {
        const uint8_t c = token[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;  // NUL fails here too
    }
    return true;
}

bool wifi_token_equal(const char* a, const char* b) {
    if (!wifi_token_is_valid(a) || !wifi_token_is_valid(b)) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < WIFI_TOKEN_LEN; i++) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool wifi_provisioning_allowed(bool has_credentials, bool window_open) {
    return !has_credentials || window_open;
}

bool wifi_disconnect_is_auth_failure(int reason) {
    // 2 AUTH_EXPIRE, 15 4WAY_HANDSHAKE_TIMEOUT, 202 AUTH_FAIL, 203 ASSOC_FAIL, 204 HANDSHAKE_TIMEOUT
    return reason == 2 || reason == 15 || reason == 202 || reason == 203 || reason == 204;
}

uint32_t wifi_backoff_next(uint32_t current_ms) {
    if (current_ms < WIFI_BACKOFF_INITIAL_MS) return WIFI_BACKOFF_INITIAL_MS;
    if (current_ms >= WIFI_BACKOFF_MAX_MS / 2) return WIFI_BACKOFF_MAX_MS;  // also guards the doubling
    return current_ms * 2;
}

WifiProvisionResult wifi_parse_provisioning(const uint8_t* payload, size_t len, WifiProvisionFields* out) {
    if (!out) return WifiProvisionResult::TRUNCATED;
    memset(out, 0, sizeof(*out));
    if (!payload || len < 1) return WifiProvisionResult::TRUNCATED;

    const size_t ssid_len = payload[0];
    if (ssid_len < 1 || ssid_len > WIFI_SSID_MAX_LEN) return WifiProvisionResult::BAD_SSID;
    if (len < 1 + ssid_len + 1) return WifiProvisionResult::TRUNCATED;
    const uint8_t* ssid = payload + 1;

    const size_t pass_len = payload[1 + ssid_len];
    const size_t pass_off = 2 + ssid_len;
    if (len - pass_off < pass_len) return WifiProvisionResult::TRUNCATED;
    const uint8_t* pass = payload + pass_off;

    if (!wifi_validate_ssid(ssid, ssid_len)) return WifiProvisionResult::BAD_SSID;
    if (!wifi_validate_password(pass, pass_len)) return WifiProvisionResult::BAD_PASSWORD;

    const size_t token_off = pass_off + pass_len;
    const size_t remaining = len - token_off;
    if (remaining < WIFI_TOKEN_LEN) return WifiProvisionResult::TRUNCATED;
    if (remaining > WIFI_TOKEN_LEN) return WifiProvisionResult::TRAILING_BYTES;
    if (!wifi_validate_token_bytes(payload + token_off, WIFI_TOKEN_LEN)) return WifiProvisionResult::BAD_TOKEN;

    memcpy(out->ssid, ssid, ssid_len);
    memcpy(out->password, pass, pass_len);
    memcpy(out->token, payload + token_off, WIFI_TOKEN_LEN);
    return WifiProvisionResult::OK;
}
