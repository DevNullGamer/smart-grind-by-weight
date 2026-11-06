#pragma once

//==============================================================================
// BLUETOOTH LOW ENERGY CONFIGURATION
//==============================================================================
// This file contains all Bluetooth Low Energy (BLE) related configuration
// constants for the ESP32 coffee grinder. This includes service definitions,
// timing parameters, and communication settings for OTA updates and data export.

//------------------------------------------------------------------------------
// BLE OTA (Over-The-Air Update) SERVICE
//------------------------------------------------------------------------------
#define BLE_OTA_SERVICE_UUID "12345678-1234-1234-1234-123456789abc"            // Main OTA service UUID
#define BLE_OTA_DATA_CHAR_UUID "87654321-4321-4321-4321-cba987654321"          // Characteristic for firmware data transfer
#define BLE_OTA_CONTROL_CHAR_UUID "11111111-2222-3333-4444-555555555555"      // Characteristic for OTA commands (start/end/abort)
#define BLE_OTA_STATUS_CHAR_UUID "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"       // Characteristic for OTA status notifications
#define BLE_OTA_BUILD_NUMBER_CHAR_UUID "66666666-7777-8888-9999-000000000000" // Characteristic for current firmware build number
#define BLE_OTA_INACTIVITY_TIMEOUT_MS 15000                                    // Abort a started OTA when no data chunk arrives for this long

// Device Settings
#define BLE_DEVICE_NAME "GrindByWeight"                                       // Bluetooth device name (GATT + advertising)
#define BLE_OTA_DEVICE_NAME BLE_DEVICE_NAME                                    // Bluetooth device name when in OTA mode

//------------------------------------------------------------------------------
// BLE DATA EXPORT SERVICE
//------------------------------------------------------------------------------
#define BLE_DATA_SERVICE_UUID "22334455-6677-8899-aabb-ccddeeffffaa"          // Measurement data service UUID
#define BLE_DATA_CONTROL_CHAR_UUID "33445566-7788-99aa-bbcc-ddeeffaabbcc"     // Control characteristic (start/stop data export)
#define BLE_DATA_TRANSFER_CHAR_UUID "44556677-8899-aabb-ccdd-eeffaabbccdd"    // Data transfer characteristic
#define BLE_DATA_STATUS_CHAR_UUID "55667788-99aa-bbcc-ddee-ffaabbccddee"      // Status notifications characteristic
#define BLE_DATA_CHUNK_SIZE_BYTES 512                                          // Per-chunk payload size for data export

//------------------------------------------------------------------------------
// BLE DEBUG SERVICE (Nordic UART Service)
//------------------------------------------------------------------------------
#define BLE_DEBUG_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"         // Nordic UART Service UUID
#define BLE_DEBUG_RX_CHAR_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"         // ESP32 RX (Client -> Device)
#define BLE_DEBUG_TX_CHAR_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"         // ESP32 TX (Device -> Client)
#define BLE_DEBUG_MAX_CHUNK_BYTES 180                                          // Cap debug payloads to reduce BLE heap pressure
#define BLE_DEBUG_CHUNK_DELAY_MS 25                                            // Delay between debug notifications for flow control

//------------------------------------------------------------------------------
// BLE SYSTEM INFO SERVICE
//------------------------------------------------------------------------------
#define BLE_SYSINFO_SERVICE_UUID "77889900-aabb-ccdd-eeff-112233445566"        // System information service UUID
#define BLE_SYSINFO_SYSTEM_CHAR_UUID "88990011-bbcc-ddee-ff11-223344556677"    // System info (version, uptime, memory)
#define BLE_SYSINFO_PERFORMANCE_CHAR_UUID "99001122-ccdd-eeff-1122-334455667788" // Performance metrics
#define BLE_SYSINFO_HARDWARE_CHAR_UUID "00112233-ddee-ff11-2233-445566778899"  // Hardware status
#define BLE_SYSINFO_SESSIONS_CHAR_UUID "11223344-eeff-1122-3344-556677889900"  // Session statistics
#define BLE_SYSINFO_DIAGNOSTICS_CHAR_UUID "22334455-ff00-1111-2222-334455667788"  // Comprehensive diagnostic report trigger

#define BLE_SYSINFO_MAX_PAYLOAD_BYTES 512                                       // Maximum payload size for system info

//------------------------------------------------------------------------------
// GLINK (Grinder Link v1) SERVICE: constants only, frozen by docs/GLINK-PROTOCOL.md
//------------------------------------------------------------------------------
// Scheme (analogue of the Bookoo Motto80 UUIDs): 53474257 = ASCII "SGBW", tail 4752494e4421 = ASCII "GRIND!".
// The two middle groups (4632, 901b) were drawn at random, in the shape of a version-4 UUID with the RFC variant bits.
// Collision check (step 1 of chg020): none of the UUIDs above equals one of these, and none shares the 53474257 prefix.
// tools/tests/test_glink_contract.py keeps this block, the spec tables and tools/tests/glink_vectors.json equal.
#define GLINK_SERVICE_UUID "53474257-0001-4632-901b-4752494e4421"              // Primary service
#define GLINK_RX_CHAR_UUID "53474257-0002-4632-901b-4752494e4421"              // RX (client -> device): Write, Write Without Response
#define GLINK_TX_CHAR_UUID "53474257-0003-4632-901b-4752494e4421"              // TX (device -> client): Notify

#define GLINK_PROTO_VERSION 1                                                  // "proto" in baseInfo
#define GLINK_FRAME_MAGIC 0xA5                                                 // Fragment frame v1: first byte
#define GLINK_FRAME_VERSION 0x01                                               // Fragment frame v1: second byte
#define GLINK_FRAME_HEADER_BYTES 8                                             // magic, version, sequence, totalLength, offset
#define GLINK_DEFAULT_PACKET_BYTES 20                                          // Packet size when the ATT MTU is unknown (MTU 23)
#define GLINK_MAX_PACKET_BYTES 512                                             // Packet size is at most min(ATT MTU - 3, this)
#define GLINK_MAX_MESSAGE_BYTES 4096                                           // totalLength above this is a bad frame (Motto80 limit)
#define GLINK_MAX_REQUEST_BYTES 512                                            // Larger requests: PAYLOAD_TOO_LARGE
#define GLINK_MAX_REPLY_BYTES 1024                                             // Largest response or broadcast
#define GLINK_REASSEMBLY_TIMEOUT_MS 2000                                       // Gap between fragments after which a message is dropped
#define GLINK_HANDSHAKE_TIMEOUT_MS 10000                                       // No appHello within this long after connect: disconnect
#define GLINK_BAD_FRAME_LIMIT 10                                               // Bad fragments within the window that disconnect a client
#define GLINK_BAD_FRAME_WINDOW_MS 10000                                        // Window for GLINK_BAD_FRAME_LIMIT
#define GLINK_JSON_MAX_DEPTH 4                                                 // Object nesting in a request (the top-level object is depth 1)
#define GLINK_JSON_KEY_MAX_LEN 32                                              // Longest key (charset A-Za-z0-9_)
#define GLINK_JSON_STRING_MAX_LEN 32                                           // Longest string value, in bytes
#define GLINK_JSON_NUMBER_MAX_LEN 15                                           // Longest number token, in characters
#define GLINK_PROFILE_COUNT_MAX 4                                              // Profile indices 0..3
#define GLINK_PROFILE_WRITABLE_INDEX 3                                         // The only profile writable over BLE (Profile Dose)
#define GLINK_DOSE_MIN_G 5.0f                                                  // Dose range for writes and BLE starts
#define GLINK_DOSE_MAX_G 60.0f
#define GLINK_GRIND_SIZE_MAX 999.9f                                            // grindSize range is 0.0 .. this (display only)
#define GLINK_GRIND_UNIT_MAX_LEN 6                                             // grindUnit length after trimming
#define GLINK_START_LEAD_IN_MS 3000                                            // Touch-to-cancel lead-in of every BLE start
#define GLINK_WRITE_MIN_INTERVAL_MS 1000                                       // Minimum time between two stored changes (RATE_LIMITED)
#define GLINK_UI_REPLY_TIMEOUT_MS 1000                                         // UI task must answer within this, else UI_BUSY
#define GLINK_PERIOD_HEARTBEAT_MS 2000                                         // periodInfo when nothing changed
#define GLINK_PERIOD_GRINDING_FAST_MS 250                                      // periodInfo while GRINDING, ATT MTU >= GLINK_FAST_MTU_MIN
#define GLINK_PERIOD_GRINDING_SLOW_MS 1000                                     // periodInfo while GRINDING, ATT MTU below that
#define GLINK_FAST_MTU_MIN 100                                                 // ATT MTU from which the fast grinding cadence is used

// Device-side GLink knobs that are not part of the frozen protocol tables (docs/GLINK-PROTOCOL.md section 8 states the
// pacing; the BLE_ prefix keeps them out of the GLINK_ contract check in tools/tests/test_glink_contract.py).
#define BLE_GLINK_TX_PACE_MS 20                                                // Pacing window of notifications (the Bluetooth task ticks every 20 ms)
#define BLE_GLINK_TX_FRAGMENTS_PER_PACE 2                                      // Fragments sent per pacing window
#define BLE_GLINK_BASE_INFO_SN_HEX_LEN 12                                      // snCode: 12 lower-case hex digits of the factory MAC
#define BLE_GLINK_SNAPSHOT_PUBLISH_MS 100                                      // The UI task republishes the GLink snapshot at least this often while a client is linked
#define BLE_GLINK_SNAPSHOT_STALE_MS 1000                                       // A snapshot older than this is not trusted (reads and periodInfo fall back, requests get UI_BUSY)
#define BLE_GLINK_UI_RUN_TIMEOUT_MS 2000                                       // A request the UI task took but did not finish within this long is answered UI_BUSY

//------------------------------------------------------------------------------
// BLE TIMEOUT SETTINGS
//------------------------------------------------------------------------------
#define BLE_AUTO_DISABLE_TIMEOUT_MS (30 * 60 * 1000)                          // Auto-disable BLE after inactivity
#define BLE_BOOTUP_AUTO_DISABLE_TIMEOUT_MS (5 * 60 * 1000)                    // Auto-disable BLE after bootup period


//------------------------------------------------------------------------------
// BLE INITIALIZATION TIMING
//------------------------------------------------------------------------------
// Delays for stable BLE stack initialization
#define BLE_INIT_STACK_DELAY_MS 100                                            // Delay after BLE stack initialization
#define BLE_INIT_SERVER_DELAY_MS 50                                            // Delay after server creation
#define BLE_INIT_SERVICE_DELAY_MS 25                                           // Delay after service creation
#define BLE_INIT_CHARACTERISTIC_DELAY_MS 25                                    // Delay after each characteristic setup
#define BLE_INIT_START_DELAY_MS 50                                             // Delay after service start
#define BLE_INIT_ADVERTISING_DELAY_MS 25                                       // Delay after advertising setup

//------------------------------------------------------------------------------
// BLE SHUTDOWN TIMING
//------------------------------------------------------------------------------
#define BLE_SHUTDOWN_ADVERTISING_DELAY_MS 50                                   // Delay to allow advertising to stop cleanly
#define BLE_SHUTDOWN_DEINIT_DELAY_MS 100                                       // Delay to allow BLE stack to deinitialize

//------------------------------------------------------------------------------
// BLE POWER MANAGEMENT
//------------------------------------------------------------------------------
#define BLE_NORMAL_CPU_FREQ_MHZ 240                                            // Normal CPU frequency during BLE operations
#define BLE_REDUCED_CPU_FREQ_MHZ 240                                           // Reduced CPU frequency for BLE mode
