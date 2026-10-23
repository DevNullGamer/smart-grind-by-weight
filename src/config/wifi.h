#pragma once

//==============================================================================
// WIFI CONFIGURATION
//==============================================================================
// Plain #defines only: this file is also included by the host-testable
// network/wifi_credentials module (no Arduino dependencies).

//------------------------------------------------------------------------------
// CREDENTIAL LIMITS (IEEE 802.11 / WPA2-PSK)
//------------------------------------------------------------------------------
#define WIFI_SSID_MAX_LEN 32                                                   // SSID: 1-32 bytes
#define WIFI_PASSWORD_MIN_LEN 8                                                // WPA2 passphrase: 8-63 bytes (empty = open network)
#define WIFI_PASSWORD_MAX_LEN 63
#define WIFI_TOKEN_LEN 16                                                      // Provisioning/API token: 16 hex characters
#define WIFI_HOSTNAME_MAX_LEN 63                                               // mDNS label limit

//------------------------------------------------------------------------------
// STORAGE (NVS namespace "wifi")
//------------------------------------------------------------------------------
// NOTE: SSID, password and token are stored in PLAINTEXT in the default (unencrypted) NVS partition.
// Anyone with physical/USB access can read them; flash encryption / NVS encryption is out of scope.
#define WIFI_NVS_NAMESPACE "wifi"
#define WIFI_NVS_KEY_SSID "ssid"
#define WIFI_NVS_KEY_PASSWORD "pass"
#define WIFI_NVS_KEY_TOKEN "token"
#define WIFI_NVS_KEY_PUSH_ENABLED "push_enabled"                               // bool, default false
#define WIFI_NVS_KEY_NAME "name"                                               // mDNS hostname, default WIFI_DEFAULT_HOSTNAME
#define WIFI_DEFAULT_HOSTNAME "grindbyweight"

//------------------------------------------------------------------------------
// CONNECTION BACKOFF
//------------------------------------------------------------------------------
#define WIFI_BACKOFF_INITIAL_MS 5000                                           // First retry delay after a failed/lost connection
#define WIFI_BACKOFF_MAX_MS 60000                                              // Delay doubles up to this cap

#define WIFI_AUTH_RETRY_MS 30000                                               // Minimum retry delay after an authentication failure (wrong password)
#define WIFI_CONNECT_TIMEOUT_MS 30000                                          // A connect attempt with neither IP nor disconnect event counts as failed
#define WIFI_REAPPLY_DELAY_MS 300                                              // Pause between dropping the old connection and connecting with new credentials
