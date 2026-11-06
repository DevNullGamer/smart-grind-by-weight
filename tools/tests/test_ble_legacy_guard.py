"""Text guard (chg020 step 5): the pre-GLink BLE UUIDs and handlers must not change silently.

The hashes pin the handler bodies as they were before the GLink service was added (identical to
commit 1de1339). A deliberate change to an old handler must update the hash here in the same commit.
"""
import hashlib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANAGER = (ROOT / "src/bluetooth/manager.cpp").read_text(encoding="utf-8")
CONFIG = (ROOT / "src/config/bluetooth.h").read_text(encoding="utf-8")

LEGACY_UUIDS = {
    "BLE_OTA_SERVICE_UUID": "12345678-1234-1234-1234-123456789abc",
    "BLE_OTA_DATA_CHAR_UUID": "87654321-4321-4321-4321-cba987654321",
    "BLE_OTA_CONTROL_CHAR_UUID": "11111111-2222-3333-4444-555555555555",
    "BLE_OTA_STATUS_CHAR_UUID": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
    "BLE_OTA_BUILD_NUMBER_CHAR_UUID": "66666666-7777-8888-9999-000000000000",
    "BLE_DATA_SERVICE_UUID": "22334455-6677-8899-aabb-ccddeeffffaa",
    "BLE_DATA_CONTROL_CHAR_UUID": "33445566-7788-99aa-bbcc-ddeeffaabbcc",
    "BLE_DATA_TRANSFER_CHAR_UUID": "44556677-8899-aabb-ccdd-eeffaabbccdd",
    "BLE_DATA_STATUS_CHAR_UUID": "55667788-99aa-bbcc-ddee-ffaabbccddee",
    "BLE_DEBUG_SERVICE_UUID": "6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
    "BLE_DEBUG_RX_CHAR_UUID": "6E400002-B5A3-F393-E0A9-E50E24DCCA9E",
    "BLE_DEBUG_TX_CHAR_UUID": "6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
    "BLE_SYSINFO_SERVICE_UUID": "77889900-aabb-ccdd-eeff-112233445566",
    "BLE_SYSINFO_SYSTEM_CHAR_UUID": "88990011-bbcc-ddee-ff11-223344556677",
    "BLE_SYSINFO_PERFORMANCE_CHAR_UUID": "99001122-ccdd-eeff-1122-334455667788",
    "BLE_SYSINFO_HARDWARE_CHAR_UUID": "00112233-ddee-ff11-2233-445566778899",
    "BLE_SYSINFO_SESSIONS_CHAR_UUID": "11223344-eeff-1122-3344-556677889900",
    "BLE_SYSINFO_DIAGNOSTICS_CHAR_UUID": "22334455-ff00-1111-2222-334455667788",
}

# sha256 prefix (16 hex) of each handler body, line endings normalised.
LEGACY_HANDLER_HASHES = {
    "handle_ota_control_command": "f6ab62cd9c3c1dc0",
    "handle_ota_data_chunk": "02a1acf5901f03de",
    "handle_debug_command": "5df1b2d2dcb5300a",
    "handle_set_board_command": "2b0d99d66a1d5d72",
    "handle_wifi_command": "90ca6e2b72351156",
    "handle_data_control_command": "b5fbdd190c7a92a5",
    "stop_data_export": "5768a8ab1685de9c",
    "send_next_data_chunk": "3f0176ccdc280be8",
    "set_ota_status": "5038b624d24ca0a5",
    "set_data_status": "d8f8622129d2c779",
    "update_system_info": "bc2e4d43fbceff3c",
    "generate_diagnostic_report": "e9f026e362f07b29",
}


def _body(name):
    m = re.search(r"^\w[\w:<>\* ]* BluetoothManager::" + name + r"\(.*?^}\n", MANAGER, re.S | re.M)
    assert m, f"handler {name} not found"
    return m.group(0)


def test_legacy_uuids_unchanged():
    for macro, value in LEGACY_UUIDS.items():
        assert re.search(r'#define\s+' + macro + r'\s+"' + re.escape(value) + '"', CONFIG), macro


def test_glink_uuids_distinct_from_legacy():
    legacy = {v.lower() for v in LEGACY_UUIDS.values()}
    for macro in ("GLINK_SERVICE_UUID", "GLINK_RX_CHAR_UUID", "GLINK_TX_CHAR_UUID"):
        m = re.search(r'#define\s+' + macro + r'\s+"([^"]+)"', CONFIG)
        assert m and m.group(1).lower() not in legacy


def test_legacy_handlers_unchanged():
    for name, expected in LEGACY_HANDLER_HASHES.items():
        got = hashlib.sha256(_body(name).encode("utf-8")).hexdigest()[:16]
        assert got == expected, f"{name} body changed ({got})"
