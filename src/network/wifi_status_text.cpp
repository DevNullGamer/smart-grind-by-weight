#include "wifi_status_text.h"
#include <stdio.h>
#include <string.h>

namespace {

const char* copy_text(const char* text, char* out, size_t out_len) {
    if (!out || out_len == 0) return out;
    snprintf(out, out_len, "%s", text);
    return out;
}

}  // namespace

const char* wifi_status_text(WiFiState state, uint32_t retry_in_ms, char* out, size_t out_len) {
    switch (state) {
        case WiFiState::CONNECTED: return copy_text("Connected", out, out_len);
        case WiFiState::CONNECTING: return copy_text("Connecting...", out, out_len);
        case WiFiState::WAITING_RETRY: {
            uint32_t seconds = retry_in_ms / 1000 + (retry_in_ms % 1000 ? 1 : 0);
            if (seconds == 0) seconds = 1;
            if (!out || out_len == 0) return out;
            snprintf(out, out_len, "Retry in %us", static_cast<unsigned>(seconds));
            return out;
        }
        case WiFiState::NO_CREDENTIALS: break;
    }
    return copy_text("Not set up", out, out_len);
}

const char* wifi_rssi_quality(int rssi_dbm) {
    if (rssi_dbm >= -55) return "Excellent";
    if (rssi_dbm >= -67) return "Good";
    if (rssi_dbm >= -75) return "Fair";
    return "Weak";
}

const char* wifi_signal_text(int rssi_dbm, char* out, size_t out_len) {
    if (rssi_dbm >= 0) return copy_text("--", out, out_len);
    if (!out || out_len == 0) return out;
    snprintf(out, out_len, "%d dBm %s", rssi_dbm, wifi_rssi_quality(rssi_dbm));
    return out;
}

const char* wifi_shorten_ssid(const char* ssid, char* out, size_t out_len) {
    if (!out || out_len == 0) return out;
    if (!ssid || ssid[0] == '\0') return copy_text("--", out, out_len);

    // Sanitise first (runs of non-printable bytes collapse into one '?'), then shorten
    char clean[WIFI_STATUS_SSID_DISPLAY_MAX * 2 + 4];
    size_t n = 0;
    bool in_bad_run = false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(ssid); *p; p++) {
        if (*p >= 0x20 && *p <= 0x7E) {
            in_bad_run = false;
            clean[n++] = static_cast<char>(*p);
        } else if (!in_bad_run) {
            in_bad_run = true;
            clean[n++] = '?';
        }
        if (n >= sizeof(clean) - 1) break;  // far beyond what is displayed; the length check below still shortens
    }
    clean[n] = '\0';

    if (n > WIFI_STATUS_SSID_DISPLAY_MAX) {
        n = WIFI_STATUS_SSID_DISPLAY_MAX - 3;
        memcpy(clean + n, "...", 4);
    }
    return copy_text(clean, out, out_len);
}
