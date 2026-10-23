#include "wifi_manager.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <mbedtls/platform_util.h>
#include <string.h>

#if DEBUG_WIFI_DEV_CREDS
#if __has_include("spike_secrets.h")
#include "spike_secrets.h"  // defines SPIKE_WIFI_SSID / SPIKE_WIFI_PASSWORD, optionally SPIKE_API_TOKEN (gitignored)
#else
#error "DEBUG_WIFI_DEV_CREDS needs include/spike_secrets.h (see spike_secrets.h.example)"
#endif
// Dev builds only: API token stored with the seeded credentials. spike_secrets.h may define SPIKE_API_TOKEN;
// otherwise this fixed, publicly known dev token is used (so it is NOT a secret).
#ifdef SPIKE_API_TOKEN
#define WIFI_DEV_API_TOKEN SPIKE_API_TOKEN
#else
#define WIFI_DEV_API_TOKEN "0123456789abcdef"
#endif
#endif

WiFiManager wifi_manager;

volatile bool WiFiManager::provisioning_window = false;

namespace {

const char* state_label(WiFiState s) {
    switch (s) {
        case WiFiState::NO_CREDENTIALS: return "NO_CREDENTIALS";
        case WiFiState::CONNECTING: return "CONNECTING";
        case WiFiState::CONNECTED: return "CONNECTED";
        case WiFiState::WAITING_RETRY: return "WAITING_RETRY";
    }
    return "?";
}

// Reads a string key into out (always NUL-terminated). A missing key gives an empty string without an NVS
// error log. Returns false if the key does not exist.
bool read_string(Preferences& prefs, const char* key, char* out, size_t out_len) {
    out[0] = '\0';
    if (!prefs.isKey(key)) return false;
    prefs.getString(key, out, out_len);
    out[out_len - 1] = '\0';
    return true;
}

}  // namespace

const char* WiFiManager::state_name() const {
    return state_label(wifi_state);
}

void WiFiManager::log_stack_mark(const char* where) const {
    LOG_BLE("[WIFI] Bluetooth task stack high-water %s: %u bytes\n", where,
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
}

void WiFiManager::set_state(WiFiState next) {
    if (next == wifi_state) return;
    wifi_state = next;
    // Never log credentials or the token
    if (next == WiFiState::CONNECTED) {
        char ip[16];
        get_ip(ip, sizeof(ip));
        LOG_BLE("[WIFI] state=CONNECTED ip=%s rssi=%d host=%s.local\n", ip, rssi(), host_name);
    } else if (next == WiFiState::WAITING_RETRY) {
        LOG_BLE("[WIFI] state=WAITING_RETRY reason=%d retry_in_ms=%u\n", last_disconnect_reason,
                static_cast<unsigned>(retry_at_ms - millis()));
    } else {
        LOG_BLE("[WIFI] state=%s\n", state_label(next));
    }
}

void WiFiManager::load_host_name() {
    Preferences prefs;
    if (!prefs.begin(WIFI_NVS_NAMESPACE, false)) return;
    char name[WIFI_HOSTNAME_MAX_LEN + 1];
    read_string(prefs, WIFI_NVS_KEY_NAME, name, sizeof(name));
    prefs.end();
    if (wifi_validate_hostname(name)) {
        strncpy(host_name, name, sizeof(host_name) - 1);
        host_name[sizeof(host_name) - 1] = '\0';
    }
}

void WiFiManager::begin() {
    load_host_name();

    Preferences prefs;
    if (prefs.begin(WIFI_NVS_NAMESPACE, false)) {
        char ssid[WIFI_SSID_MAX_LEN + 1];
        credentials_present = read_string(prefs, WIFI_NVS_KEY_SSID, ssid, sizeof(ssid)) && ssid[0] != '\0';
        prefs.end();
    }
#if DEBUG_WIFI_DEV_CREDS
    seed_dev_credentials();
#ifndef SPIKE_API_TOKEN
    // The fallback token is public: say so loudly on every boot (never print the token itself)
    if (token_matches(WIFI_DEV_API_TOKEN)) {
        LOG_BLE("[WIFI] WARNING: DEV BUILD uses the public fallback API token, push endpoint is NOT protected\n");
    }
#endif
#endif

    if (!credentials_present) {
        LOG_BLE("[WIFI] no credentials stored, WiFi not started\n");
        return;
    }
    apply_requested = true;  // tick() starts the radio
}

#if DEBUG_WIFI_DEV_CREDS
void WiFiManager::seed_dev_credentials() {
    if (!credentials_present) {
        if (!wifi_token_is_valid(WIFI_DEV_API_TOKEN)) {
            LOG_BLE("[WIFI] dev token invalid, dev credentials not stored\n");
        } else if (store_credentials(SPIKE_WIFI_SSID, SPIKE_WIFI_PASSWORD, WIFI_DEV_API_TOKEN)) {
            credentials_present = true;
            LOG_BLE("[WIFI] dev credentials stored\n");
        } else {
            LOG_BLE("[WIFI] dev credentials could not be stored\n");
        }
    }
    // Dev builds arm the push endpoint so it can be tested without the on-device prompt
    if (!push_enabled()) set_push_enabled(true);
}
#endif

bool WiFiManager::store_credentials(const char* ssid, const char* password, const char* token) {
    Preferences prefs;
    if (!prefs.begin(WIFI_NVS_NAMESPACE, false)) return false;
    const size_t pass_len = strlen(password);
    // Order: password, token, SSID last. A stored SSID is the commit marker, so an interrupted write never
    // leaves an SSID paired with a stale password or token.
    const bool pass_ok = pass_len == 0 ? (!prefs.isKey(WIFI_NVS_KEY_PASSWORD) || prefs.remove(WIFI_NVS_KEY_PASSWORD))
                                       : prefs.putString(WIFI_NVS_KEY_PASSWORD, password) == pass_len;
    const bool ok = pass_ok && prefs.putString(WIFI_NVS_KEY_TOKEN, token) == WIFI_TOKEN_LEN &&
                    prefs.putString(WIFI_NVS_KEY_SSID, ssid) == strlen(ssid);
    prefs.end();
    return ok;
}

WiFiManager::SetResult WiFiManager::set_credentials(const char* ssid, const char* password, const char* token) {
    if (!ssid || !password || !token) return SetResult::INVALID;
    if (!wifi_validate_ssid(reinterpret_cast<const uint8_t*>(ssid), strlen(ssid)) ||
        !wifi_validate_password(reinterpret_cast<const uint8_t*>(password), strlen(password)) ||
        !wifi_token_is_valid(token)) {
        return SetResult::INVALID;
    }
    if (!store_credentials(ssid, password, token)) return SetResult::STORE_FAILED;
    credentials_present = true;
    teardown_requested = false;
    apply_requested = true;
    LOG_BLE("[WIFI] credentials stored\n");
    return SetResult::OK;
}

void WiFiManager::clear_credentials() {
    Preferences prefs;
    if (prefs.begin(WIFI_NVS_NAMESPACE, false)) {
        // SSID first: it is the commit marker
        prefs.remove(WIFI_NVS_KEY_SSID);
        prefs.remove(WIFI_NVS_KEY_PASSWORD);
        prefs.remove(WIFI_NVS_KEY_TOKEN);
        prefs.remove(WIFI_NVS_KEY_PUSH_ENABLED);
        prefs.end();
    }
    credentials_present = false;
    apply_requested = false;
    teardown_requested = true;
    LOG_BLE("[WIFI] credentials cleared\n");
}

void WiFiManager::get_ip(char* out, size_t out_len) const {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (wifi_state != WiFiState::CONNECTED) return;
    const IPAddress ip = WiFi.localIP();
    snprintf(out, out_len, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

int WiFiManager::rssi() const {
    return wifi_state == WiFiState::CONNECTED ? WiFi.RSSI() : 0;
}

bool WiFiManager::token_matches(const char* candidate) {
    Preferences prefs;
    if (!prefs.begin(WIFI_NVS_NAMESPACE, false)) return false;
    char token[WIFI_TOKEN_LEN + 1];
    read_string(prefs, WIFI_NVS_KEY_TOKEN, token, sizeof(token));
    prefs.end();
    const bool match = wifi_token_equal(token, candidate);
    mbedtls_platform_zeroize(token, sizeof(token));
    return match;
}

bool WiFiManager::push_enabled() {
    Preferences prefs;
    if (!prefs.begin(WIFI_NVS_NAMESPACE, false)) return false;
    const bool enabled = prefs.getBool(WIFI_NVS_KEY_PUSH_ENABLED, false);
    prefs.end();
    return enabled;
}

void WiFiManager::set_push_enabled(bool enabled) {
    Preferences prefs;
    if (!prefs.begin(WIFI_NVS_NAMESPACE, false)) return;
    prefs.putBool(WIFI_NVS_KEY_PUSH_ENABLED, enabled);
    prefs.end();
}

void WiFiManager::start_radio() {
    if (radio_started) return;
    WiFi.persistent(false);  // credentials live in our NVS namespace only
    if (!events_registered) {
        // Runs on the WiFi event task: only set flags, tick() does the work
        WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
            if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
                event_got_ip = true;
            } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
                last_disconnect_reason = info.wifi_sta_disconnected.reason;
                event_disconnected = true;
            } else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP) {
                last_disconnect_reason = 0;
                event_disconnected = true;
            }
        });
        events_registered = true;
    }
    WiFi.setAutoReconnect(false);  // tick() owns the backoff
    WiFi.setHostname(host_name);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    radio_started = true;
}

void WiFiManager::start_connect() {
    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char pass[WIFI_PASSWORD_MAX_LEN + 1] = {0};
    Preferences prefs;
    if (!prefs.begin(WIFI_NVS_NAMESPACE, false)) {
        // Transient NVS failure: keep the credentials flag and try again after the backoff
        backoff_ms = wifi_backoff_next(backoff_ms);
        schedule_retry(backoff_ms);
        return;
    }
    read_string(prefs, WIFI_NVS_KEY_SSID, ssid, sizeof(ssid));
    read_string(prefs, WIFI_NVS_KEY_PASSWORD, pass, sizeof(pass));
    prefs.end();
    if (ssid[0] == '\0') {
        // The namespace opened but holds no SSID: genuinely no credentials
        credentials_present = false;
        mbedtls_platform_zeroize(pass, sizeof(pass));
        set_state(WiFiState::NO_CREDENTIALS);
        return;
    }

    start_radio();
    reapplying = false;
    event_got_ip = false;
    event_disconnected = false;
    last_disconnect_reason = 0;
    connect_started_ms = millis();
    set_state(WiFiState::CONNECTING);
    WiFi.begin(ssid, pass[0] ? pass : nullptr);  // returns immediately; the result arrives as events
    mbedtls_platform_zeroize(ssid, sizeof(ssid));
    mbedtls_platform_zeroize(pass, sizeof(pass));
    log_stack_mark("after start_connect");
}

void WiFiManager::stop_radio() {
    stop_mdns();
    if (radio_started) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        radio_started = false;
    }
    log_stack_mark("after stop_radio");
}

// mDNS is started once per radio lifetime (the IDF responder follows the interface across reconnects)
void WiFiManager::start_mdns() {
    if (mdns_started) return;
    if (MDNS.begin(host_name)) {
        mdns_started = true;
        http_service_registered = false;
        log_stack_mark("after MDNS.begin");
    } else {
        LOG_BLE("[WIFI] mDNS start failed\n");
    }
}

void WiFiManager::stop_mdns() {
    if (!mdns_started) return;
    MDNS.end();
    mdns_started = false;
    http_service_registered = false;
}

void WiFiManager::schedule_retry(uint32_t delay_ms) {
    retry_at_ms = millis() + delay_ms;
    set_state(WiFiState::WAITING_RETRY);
}

void WiFiManager::on_connected() {
    event_got_ip = false;
    event_disconnected = false;
    backoff_ms = 0;
    set_state(WiFiState::CONNECTED);
    start_mdns();
}

void WiFiManager::tick() {
    const uint32_t now = millis();

    if (teardown_requested) {
        teardown_requested = false;
        stop_radio();
        set_state(WiFiState::NO_CREDENTIALS);
        return;
    }

    if (apply_requested) {
        apply_requested = false;
        backoff_ms = 0;
        if (radio_started) {
            // New credentials while running: drop the old connection, reconnect after a short pause
            WiFi.disconnect(false);
            reapplying = true;
            last_disconnect_reason = 0;
            schedule_retry(WIFI_REAPPLY_DELAY_MS);
        } else {
            start_connect();
        }
        return;
    }

    if (!credentials_present) return;

    // Registered once an HTTP server exists (see register_http_service())
    if (mdns_started && http_service_wanted && !http_service_registered) {
        MDNS.addService("http", "tcp", 80);
        http_service_registered = true;
    } else if (mdns_started && !http_service_wanted && http_service_registered) {
        mdns_service_remove("_http", "_tcp");
        http_service_registered = false;
    }

    switch (wifi_state) {
        case WiFiState::CONNECTING:
            if (event_got_ip || WiFi.status() == WL_CONNECTED) {
                on_connected();
            } else if (event_disconnected || now - connect_started_ms > WIFI_CONNECT_TIMEOUT_MS) {
                event_disconnected = false;
                backoff_ms = wifi_backoff_next(backoff_ms);
                uint32_t delay_ms = backoff_ms;
                // A refused password will not fix itself: do not hammer the access point
                if (wifi_disconnect_is_auth_failure(last_disconnect_reason) && delay_ms < WIFI_AUTH_RETRY_MS) {
                    delay_ms = WIFI_AUTH_RETRY_MS;
                }
                schedule_retry(delay_ms);
            }
            break;

        case WiFiState::CONNECTED:
            if (event_disconnected) {
                event_disconnected = false;
                backoff_ms = wifi_backoff_next(0);
                schedule_retry(backoff_ms);
            }
            break;

        case WiFiState::WAITING_RETRY:
            if (!reapplying && WiFi.status() == WL_CONNECTED) {
                on_connected();  // the link came back by itself: it is the ground truth
            } else if (static_cast<int32_t>(now - retry_at_ms) >= 0) {
                start_connect();
            }
            break;

        case WiFiState::NO_CREDENTIALS:
            break;
    }
}
