#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <stdint.h>
#include "../config/constants.h"
#include "wifi_credentials.h"

enum class WiFiState : uint8_t {
    NO_CREDENTIALS,  // nothing stored: the radio is never started
    CONNECTING,      // association / DHCP in progress
    CONNECTED,       // has an IP address
    WAITING_RETRY    // connection failed or lost; next attempt after the backoff
};

/**
 * WiFiManager - station-mode WiFi from credentials stored in NVS (namespace "wifi").
 *
 * begin() is called once from setup() after the UI and tasks are up and returns immediately; it only
 * reads NVS and requests a connect. ALL radio work (WiFi mode/begin/disconnect, mDNS) runs in tick(),
 * which the Bluetooth task calls, so the connect is asynchronous and the backoff lives there. WiFi events
 * only set flags. SSID and password are read from NVS into stack buffers just for WiFi.begin(), zeroed
 * afterwards, and never logged (the WiFi driver keeps its own copy while running). Credentials sit in
 * plaintext NVS, see config/wifi.h.
 */
class WiFiManager {
public:
    void begin();

    // Called periodically from the Bluetooth task loop (no WiFi call blocks for long).
    void tick();

    enum class SetResult : uint8_t { OK, INVALID, STORE_FAILED };

    // Validates and stores the credentials and the API token (always rewritten; the provisioning tool
    // generates it), then (re)connects from tick(). Write order: password, token, SSID last (commit marker).
    SetResult set_credentials(const char* ssid, const char* password, const char* token);
    // Removes SSID, password, token and the push flag, and takes the radio down from tick().
    void clear_credentials();

    bool has_credentials() const { return credentials_present; }
    WiFiState state() const { return wifi_state; }
    const char* state_name() const;
    bool is_connected() const { return wifi_state == WiFiState::CONNECTED; }

    // Dotted IP (empty string when not connected), RSSI in dBm (0 when not connected)
    void get_ip(char* out, size_t out_len) const;
    int rssi() const;
    const char* hostname() const { return host_name; }
    // Constant-time check of a candidate token against the stored one (false if none is stored)
    bool token_matches(const char* candidate);

    bool push_enabled();
    void set_push_enabled(bool enabled);

    // BLE credential commands are accepted only while no credentials are stored or this window is open
    // (the WiFi menu page opens it). Closed by default.
    // BLE credential commands: allowed with no credentials stored, or while the window is open
    bool provisioning_allowed() const { return wifi_provisioning_allowed(credentials_present, provisioning_window); }
    static bool provisioning_window_open() { return provisioning_window; }
    static void set_provisioning_window(bool open) { provisioning_window = open; }

    // Call once an HTTP server listens on port 80: mDNS then advertises the "http" service
    void register_http_service() { http_service_wanted = true; }

private:
    static volatile bool provisioning_window;

    // Written from the WiFi event task, consumed by tick()
    volatile bool event_got_ip = false;
    volatile bool event_disconnected = false;

    // Set by set_credentials()/clear_credentials() from any task, consumed by tick()
    volatile bool apply_requested = false;
    volatile bool teardown_requested = false;

    WiFiState wifi_state = WiFiState::NO_CREDENTIALS;
    volatile bool credentials_present = false;
    volatile bool http_service_wanted = false;
    bool radio_started = false;
    bool events_registered = false;
    bool mdns_started = false;
    bool http_service_registered = false;
    bool reapplying = false;  // old connection is being dropped for new credentials: ignore its link state
    uint32_t backoff_ms = 0;
    uint32_t retry_at_ms = 0;
    uint32_t connect_started_ms = 0;
    int last_disconnect_reason = 0;
    char host_name[WIFI_HOSTNAME_MAX_LEN + 1] = WIFI_DEFAULT_HOSTNAME;

    void set_state(WiFiState next);
    void load_host_name();
    bool store_credentials(const char* ssid, const char* password, const char* token);
    void on_connected();
    void schedule_retry(uint32_t delay_ms);
    void start_radio();
    void start_connect();
    void stop_radio();
    void start_mdns();
    void stop_mdns();
    void log_stack_mark(const char* where) const;
#if DEBUG_WIFI_DEV_CREDS
    void seed_dev_credentials();
#endif
};

extern WiFiManager wifi_manager;
