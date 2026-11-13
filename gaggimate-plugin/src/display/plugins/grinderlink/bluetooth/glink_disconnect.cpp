#include "glink_disconnect.h"

#include <cstdio>

namespace {

constexpr unsigned HCI_ERR_BASE = 0x200;  // BLE_HS_HCI_ERR(hci) in NimBLE's ble_hs.h
constexpr unsigned HCI_CODE_MAX = 0xFF;

struct NamedReason {
    uint8_t code;
    const char* name;
};

// The reasons a GLink link is likely to end with: link supervision, the other side or this device stopping it, setup
// failures and the security checks.
constexpr NamedReason NAMED_REASONS[] = {
    {0x08, "supervision timeout"},
    {0x13, "remote user terminated"},
    {0x14, "remote low resources"},
    {0x15, "remote power off"},
    {0x16, "local host terminated"},
    {0x22, "LMP response timeout"},
    {0x28, "instant passed"},
    {0x3D, "MIC failure"},
    {0x3E, "connection failed to establish"},
};

}  // namespace

const char* glink_disconnect_reason_name(uint8_t hci_code) {
    for (const NamedReason& named : NAMED_REASONS) {
        if (named.code == hci_code) return named.name;
    }
    return nullptr;
}

void glink_disconnect_describe(char* out, size_t cap, int nimble_reason) {
    if (out == nullptr || cap == 0) return;
    if (nimble_reason == 0) {
        snprintf(out, cap, "reason none");
        return;
    }
    const unsigned value = static_cast<unsigned>(nimble_reason);
    if (value < HCI_ERR_BASE || value > HCI_ERR_BASE + HCI_CODE_MAX) {
        snprintf(out, cap, "reason 0x%03X (not an HCI code)", value);
        return;
    }
    const uint8_t hci = static_cast<uint8_t>(value - HCI_ERR_BASE);
    const char* name = glink_disconnect_reason_name(hci);
    if (name != nullptr) {
        snprintf(out, cap, "reason 0x%02X (%s)", static_cast<unsigned>(hci), name);
    } else {
        snprintf(out, cap, "reason 0x%02X", static_cast<unsigned>(hci));
    }
}

namespace {

// esp_power_level_t 0..15 in ESP-IDF 5 order (15 is P20; the deprecated P21 has the same value)
constexpr int TX_POWER_DBM[] = {-24, -21, -18, -15, -12, -9, -6, -3, 0, 3, 6, 9, 12, 15, 18, 20};

}  // namespace

bool glink_tx_power_level_dbm(int level, int* dbm) {
    if (dbm == nullptr || level < 0 || level >= static_cast<int>(sizeof(TX_POWER_DBM) / sizeof(TX_POWER_DBM[0]))) return false;
    *dbm = TX_POWER_DBM[level];
    return true;
}

void glink_format_dbm(char* out, size_t cap, int dbm) {
    if (out == nullptr || cap == 0) return;
    snprintf(out, cap, "%+d dBm", dbm);
}
