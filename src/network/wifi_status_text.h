#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure text helpers for the WiFi menu page. No Arduino/ESP dependencies so they can be unit tested on the host.

enum class WiFiState : uint8_t {
    NO_CREDENTIALS,  // nothing stored: the radio is never started
    CONNECTING,      // association / DHCP in progress
    CONNECTED,       // has an IP address
    WAITING_RETRY    // connection failed or lost; next attempt after the backoff
};

// Longest SSID text shown on the page (including a trailing "..."); the SSID itself may be 32 bytes.
#define WIFI_STATUS_SSID_DISPLAY_MAX 20
// Buffer size that always fits the output of the functions below
#define WIFI_STATUS_TEXT_BUF 32

// "Connected" / "Connecting..." / "Retry in 20s" / "Not set up". retry_in_ms is only used for WAITING_RETRY
// (rounded up to whole seconds, at least 1s). Returns out; always NUL-terminated for out_len > 0.
const char* wifi_status_text(WiFiState state, uint32_t retry_in_ms, char* out, size_t out_len);

// Excellent (>= -55 dBm), Good (>= -67), Fair (>= -75), otherwise Weak
const char* wifi_rssi_quality(int rssi_dbm);

// "-62 dBm Good"; "--" when rssi_dbm is 0 (not connected) or positive (invalid)
const char* wifi_signal_text(int rssi_dbm, char* out, size_t out_len);

// Copy of an SSID for display: printable ASCII only (every run of other bytes, e.g. UTF-8 sequences or control
// characters, becomes one '?'), shortened to WIFI_STATUS_SSID_DISPLAY_MAX characters with a "..." suffix. Empty or
// null input gives "--". Returns out.
const char* wifi_shorten_ssid(const char* ssid, char* out, size_t out_len);
