#pragma once

#include <cstddef>
#include <cstdint>

// Names the disconnect reason of a GLink link for the log (chg023). NimBLE reports a disconnect as BLE_HS_HCI_ERR(hci),
// that is 0x200 + the HCI reason code (Bluetooth Core spec, Vol 2 Part F 1.3); other values are NimBLE host errors.
// Pure: no Arduino, no NimBLE, no heap. Shared with gaggimate-plugin (byte-identical copies).

// Name of an HCI reason code, e.g. 0x08 "supervision timeout"; nullptr when the code is not named here.
const char* glink_disconnect_reason_name(uint8_t hci_code);

// Writes "reason 0x08 (supervision timeout)" for a NimBLE disconnect reason, "reason 0x2A" for an HCI code without a
// name, "reason none" for 0 and "reason 0x00E (not an HCI code)" for a host error. Always NUL-terminated when cap is not 0.
void glink_disconnect_describe(char* out, size_t cap, int nimble_reason);

// Radio diagnostics (chg023 step 1b). An ESP-IDF esp_power_level_t value (ESP-IDF 5 order: 0 = N24 ... 11 = P9, 15 = P20,
// 0xFF = invalid) to dBm. False for a value the enum does not name.
bool glink_tx_power_level_dbm(int level, int* dbm);

// Writes a signed dBm value: "+9 dBm", "-63 dBm". NUL-terminated when cap is not 0.
void glink_format_dbm(char* out, size_t cap, int dbm);
