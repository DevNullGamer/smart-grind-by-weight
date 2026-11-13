#include "manager.h"
#include <algorithm>
#include <cstdarg>
#include <Arduino.h>
#include <esp_system.h>
#include <LittleFS.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <mbedtls/platform_util.h>
#include "../system/performance_monitor.h"
#include "../system/statistics_manager.h"
#include "../system/diagnostics_controller.h"
#include "../system/nvs_settings_store.h"
#include "../system/update_guard.h"
#include "../config/constants.h"
#include "../config/user.h"
#include "../config/grind_control.h"
#include "../config/build_info.h"
#include "../logging/grind_logging.h"
#include "../hardware/hardware_manager.h"
#include "../hardware/board_detect.h"
#include "../network/wifi_manager.h"
#include "../hardware/WeightSensor.h"
#include "../controllers/grind_controller.h"
#include "../network/portal_stop.h"
#include "glink_decide.h"
#include "glink_gate.h"

BluetoothManager::BluetoothManager()
    : ble_server(nullptr)
    , ota_service(nullptr)
    , data_service(nullptr)
    , debug_service(nullptr)
    , sysinfo_service(nullptr)
    , glink_service_(nullptr)
    , ota_data_characteristic(nullptr)
    , ota_control_characteristic(nullptr)
    , ota_status_characteristic(nullptr)
    , build_number_characteristic(nullptr)
    , data_control_characteristic(nullptr)
    , data_transfer_characteristic(nullptr)
    , data_status_characteristic(nullptr)
    , debug_rx_characteristic(nullptr)
    , debug_tx_characteristic(nullptr)
    , sysinfo_system_characteristic(nullptr)
    , sysinfo_performance_characteristic(nullptr)
    , sysinfo_hardware_characteristic(nullptr)
    , sysinfo_sessions_characteristic(nullptr)
    , sysinfo_diagnostics_characteristic(nullptr)
    , glink_rx_characteristic(nullptr)
    , glink_tx_characteristic(nullptr)
    , device_connected(false)
    , ble_enabled(false)
    , full_enabled(false)
    , glink_link_wanted(false)
    , stack_initialized(false)
    , connection_id(0)
    , debug_stream_active(false)
    , enable_time(0)
    , timeout_ms(BLE_AUTO_DISABLE_TIMEOUT_MS)
    , last_disconnect_time(0)
    , data_export_in_progress(false)
    , data_status(BLE_DATA_IDLE)
    , current_chunk(0)
    , next_chunk_time(0)
    , current_file_session_id(0)
    , sessions_info_dirty(true)
    , last_session_storage_version(0)
    , last_reported_export_state(false)
    , ui_status_queue(nullptr)
    , diagnostic_report_pending(false)
    , diagnostic_report_in_progress(false)
    , link_epoch(0)
    , link_connect_ms(0)
    , att_mtu(0)
    , tx_subscribed(false)
    , rx_activity(false)
    , tx_failed(false)
    , glink_rx_mux(portMUX_INITIALIZER_UNLOCKED)
    , glink_tx(glink_tx_buffer, sizeof(glink_tx_buffer))
    , glink_tx_sequence(0)
    , glink_epoch_seen(0)
    , glink_disconnect_sent(false)
    , last_sysinfo_update_ms(0) {
}

BluetoothManager::~BluetoothManager() {
    disable();
}

void BluetoothManager::init(Preferences* prefs) {
    log("Bluetooth: Manager initialized (enable via Developer Mode)\n");
    ota_handler.init(prefs);
    // Create UI status queue to marshal UI updates to UI task
    if (!ui_status_queue) {
        ui_status_queue = xQueueCreate(8, sizeof(UIStatusMessage));
    }
}

void BluetoothManager::set_ui_status_callback(UIStatusCallback callback) {
    ui_status_callback = callback;
}

void BluetoothManager::update_ui_status(const char* status) {
    // Don't call into UI from BLE task; enqueue for UI task to process
    enqueue_ui_status(status);
}

void BluetoothManager::enqueue_ui_status(const char* status) {
    if (!ui_status_queue) return;
    UIStatusMessage msg;
    msg.text[0] = '\0';
    if (status) {
        strncpy(msg.text, status, sizeof(msg.text) - 1);
        msg.text[sizeof(msg.text) - 1] = '\0';
    }
    // Non-blocking send; drop if full to avoid blocking BLE task
    xQueueSend(ui_status_queue, &msg, 0);
}

bool BluetoothManager::dequeue_ui_status(char* out, size_t out_len) {
    if (!ui_status_queue || !out || out_len == 0) return false;
    UIStatusMessage msg;
    if (xQueueReceive(ui_status_queue, &msg, 0) == pdPASS) {
        strncpy(out, msg.text, out_len - 1);
        out[out_len - 1] = '\0';
        return true;
    }
    return false;
}

// Creates the BLE stack, server, services and advertising data (once per boot; disable() keeps them). Leaves the
// mode flags to the caller.
void BluetoothManager::create_stack() {
    // First start since boot: initialize BLE with delays for power stability
    BLEDevice::init(BLE_DEVICE_NAME);
    
    // Request a larger MTU to improve throughput when the client supports it.
    // Some platforms (e.g., macOS/iOS) may ignore this request and keep a lower MTU.
    // That's fine — we also keep chunk sizes small and paced below.
    BLEDevice::setMTU(517);
    delay(BLE_INIT_STACK_DELAY_MS);
    
    ble_server = BLEDevice::createServer();
    delay(BLE_INIT_SERVER_DELAY_MS);
    ble_server->setCallbacks(this);
    
    // Create OTA service
    ota_service = ble_server->createService(BLE_OTA_SERVICE_UUID);
    delay(BLE_INIT_SERVICE_DELAY_MS);
    
    ota_data_characteristic = ota_service->createCharacteristic(
        BLE_OTA_DATA_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    ota_data_characteristic->setCallbacks(this);
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);
    
    ota_control_characteristic = ota_service->createCharacteristic(
        BLE_OTA_CONTROL_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    ota_control_characteristic->setCallbacks(this);
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);
    
    ota_status_characteristic = ota_service->createCharacteristic(
        BLE_OTA_STATUS_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);
    
    build_number_characteristic = ota_service->createCharacteristic(
        BLE_OTA_BUILD_NUMBER_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ
    );
    build_number_characteristic->setValue(ota_handler.get_build_number().c_str());
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);
    
    // Create measurement data service
    data_service = ble_server->createService(BLE_DATA_SERVICE_UUID);
    delay(BLE_INIT_SERVICE_DELAY_MS);
    
    data_control_characteristic = data_service->createCharacteristic(
        BLE_DATA_CONTROL_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    data_control_characteristic->setCallbacks(this);
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);
    
    data_transfer_characteristic = data_service->createCharacteristic(
        BLE_DATA_TRANSFER_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);
    
    data_status_characteristic = data_service->createCharacteristic(
        BLE_DATA_STATUS_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);
    
    // Create debug service (Nordic UART)
    debug_service = ble_server->createService(BLE_DEBUG_SERVICE_UUID);
    delay(BLE_INIT_SERVICE_DELAY_MS);

    debug_rx_characteristic = debug_service->createCharacteristic(
        BLE_DEBUG_RX_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    debug_rx_characteristic->setCallbacks(this);
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);

    debug_tx_characteristic = debug_service->createCharacteristic(
        BLE_DEBUG_TX_CHAR_UUID,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);
    
    // Create system info service
    sysinfo_service = ble_server->createService(BLE_SYSINFO_SERVICE_UUID);
    delay(BLE_INIT_SERVICE_DELAY_MS);
    
    sysinfo_system_characteristic = sysinfo_service->createCharacteristic(
        BLE_SYSINFO_SYSTEM_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);
    
    sysinfo_performance_characteristic = sysinfo_service->createCharacteristic(
        BLE_SYSINFO_PERFORMANCE_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);
    
    sysinfo_hardware_characteristic = sysinfo_service->createCharacteristic(
        BLE_SYSINFO_HARDWARE_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);
    
    sysinfo_sessions_characteristic = sysinfo_service->createCharacteristic(
        BLE_SYSINFO_SESSIONS_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);

    sysinfo_diagnostics_characteristic = sysinfo_service->createCharacteristic(
        BLE_SYSINFO_DIAGNOSTICS_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    sysinfo_diagnostics_characteristic->setCallbacks(this);
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);

    // GLink service (chg020), created last so the attribute handles of the existing services stay as they were.
    // Its UUID is not advertised (docs/GLINK-PROTOCOL.md section 2): clients find the device by name.
    glink_service_ = ble_server->createService(GLINK_SERVICE_UUID);
    delay(BLE_INIT_SERVICE_DELAY_MS);

    glink_rx_characteristic = glink_service_->createCharacteristic(
        GLINK_RX_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    glink_rx_characteristic->setCallbacks(this);
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);

    // NimBLE adds the CCCD (0x2902) of a notify characteristic itself; onSubscribe() reports it
    glink_tx_characteristic = glink_service_->createCharacteristic(
        GLINK_TX_CHAR_UUID,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    glink_tx_characteristic->setCallbacks(this);
    delay(BLE_INIT_CHARACTERISTIC_DELAY_MS);

    ota_service->start();
    delay(BLE_INIT_START_DELAY_MS);
    
    data_service->start();
    delay(BLE_INIT_START_DELAY_MS);
    
    debug_service->start();
    delay(BLE_INIT_START_DELAY_MS);
    
    sysinfo_service->start();
    delay(BLE_INIT_START_DELAY_MS);

    glink_service_->start();
    delay(BLE_INIT_START_DELAY_MS);
    
    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_OTA_SERVICE_UUID);
    advertising->addServiceUUID(BLE_DEBUG_SERVICE_UUID);
    advertising->addServiceUUID(BLE_DATA_SERVICE_UUID);
    advertising->addServiceUUID(BLE_SYSINFO_SERVICE_UUID);
    advertising->setScanResponse(true);
    advertising->setMinPreferred(0x06);
    advertising->setMinPreferred(0x12);
    
    // Set advertised name in both advertising data and scan response data
    advertising->setName(BLE_DEVICE_NAME);
    BLEAdvertisementData adv;
    adv.setName(BLE_DEVICE_NAME);
    advertising->setAdvertisementData(adv);
    BLEAdvertisementData sr;
    sr.setName(BLE_DEVICE_NAME);
    advertising->setScanResponseData(sr);
    
    delay(BLE_INIT_ADVERTISING_DELAY_MS);
}

void BluetoothManager::enable(unsigned long timeout_ms) {
    if (full_enabled) return;
    
    // Use default timeout if none specified
    if (timeout_ms == 0) {
        timeout_ms = BLE_AUTO_DISABLE_TIMEOUT_MS;
    }
    this->timeout_ms = timeout_ms;
    unsigned long timeout_minutes = timeout_ms / 60000;
    log("Bluetooth: Enabling BLE with reduced power settings (%lum timeout)\n", timeout_minutes);
    
    // Enable reduced power mode for BLE
    ota_handler.enable_ble_power_mode();
    enable_time = millis();
    last_disconnect_time = enable_time; // Start disconnected timeout from enable time
    
    if (stack_initialized) {
        // Soft re-enable: the BLE stack, server, services and advertising data from the first start are
        // still in place (disable() never deinitialises, see disable()), so only advertising restarts.
        // From LINK_ONLY the stack is already advertising (or a client is connected): nothing to restart.
        const bool was_serving = ble_enabled;
        ble_enabled = true;
        full_enabled = true;
        set_ota_status(BLE_OTA_READY);
        set_data_status(BLE_DATA_IDLE);
        if (build_number_characteristic) {
            // LINK_ONLY blanks the old read-only values; sysinfo follows via the refresh below
            build_number_characteristic->setValue(ota_handler.get_build_number().c_str());
        }
        if (was_serving) {
            last_sysinfo_update_ms = 0;  // LINK_ONLY blanked the old values: refresh at once (a cold enable keeps the old timing)
        }
        refresh_system_info();
        sessions_info_dirty = true;
        if (!was_serving) {
            start_advertising();
        }
        log("Bluetooth: Ready - device is advertising again (%lum timeout)\n", timeout_minutes);
        return;
    }

    create_stack();
    
    ble_enabled = true;
    full_enabled = true;
    stack_initialized = true;
    set_ota_status(BLE_OTA_READY);
    
    // Initialize system information
    refresh_system_info();
    sessions_info_dirty = true;
    
    start_advertising();
    log("Bluetooth: Ready - device is advertising (%lum timeout)\n", timeout_minutes);
}

void BluetoothManager::enable_during_bootup() {
    glink_link_wanted = ui_settings.glink_link();
    if (ui_settings.ble_startup()) {
        enable(BLE_BOOTUP_AUTO_DISABLE_TIMEOUT_MS);
    } else if (glink_link_wanted) {
        start_link_only();
    }
}

// LINK_ONLY: the stack serves GLink only and has no auto-disable timer. The stack (about 79 KB) is resident from
// here on, as after any first start.
void BluetoothManager::start_link_only() {
    if (ble_enabled) return;
    log("Bluetooth: GaggiMate link on - advertising for GLink only, no auto-disable\n");

    ota_handler.enable_ble_power_mode();
    if (!stack_initialized) {
        create_stack();
        ble_enabled = true;
        stack_initialized = true;
    } else {
        ble_enabled = true;
    }
    start_advertising();
}

void BluetoothManager::set_glink_link(bool on) {
    glink_link_wanted = on;
    if (full_enabled) {
        return;  // FULL serves GLink as well; the setting decides what happens when FULL ends
    }
    if (on) {
        start_link_only();
    } else if (ble_enabled) {
        log("Bluetooth: GaggiMate link off\n");
        shut_down_active();
    }
}

// Soft disable: BLEDevice::deinit()/init() leaks ~11 KB of internal heap per cycle, so the stack stays
// initialised (~79 KB resident) and disable() only stops all BLE activity. enable() reuses the server,
// services and characteristics. DEBUG_BLE_HARD_DEINIT=1 restores the old full teardown for measurements.
void BluetoothManager::disable() {
    if (!full_enabled) return;
    full_enabled = false;
    shut_down_active();

    // With the GaggiMate link on, the stack keeps advertising for GLink once Bluetooth Enabled ends
    if (glink_link_wanted.load()) {
        start_link_only();
    }
}

// Stops every BLE activity (any mode): drops the client, stops advertising. The stack stays resident.
void BluetoothManager::shut_down_active() {
    if (!ble_enabled) return;
    
    log("Bluetooth: Disabling BLE and restoring normal power...\n");
    
    if (ota_handler.is_ota_active()) {
        ota_handler.abort_ota();
    }
    
    if (data_export_in_progress) {
        stop_data_export();
    }
    
    // Cleared first so onDisconnect() does not restart advertising and handle() goes idle
    ble_enabled = false;
    diagnostic_report_pending = false;

    // Stop advertising now so no new client connects while the existing one is being dropped
    if (stack_initialized) {
        BLEDevice::stopAdvertising();
    }

    // Drop the connected client and wait briefly for the disconnect event
    if (ble_server) {
        if (device_connected) {
            ble_server->disconnect(connection_id);
        }
        for (int i = 0; i < 20 && ble_server->getConnectedCount() > 0; i++) {
            delay(50);
        }
    }

    BLEDevice::stopAdvertising();
    delay(BLE_SHUTDOWN_ADVERTISING_DELAY_MS);
    
#if DEBUG_BLE_HARD_DEINIT
    log("Bluetooth: Deinitializing BLE stack...\n");
    BLEDevice::deinit(false);
    delay(BLE_SHUTDOWN_DEINIT_DELAY_MS);
    stack_initialized = false;
    ble_server = nullptr;
    ota_service = nullptr;
    data_service = nullptr;
    debug_service = nullptr;
    sysinfo_service = nullptr;
    glink_service_ = nullptr;
    glink_rx_characteristic = nullptr;
    glink_tx_characteristic = nullptr;
    ota_data_characteristic = nullptr;
    ota_control_characteristic = nullptr;
    ota_status_characteristic = nullptr;
    build_number_characteristic = nullptr;
    data_control_characteristic = nullptr;
    data_transfer_characteristic = nullptr;
    data_status_characteristic = nullptr;
    debug_rx_characteristic = nullptr;
    debug_tx_characteristic = nullptr;
    sysinfo_system_characteristic = nullptr;
    sysinfo_performance_characteristic = nullptr;
    sysinfo_hardware_characteristic = nullptr;
    sysinfo_sessions_characteristic = nullptr;
    sysinfo_diagnostics_characteristic = nullptr;
#endif

    // Stays true if the client did not disconnect in time; onDisconnect() clears it later
    device_connected = ble_server && ble_server->getConnectedCount() > 0;
    debug_stream_active = false;
    
    // Restore normal power settings
    ota_handler.restore_normal_power_mode();
    log("Bluetooth: Disable complete\n");
}

void BluetoothManager::handle() {
    if (!ble_enabled) {
        // Soft-disabled: the stack is still up, so make sure nothing (host reset resync, failed connect)
        // has restarted advertising
        if (stack_initialized && BLEDevice::getAdvertising()->isAdvertising()) {
            BLEDevice::stopAdvertising();
        }
        return;
    }

    // Everything up to the GLink tick is FULL only: the old services and the auto-disable timers.
    // LINK_ONLY serves GLink alone and has no timer.
    if (full_enabled) {
        // Only check timeout when no client is connected
        if (!device_connected) {
            unsigned long disconnected_elapsed = millis() - last_disconnect_time;

            if (disconnected_elapsed > timeout_ms) {
                log("Bluetooth: Timeout reached (%lu minutes disconnected), disabling BLE\n", timeout_ms / 60000);
                disable();
                return;
            }
        } else {
            // While connected, constantly reset timeout to default for UI display
            timeout_ms = BLE_AUTO_DISABLE_TIMEOUT_MS;
        }

        // A stalled OTA would keep the hardware tasks suspended: abort it
        if (ota_handler.check_inactivity_timeout()) {
            if (ota_handler.get_failure_reason()) {
                update_ui_status(ota_handler.get_failure_reason());
            }
            set_ota_status(BLE_OTA_ERROR);
        }

        // Handle data export updates
        update_data_export();
        process_sessions_info_updates();

        // Run deferred diagnostic report generation on BLE task (not on NimBLE callback thread)
        if (device_connected && debug_tx_characteristic && diagnostic_report_pending && !diagnostic_report_in_progress) {
            diagnostic_report_in_progress = true;
            diagnostic_report_pending = false;
            generate_diagnostic_report();
            diagnostic_report_in_progress = false;
        }

        // Update system info periodically if connected (every 10 seconds)
        if (device_connected && millis() - last_sysinfo_update_ms > 10000) {
            refresh_system_info();
            last_sysinfo_update_ms = millis();
        }
    }

    glink_service_tick(millis());
}

void BluetoothManager::start_advertising() {
    if (ble_enabled && ble_server) {
        BLEDevice::startAdvertising();
    }
}

void BluetoothManager::stop_advertising() {
    if (ble_enabled) {
        BLEDevice::stopAdvertising();
    }
}

unsigned long BluetoothManager::get_remaining_time_ms() const {
    if (!ota_handler.is_ota_active()) return 0;
    
    unsigned long elapsed = millis() - enable_time;
    float progress = ota_handler.get_progress();
    if (progress <= 0) return 0;
    
    unsigned long total_estimated = elapsed * 100.0f / progress;
    return (total_estimated > elapsed) ? (total_estimated - elapsed) : 0;
}

unsigned long BluetoothManager::get_bluetooth_timeout_remaining_ms() const {
    if (!full_enabled) return 0;  // LINK_ONLY has no timer
    
    if (device_connected) {
        // When connected, use the full timeout from connection time
        unsigned long elapsed = millis() - enable_time;
        return (elapsed < timeout_ms) ? (timeout_ms - elapsed) : 0;
    } else {
        // When disconnected, use time since last disconnect
        unsigned long disconnected_elapsed = millis() - last_disconnect_time;
        return (disconnected_elapsed < timeout_ms) ? (timeout_ms - disconnected_elapsed) : 0;
    }
}


void BluetoothManager::start_data_export() {
    if (!full_enabled || !device_connected) {
        log("Bluetooth Data: Cannot start export - BLE not enabled or not connected\n");
        return;
    }

    if (UpdateGuard::is_held()) {
        log("Bluetooth Data: Cannot start export - a WiFi update is running\n");
        return;
    }

    if (data_export_in_progress) {
        log("Bluetooth Data: Export already in progress\n");
        return;
    }
    
    log("Bluetooth Data: Starting data export - sending file list\n");
    send_file_list();
}

void BluetoothManager::stop_data_export() {
    if (!data_export_in_progress) { // No log needed if nothing is happening
        return;
    }
    
    log("Bluetooth Data: Stopping export\n");
    data_export_in_progress = false;
    current_chunk = 0;
    next_chunk_time = 0;
    current_file_session_id = 0;
    mark_sessions_info_dirty();
    
    // Clean shutdown of stream
    data_stream.close_stream();
    
    set_data_status(BLE_DATA_IDLE);
}

void BluetoothManager::update_data_export() {
    // Check if we need to send the next chunk
    if (data_export_in_progress && millis() >= next_chunk_time) {
        send_next_data_chunk();
    }
}

void BluetoothManager::send_next_data_chunk() {
    if (!data_export_in_progress || !data_transfer_characteristic) {
        return;
    }

    // If client dropped mid-transfer, stop cleanly and avoid further notify attempts
    if (!device_connected) {
        stop_data_export();
        set_data_status(BLE_DATA_ERROR);
        return;
    }
    
    uint8_t buffer[BLE_DATA_CHUNK_SIZE_BYTES];
    size_t actual_size = 0;
    
    // Per-file streaming only
    if (current_file_session_id == 0) {
        log("Bluetooth Data: No file session active for chunk request\n");
        stop_data_export();
        set_data_status(BLE_DATA_ERROR);
        return;
    }
    
    bool has_data = data_stream.read_file_chunk(buffer, sizeof(buffer), &actual_size);
    
    if (has_data && actual_size > 0) {
        // Send the data chunk
        data_transfer_characteristic->setValue(buffer, actual_size);
        data_transfer_characteristic->notify();
        
        current_chunk++;
        
        // Chunk sent (reduced logging)
        
        // Update progress
        uint8_t progress = data_stream.get_progress_percent();
        uint8_t status_data[2] = { (uint8_t)BLE_DATA_EXPORTING, progress };
        if (data_status_characteristic) {
            data_status_characteristic->setValue(status_data, 2);
            data_status_characteristic->notify();
        }
        
        // Pace notifications to avoid overflowing BLE buffers/OS queues.
        // Combined with smaller chunk size this greatly reduces blocking in notify().
        next_chunk_time = millis() + 25; // ~6.4 KB/s at 160 B per 25 ms
    }
    
    // Check if file transfer is complete
    if (!has_data) {
        log("Bluetooth Data: File transfer complete for session %lu - sent %d chunks.\n", current_file_session_id, current_chunk);
        data_export_in_progress = false;
        current_chunk = 0;
        current_file_session_id = 0;
        mark_sessions_info_dirty();
        
        data_stream.close_stream();
        
        delay(200); // Give the BLE buffer time to clear
        set_data_status(BLE_DATA_COMPLETE);
    }
}

float BluetoothManager::get_data_export_progress() const {
    if (!data_export_in_progress) return 0.0f;
    return static_cast<float>(data_stream.get_progress_percent());
}

uint32_t BluetoothManager::get_data_export_session_count() const {
    return data_stream.get_total_sessions();
}

void BluetoothManager::send_measurement_count() {
    uint16_t count = data_stream.get_total_sessions();
    
    if (data_status_characteristic) {
        // Send as 2-byte little-endian
        uint8_t count_data[2];
        count_data[0] = count & 0xFF;
        count_data[1] = (count >> 8) & 0xFF;
        data_status_characteristic->setValue(count_data, 2);
        data_status_characteristic->notify();
    }
    
    log("Bluetooth Data: Sent measurement count: %d\n", count);
}

void BluetoothManager::send_log_message(const char* message) {
    if (debug_stream_active && device_connected && debug_tx_characteristic) {
        debug_tx_characteristic->setValue((uint8_t*)message, strlen(message));
        debug_tx_characteristic->notify();
    }
}

void BluetoothManager::clear_measurement_data() {
    // Note: GrindLogger doesn't have a clear method, data will be overwritten in circular buffer
    log("Bluetooth Data: Measurement data will be overwritten by new grinds\n");
    set_data_status(BLE_DATA_IDLE);
}

void BluetoothManager::send_file_list() {
    if (!full_enabled || !device_connected || !data_transfer_characteristic) {
        log("Bluetooth Data: Cannot send file list - BLE not enabled or not connected\n");
        set_data_status(BLE_DATA_ERROR);
        return;
    }
    
    // Get list of session files
    const uint32_t max_sessions = 100;  // Limit to prevent memory issues
    uint32_t* session_ids = (uint32_t*)malloc(max_sessions * sizeof(uint32_t));
    if (!session_ids) {
        log("ERROR: Failed to allocate memory for session list\n");
        set_data_status(BLE_DATA_ERROR);
        return;
    }
    
    uint32_t session_count = data_stream.get_session_list(session_ids, max_sessions);
    
    // Send file list: [session_count:4][session_id1:4][session_id2:4]...
    size_t total_size = 4 + (session_count * 4);
    uint8_t* buffer = (uint8_t*)malloc(total_size);
    if (!buffer) {
        free(session_ids);
        log("ERROR: Failed to allocate memory for file list buffer\n");
        set_data_status(BLE_DATA_ERROR);
        return;
    }
    
    // Write session count (little-endian)
    buffer[0] = session_count & 0xFF;
    buffer[1] = (session_count >> 8) & 0xFF;
    buffer[2] = (session_count >> 16) & 0xFF;
    buffer[3] = (session_count >> 24) & 0xFF;
    
    // Write session IDs
    for (uint32_t i = 0; i < session_count; i++) {
        uint32_t offset = 4 + (i * 4);
        buffer[offset + 0] = session_ids[i] & 0xFF;
        buffer[offset + 1] = (session_ids[i] >> 8) & 0xFF;
        buffer[offset + 2] = (session_ids[i] >> 16) & 0xFF;
        buffer[offset + 3] = (session_ids[i] >> 24) & 0xFF;
    }
    
    // Send the file list
    data_transfer_characteristic->setValue(buffer, total_size);
    data_transfer_characteristic->notify();
    
    log("Bluetooth Data: Sent file list with %lu sessions\n", session_count);
    
    free(session_ids);
    free(buffer);
    
    // Give the BLE buffer time to transmit the data before sending status
    delay(100);
    
    // Mark transfer complete so client stops waiting
    set_data_status(BLE_DATA_COMPLETE);
}

void BluetoothManager::send_individual_file(uint32_t session_id) {
    if (!full_enabled || !device_connected) {
        log("Bluetooth Data: Cannot start file transfer - BLE not enabled or not connected\n");
        set_data_status(BLE_DATA_ERROR);
        return;
    }
    
    if (data_export_in_progress) {
        log("❌ FILE REQUEST ATTEMPT - Transfer already in progress! Client sent duplicate REQUEST command.\n");
        return;
    }
    
    // Initialize individual file stream
    if (!data_stream.initialize_file_stream(session_id)) {
        log("Bluetooth Data: Failed to initialize file stream for session %lu\n", session_id);
        set_data_status(BLE_DATA_ERROR);
        return;
    }
    
    log("Bluetooth Data: Starting individual file transfer for session %lu\n", session_id);
    
    data_export_in_progress = true;
    current_file_session_id = session_id;
    current_chunk = 0;
    next_chunk_time = millis(); // Start immediately
    mark_sessions_info_dirty();
    set_data_status(BLE_DATA_EXPORTING);
}

void BluetoothManager::log(const char* format, ...) {
    char buffer[512]; // Increased from 256 to 2048 bytes for large debug messages.
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Note: This is the log method itself, so we print to Serial directly
    Serial.print(buffer);

    // is_debug_stream_active() is the same as debug_stream_active
    if (debug_stream_active) {
        send_log_message(buffer);
    }
}

void BluetoothManager::set_ota_status(BLEOTAStatus status) {
    if (ota_status_characteristic) {
        uint8_t status_value = static_cast<uint8_t>(status);
        ota_status_characteristic->setValue(&status_value, 1);
        ota_status_characteristic->notify();
    }
}

void BluetoothManager::set_data_status(BLEDataStatus status) {
    data_status = status;
    if (data_status_characteristic) {
        uint8_t status_value = static_cast<uint8_t>(status);
        data_status_characteristic->setValue(&status_value, 1);
        data_status_characteristic->notify();
    }
}

void BluetoothManager::handle_ota_control_command(BLECharacteristic* characteristic) {
    String data = characteristic->getValue();
    if (data.length() == 0) return;
    
    uint8_t command = data[0];
    
    switch (command) {
        case BLE_OTA_CMD_START:
            // New protocol: [CMD][patch_size:4][is_full_update:1][build_number_length:1][build_number:N]
            if (data.length() >= 6) {  // 1 + 4 + 1 bytes minimum (cmd + patch_size + full_update_flag)
                uint32_t patch_size = *(uint32_t*)(data.c_str() + 1);
                bool is_full_update = data[5] != 0;
                
                log("Bluetooth OTA: Starting %s update (%lu KB)\n", 
                    is_full_update ? "full" : "delta", (unsigned long)patch_size / 1024);

                // Parse build number if present
                String expected_build = "";
                String expected_firmware_version = "";
                size_t offset = 7;
                
                if (data.length() > 6) {
                    uint8_t build_length = data[6];
                    if (build_length > 0 && data.length() >= offset + build_length) {
                        expected_build = String(data.c_str() + offset, build_length);
                        log("Bluetooth OTA: Expected build after update: %s\n", expected_build.c_str());
                        offset += build_length;
                    }
                    
                    // Parse firmware version if present (backwards compatible extension)
                    if (data.length() > offset) {
                        uint8_t version_length = data[offset];
                        offset++;
                        if (version_length > 0 && data.length() >= offset + version_length) {
                            expected_firmware_version = String(data.c_str() + offset, version_length);
                            log("Bluetooth OTA: Expected firmware version after update: %s\n", expected_firmware_version.c_str());
                        }
                    }
                }
                
                if (ota_handler.start_ota(patch_size, expected_build, is_full_update, expected_firmware_version)) {
                    update_ui_status("Receiving update...");
                    set_ota_status(BLE_OTA_RECEIVING);
                } else {
                    if (ota_handler.get_failure_reason()) {
                        update_ui_status(ota_handler.get_failure_reason());
                    }
                    set_ota_status(BLE_OTA_ERROR);
                }
            } else {
                log("Bluetooth OTA: ❌ Invalid start command format (need at least 6 bytes)\n");
                set_ota_status(BLE_OTA_ERROR);
            }
            break;
            
        case BLE_OTA_CMD_END:            
            log("Bluetooth OTA: Received END command\n");
            LOG_OTA_DEBUG("BLE_OTA_CMD_END received, checking if OTA active...\n");
            if (ota_handler.is_ota_active()) {
                LOG_OTA_DEBUG("OTA is active, updating UI status...\n");
                update_ui_status("Applying patch...");
                LOG_OTA_DEBUG("UI status updated, calling complete_ota()...\n");
                if (ota_handler.complete_ota()) {
                    LOG_OTA_DEBUG("complete_ota() returned SUCCESS\n");
                    set_ota_status(BLE_OTA_SUCCESS);
                    update_ui_status("Restarting...");
                } else {
                    LOG_OTA_DEBUG("complete_ota() returned FAILED\n");
                    if (ota_handler.get_failure_reason()) {
                        update_ui_status(ota_handler.get_failure_reason());
                    }
                    set_ota_status(BLE_OTA_ERROR);
                }
            } else {
                LOG_OTA_DEBUG("OTA is NOT active - ignoring END command\n");
            }
            break;
            
        case BLE_OTA_CMD_ABORT:
            ota_handler.abort_ota();
            set_ota_status(BLE_OTA_ERROR);
            break;
    }
}

void BluetoothManager::handle_ota_data_chunk(BLECharacteristic* characteristic) {
    if (!ota_handler.is_ota_active()) return;
    
    String data = characteristic->getValue();
    size_t chunk_size = data.length();
    if (chunk_size == 0) return;
    
    if (!ota_handler.process_data_chunk((const uint8_t*)data.c_str(), chunk_size)) {
        if (ota_handler.get_failure_reason()) {
            update_ui_status(ota_handler.get_failure_reason());
        }
        set_ota_status(BLE_OTA_ERROR);
    }
}

void BluetoothManager::handle_debug_command(BLECharacteristic* characteristic) {
    String value = characteristic->getValue();
    if (value.length() > 0) {
        uint8_t command = value[0];
        switch (command) {
            case BLE_DEBUG_CMD_ENABLE:
                debug_stream_active = true;
                log("BLE_DEBUG: Stream enabled\n");
#if ENABLE_GRIND_DEBUG
                // Print struct layout debug info immediately upon debug stream activation
                log("BLE_DEBUG: Printing struct layout debug info...\n");
                delay(50); // Small delay to ensure previous message is sent
                grind_logger.print_struct_layout_debug();
                log("BLE_DEBUG: Struct layout debug info complete\n");
#else
                log("BLE_DEBUG: Struct layout debug disabled (ENABLE_GRIND_DEBUG=0)\n");
#endif
                break;
            case BLE_DEBUG_CMD_DISABLE:
                log("BLE_DEBUG: Stream disabled\n");
                debug_stream_active = false;
                break;
            case BLE_DEBUG_CMD_SET_BOARD:
                handle_set_board_command(value);
                break;
            case BLE_DEBUG_CMD_WIFI_SET:
            case BLE_DEBUG_CMD_WIFI_CLEAR:
                handle_wifi_command(value);
                break;
            case 0x00: // Keepalive from python script
                break;
            default:
                // Echo command for now
                log("BLE_DEBUG: Received '%.*s'\n", (int)value.length(), value.c_str());
                break;
        }
    }
}

void BluetoothManager::handle_set_board_command(const String& value) {
    if (value.length() != 2 || static_cast<uint8_t>(value[1]) > static_cast<uint8_t>(BoardOverride::FORCE_V2)) {
        log("BLE_DEBUG: Board override rejected, expected [0x03, 0|1|2]\n");
        return;
    }
    // Auto only restores probing and cannot enable the motor, so it is always accepted.
    // Forcing V1/V2 is a recovery path for a board that was not established by probe or
    // user; once established it can only be changed on the device (Menu -> Hardware board).
    const bool established = board().source == BoardSource::PROBE || board().source == BoardSource::USER;
    if (value[1] != 0 && established) {
        log("BLE_DEBUG: Board override rejected, board is %s (%s). Use Menu -> Hardware board.\n",
            board().name(), board().source_name());
        return;
    }

    const BoardOverride requested = static_cast<BoardOverride>(static_cast<uint8_t>(value[1]));
    if (board_override_store(requested)) {
        log("BLE_DEBUG: Board override stored (%d). Restart the grinder to apply.\n", static_cast<int>(requested));
    } else {
        log("BLE_DEBUG: Board override could not be stored\n");
    }
}

void BluetoothManager::handle_wifi_command(const String& value) {
    // Credentials can be changed over BLE only for first-time setup or while the WiFi menu page is open.
    // Never log the payload: it contains the password.
    if (!wifi_manager.provisioning_allowed()) {
        log("BLE_DEBUG: WiFi command rejected, credentials already stored. Open Menu -> WiFi first.\n");
        return;
    }

    if (static_cast<uint8_t>(value[0]) == BLE_DEBUG_CMD_WIFI_CLEAR) {
        if (value.length() != 1) {
            log("BLE_DEBUG: WiFi clear rejected, expected [0x05]\n");
            return;
        }
        if (wifi_manager.clear_credentials()) {
            log("BLE_DEBUG: WiFi credentials cleared\n");
        } else {
            log("BLE_DEBUG: WiFi clear failed (storage error)\n");
        }
        return;
    }

    WifiProvisionFields fields;
    const WifiProvisionResult parsed =
        wifi_parse_provisioning(reinterpret_cast<const uint8_t*>(value.c_str()) + 1, value.length() - 1, &fields);
    if (parsed != WifiProvisionResult::OK) {
        log("BLE_DEBUG: WiFi set rejected, bad payload (%d), expected [0x04, ssid_len, ssid, pass_len, pass, token16]\n",
            static_cast<int>(parsed));
        return;
    }

    const WiFiManager::SetResult result = wifi_manager.set_credentials(fields.ssid, fields.password, fields.token);
    mbedtls_platform_zeroize(&fields, sizeof(fields));
    switch (result) {
        case WiFiManager::SetResult::OK:
            log("BLE_DEBUG: WiFi credentials stored, connecting\n");
            break;
        case WiFiManager::SetResult::INVALID:
            log("BLE_DEBUG: WiFi set rejected, invalid credentials\n");
            break;
        case WiFiManager::SetResult::STORE_FAILED:
            log("BLE_DEBUG: WiFi credentials could not be stored\n");
            break;
    }
}

void BluetoothManager::handle_data_control_command(BLECharacteristic* characteristic) {
    String data = characteristic->getValue();
    if (data.length() == 0) return;
    
    uint8_t command = data[0];
    log("Bluetooth Data: Received command 0x%02X\n", command);
    
    switch (command) {
        case BLE_DATA_CMD_STOP_EXPORT:
            log("Bluetooth Data: Stopping measurement data export\n");
            stop_data_export();
            break;
            
        case BLE_DATA_CMD_GET_COUNT:
            log("Bluetooth Data: Getting measurement count\n");
            send_measurement_count();
            break;
            
        case BLE_DATA_CMD_CLEAR_DATA:
            log("Bluetooth Data: Clearing measurement data\n");
            clear_measurement_data();
            break;
            
        case BLE_DATA_CMD_GET_FILE_LIST:
            log("Bluetooth Data: Getting file list\n");
            send_file_list();
            break;
            
        case BLE_DATA_CMD_REQUEST_FILE:
            if (data.length() >= 5) {
                uint32_t session_id = 0;
                memcpy(&session_id, data.c_str() + 1, 4);
                log("Bluetooth Data: Requesting file for session %lu\n", session_id);
                send_individual_file(session_id);
            } else {
                log("Bluetooth Data: Invalid REQUEST_FILE command length\n");
                set_data_status(BLE_DATA_ERROR);
            }
            break;
            
        default:
            log("Bluetooth Data: Unknown command: 0x%02X\n", command);
            set_data_status(BLE_DATA_ERROR);
            break;
    }
}

// BLE Callbacks
void BluetoothManager::onConnect(BLEServer* server) {
    // The NimBLE server stores the handle of the newest connection before calling back, so getConnId() names the
    // client that just connected, also when another one is still connected.
    const uint16_t new_id = server->getConnId();
    if (!ble_enabled) {
        // Connected while soft-disabled (advertising was still running): refuse
        server->disconnect(new_id);
        return;
    }
    if (UpdateGuard::is_held()) {
        // A WiFi update owns the flash and the tasks: no BLE client may interfere
        log("BLE: Client refused - a WiFi update is running\n");
        server->disconnect(new_id);
        return;
    }
    if (device_connected && new_id != connection_id) {
        // One client at a time (FULL and LINK_ONLY): the connected one keeps its state, the newcomer is dropped
        log("BLE: Second client refused - one client at a time\n");
        server->disconnect(new_id);
        return;
    }
    connection_id = new_id;
    device_connected = true;

    // GLink per-connection state starts empty (docs/GLINK-PROTOCOL.md 7.4); the Bluetooth task sees the new epoch
    portENTER_CRITICAL(&glink_rx_mux);
    glink_rx.reset();
    portEXIT_CRITICAL(&glink_rx_mux);
    tx_subscribed = false;
    tx_failed = false;
    rx_activity = false;
    att_mtu = 0;
    link_connect_ms = millis();
    link_epoch++;

    log("BLE: Client connected - timeout paused while connected\n");
    mark_sessions_info_dirty();
}

void BluetoothManager::onDisconnect(BLEServer* server, ble_gap_conn_desc* desc) {
    const uint16_t gone_id = desc ? desc->conn_handle : connection_id;
    if (device_connected && gone_id != connection_id) {
        // A refused second client went away: the accepted one is untouched
        return;
    }

    const bool was_connected = device_connected;
    device_connected = false;
    last_disconnect_time = millis(); // Reset timeout countdown from now
    if (was_connected) {
        tx_subscribed = false;
        att_mtu = 0;
        portENTER_CRITICAL(&glink_rx_mux);
        glink_rx.reset();
        portEXIT_CRITICAL(&glink_rx_mux);
        link_epoch++;
    }

    log("BLE: Client disconnected - timeout countdown resumed\n");

    if (ota_handler.is_ota_active()) {
        ota_handler.abort_ota();
    }

    if (data_export_in_progress) {
        stop_data_export();
    }

    debug_stream_active = false;

    // Restart advertising for next connection
    delay(500);
    start_advertising();
}

void BluetoothManager::onMtuChanged(BLEServer* server, ble_gap_conn_desc* desc, uint16_t mtu) {
    // The negotiated ATT MTU of the accepted client; the Bluetooth task sizes the packets from it
    if (desc && device_connected && desc->conn_handle == connection_id) {
        att_mtu = mtu;
    }
}

BleChar BluetoothManager::classify(const BLECharacteristic* characteristic) const {
    if (!characteristic) return BleChar::COUNT;
    if (characteristic == ota_data_characteristic) return BleChar::OTA_DATA;
    if (characteristic == ota_control_characteristic) return BleChar::OTA_CONTROL;
    if (characteristic == ota_status_characteristic) return BleChar::OTA_STATUS;
    if (characteristic == build_number_characteristic) return BleChar::OTA_BUILD_NUMBER;
    if (characteristic == data_control_characteristic) return BleChar::DATA_CONTROL;
    if (characteristic == data_transfer_characteristic) return BleChar::DATA_TRANSFER;
    if (characteristic == data_status_characteristic) return BleChar::DATA_STATUS;
    if (characteristic == debug_rx_characteristic) return BleChar::DEBUG_RX;
    if (characteristic == debug_tx_characteristic) return BleChar::DEBUG_TX;
    if (characteristic == sysinfo_system_characteristic) return BleChar::SYSINFO_SYSTEM;
    if (characteristic == sysinfo_performance_characteristic) return BleChar::SYSINFO_PERFORMANCE;
    if (characteristic == sysinfo_hardware_characteristic) return BleChar::SYSINFO_HARDWARE;
    if (characteristic == sysinfo_sessions_characteristic) return BleChar::SYSINFO_SESSIONS;
    if (characteristic == sysinfo_diagnostics_characteristic) return BleChar::SYSINFO_DIAGNOSTICS;
    if (characteristic == glink_rx_characteristic) return BleChar::GLINK_RX;
    if (characteristic == glink_tx_characteristic) return BleChar::GLINK_TX;
    return BleChar::COUNT;
}

void BluetoothManager::onWrite(BLECharacteristic* characteristic, ble_gap_conn_desc* desc) {
    if (characteristic == glink_rx_characteristic) {
        glink_on_rx_write(characteristic, desc ? desc->conn_handle : BLE_HS_CONN_HANDLE_NONE);
        return;
    }
    onWrite(characteristic);
}

void BluetoothManager::onWrite(BLECharacteristic* characteristic) {
    if (!ble_enabled) return;
    // Per-characteristic filter: LINK_ONLY serves GLink only (OTA, data export, debug and diagnostics are refused)
    if (!ble_char_access_allowed(link_mode(), classify(characteristic), BleAccess::WRITE)) {
        LOG_BLE("  -> write refused in this mode\n");
        return;
    }
    if (characteristic == ota_control_characteristic) {
        handle_ota_control_command(characteristic);
    } else if (characteristic == ota_data_characteristic) {
        handle_ota_data_chunk(characteristic);
    } else if (characteristic == debug_rx_characteristic) {
        LOG_BLE("  -> Handling debug command\n");
        handle_debug_command(characteristic);
    } else if (characteristic == data_control_characteristic) {
        LOG_BLE("  -> Handling data control\n");
        handle_data_control_command(characteristic);
    } else if (characteristic == sysinfo_diagnostics_characteristic) {
        LOG_BLE("  -> QUEUING DIAGNOSTIC REPORT REQUEST\n");
        diagnostic_report_pending = true; // Defer heavy work to bluetooth task context
    } else {
        LOG_BLE("  -> UNKNOWN characteristic!\n");
    }
}

void BluetoothManager::onRead(BLECharacteristic* characteristic) {
    // Runs before the stack reads the value: a read the mode refuses (LINK_ONLY: sysinfo, OTA build number and
    // status, data export) returns nothing. FULL restores the values when it starts (enable()).
    const BleChar ch = classify(characteristic);
    if (ch != BleChar::COUNT && !ble_char_access_allowed(link_mode(), ch, BleAccess::READ)) {
        characteristic->setValue(reinterpret_cast<const uint8_t*>(""), 0);
    }
}

void BluetoothManager::onSubscribe(BLECharacteristic* characteristic, ble_gap_conn_desc* desc, uint16_t sub_value) {
    // CCCD of GLink TX: bit 0 is notifications. Only the accepted client counts.
    constexpr uint16_t kSubNotify = 0x0001;
    if (characteristic == glink_tx_characteristic && desc && device_connected && desc->conn_handle == connection_id) {
        tx_subscribed = (sub_value & kSubNotify) != 0;
    }
}

void BluetoothManager::onStatus(BLECharacteristic* characteristic, Status status, uint32_t code) {
    // Any outcome of a GLink notification other than success means the fragment did not go out: the Bluetooth task
    // drops the rest of that message and sends a fresh one later (docs/GLINK-PROTOCOL.md section 8).
    if (characteristic == glink_tx_characteristic && status != Status::SUCCESS_NOTIFY) {
        tx_failed = true;
    }
}

//------------------------------------------------------------------------------
// GLink (chg020): the Bluetooth task parses, answers and broadcasts; the host task only copies fragments.
// No payload is ever logged.
//------------------------------------------------------------------------------
void BluetoothManager::glink_on_rx_write(BLECharacteristic* characteristic, uint16_t conn_handle) {
    // Only the accepted client may talk to GLink; a refused or unknown connection is ignored
    if (!ble_enabled || !device_connected || conn_handle != connection_id) return;
    if (!ble_char_access_allowed(link_mode(), BleChar::GLINK_RX, BleAccess::WRITE)) return;

    const size_t len = characteristic->getLength();
    if (len == 0) return;
    rx_activity = true;
    const uint32_t now_ms = millis();
    portENTER_CRITICAL(&glink_rx_mux);
    glink_rx.on_fragment(characteristic->getData(), len, now_ms);
    portEXIT_CRITICAL(&glink_rx_mux);
}

void BluetoothManager::glink_disconnect_client(const char* reason) {
    if (glink_disconnect_sent) return;
    glink_disconnect_sent = true;
    log("GLink: disconnecting the client (%s)\n", reason);
    if (ble_server && device_connected) {
        ble_server->disconnect(connection_id);
    }
}

static GlinkPeriodKey glink_period_key(const GlinkPeriodInfo& info) {
    GlinkPeriodKey key;
    key.dev_state = info.dev_state;
    key.phase = info.phase;
    key.select_profile = info.select_profile;
    key.blocked = info.blocked;
    key.result_seq = info.result_seq;
    key.profile_rev = info.profile_rev;
    return key;
}

// snCode: the 6 bytes of the factory MAC as 12 lower-case hex digits (docs/GLINK-PROTOCOL.md 5.2)
static void glink_serial_code(char* out, size_t cap) {
    const uint64_t mac = ESP.getEfuseMac();
    snprintf(out, cap, "%02x%02x%02x%02x%02x%02x", static_cast<unsigned>(mac & 0xFF),
             static_cast<unsigned>((mac >> 8) & 0xFF), static_cast<unsigned>((mac >> 16) & 0xFF),
             static_cast<unsigned>((mac >> 24) & 0xFF), static_cast<unsigned>((mac >> 32) & 0xFF),
             static_cast<unsigned>((mac >> 40) & 0xFF));
}

void BluetoothManager::glink_send_error(int object, GlinkError code, const char* message) {
    char* out = glink_tx.scratch();
    if (!out) return;
    const size_t n = glink_encode_error(out, glink_tx.scratch_capacity(), object, code, message, nullptr);
    if (n != 0) {
        tx_failed = false;
        glink_tx.commit(n, glink_tx_sequence++);
    }
}

// Answers at most one pending item (an oversize or overrun notice, or one request). The TX buffer is idle.
void BluetoothManager::glink_process_request(uint32_t now_ms) {
    enum class Work : uint8_t { NONE, TOO_LARGE, OVERRUN, REQUEST } work = Work::NONE;
    portENTER_CRITICAL(&glink_rx_mux);
    if (glink_rx.take_too_large()) {
        work = Work::TOO_LARGE;
    } else if (glink_rx.take_overrun()) {
        work = Work::OVERRUN;
    } else if (glink_rx.has_message()) {
        work = Work::REQUEST;
    }
    portEXIT_CRITICAL(&glink_rx_mux);

    if (work == Work::TOO_LARGE) {
        glink_send_error(-1, GlinkError::PAYLOAD_TOO_LARGE, glink_msg::TOO_LARGE);
        return;
    }
    if (work == Work::OVERRUN) {
        glink_send_error(-1, GlinkError::BUSY, glink_msg::BUSY);
        return;
    }
    if (work != Work::REQUEST) return;

    // The pending buffer is stable until release_message(): the host task only flags overruns while it is occupied
    GlinkRequest request;
    GlinkFailure failure;
    char* out = glink_tx.scratch();
    const size_t cap = glink_tx.scratch_capacity();
    size_t n = 0;

    if (!glink_parse_request(glink_rx.message(), glink_rx.message_length(), request, failure) ||
        !glink_gate(glink_session.handshaken(), glink_pending_, request, failure)) {
        n = glink_encode_failure(out, cap, failure);
    } else if (request.object == GlinkObject::APP_HELLO) {
        // No direct response: baseInfo, then periodInfo, follow as broadcasts
        glink_session.on_handshake();
        glink_result_feed.on_handshake();
        log("GLink: client handshake\n");
    } else if (request.object == GlinkObject::GRIND_CONTROL && request.op == GlinkOp::STOP) {
        // Never queued, never refused: cuts the motor and the grind on this task (the same call as the web panel's stop),
        // and bumps the counter the UI task watches to cancel a lead-in or an autotune run. Also before the handshake and
        // during an update. Answers at once, also when nothing was running.
        portal_stop_now();
        log("GLink: stop\n");
        n = glink_encode_stop(out, cap);
    } else if (UpdateGuard::is_held() || ota_handler.is_ota_active()) {
        n = glink_encode_error(out, cap, static_cast<int>(request.object), GlinkError::UPDATING, glink_msg::UPDATING, nullptr);
    } else if (request.object == GlinkObject::GRIND_PROFILE && request.op == GlinkOp::GET) {
        // Served from the snapshot the UI task publishes; a stale one means the UI task is not running
        if (!glink_command_gate.read_snapshot(&glink_snapshot_)) {
            n = glink_encode_error(out, cap, static_cast<int>(request.object), GlinkError::UI_BUSY, glink_msg::UI_BUSY, nullptr);
        } else {
            n = glink_serve_get(glink_snapshot_, request, out, cap, failure);
            if (n == 0) n = glink_encode_failure(out, cap, failure);
        }
    } else if (request.object == GlinkObject::GRIND_RESULT) {
        // grindResult get: the last result is part of the snapshot the UI task publishes
        if (!glink_command_gate.read_snapshot(&glink_snapshot_)) {
            n = glink_encode_error(out, cap, static_cast<int>(request.object), GlinkError::UI_BUSY, glink_msg::UI_BUSY, nullptr);
        } else {
            n = glink_serve_result_get(glink_snapshot_, out, cap, failure);
            if (n == 0) n = glink_encode_failure(out, cap, failure);
        }
    } else {
        // grindProfile select and update, grindControl start: decided and applied on the UI task; the answer is polled on
        // the next ticks
        if (glink_command_gate.submit(request)) {
            glink_pending_ = true;
            glink_pending_object_ = static_cast<int>(request.object);
            glink_pending_epoch_ = glink_epoch_seen;
        } else {
            n = glink_encode_error(out, cap, static_cast<int>(request.object), GlinkError::UI_BUSY, glink_msg::UI_BUSY, nullptr);
        }
    }

    portENTER_CRITICAL(&glink_rx_mux);
    glink_rx.release_message();
    portEXIT_CRITICAL(&glink_rx_mux);

    if (n != 0) {
        tx_failed = false;
        glink_tx.commit(n, glink_tx_sequence++);
    }
}

// The UI task's answer to the grindProfile / grindControl request filed earlier. Polled only while the TX buffer is idle, so the answer
// (which the poll consumes) always has room; an answer for a client that has gone meanwhile is dropped (7.4).
void BluetoothManager::glink_poll_pending() {
    if (!glink_pending_ || !glink_tx.idle()) return;
    GlinkMailboxResult reply;
    const GlinkMailboxWait wait = glink_command_gate.poll(&reply);
    if (wait == GlinkMailboxWait::WAITING) return;
    glink_pending_ = false;
    if (glink_pending_epoch_ != glink_epoch_seen) return;

    char* out = glink_tx.scratch();
    const size_t cap = glink_tx.scratch_capacity();
    size_t n = 0;
    if (wait == GlinkMailboxWait::TIMEOUT) {
        // An abandoned request answers UI_BUSY although the UI task may still apply the write; profileRev and the
        // compare-first writes make the client's retry harmless
        n = glink_encode_error(out, cap, glink_pending_object_, GlinkError::UI_BUSY, glink_msg::UI_BUSY, nullptr);
    } else if (reply.kind == GlinkReplyKind::PROFILE_UPDATE) {
        n = glink_encode_profile_update(out, cap, reply.entry, reply.changed, reply.stored);
    } else if (reply.kind == GlinkReplyKind::SELECT) {
        n = glink_encode_select(out, cap, reply.select_profile);
    } else if (reply.kind == GlinkReplyKind::START) {
        n = glink_encode_start(out, cap, reply.lead_in_ms);
    } else {
        n = glink_encode_failure(out, cap, reply.failure);
    }
    if (n != 0) {
        tx_failed = false;
        glink_tx.commit(n, glink_tx_sequence++);
    }
}

// periodInfo from the UI task's snapshot. If the UI task stopped publishing, the heartbeat goes on with devState UNKNOWN
// and no live values, so a client sees the link is alive but the grinder is not answering.
void BluetoothManager::glink_fill_period(GlinkPeriodInfo& info) {
    if (glink_command_gate.read_snapshot(&glink_snapshot_)) {
        info = glink_snapshot_.period;
        glink_last_period_ = info;
        glink_have_period_ = true;
        return;
    }
    info = glink_have_period_ ? glink_last_period_ : GlinkPeriodInfo();
    info.dev_state = GlinkDevState::UNKNOWN;
    info.weight_valid = false;
    info.phase = GlinkPhase::NONE;
    info.has_target = false;
    info.has_lead_in = false;
    info.blocked = glink_blocked(GlinkDevState::UNKNOWN, GlinkStartBlock::NONE);
}

// grindResult broadcast (5.6): once per finished grind, to the handshaken client, from the snapshot the UI task publishes.
// Not sent while the snapshot is stale. A notification that fails drops the message; the client falls back to `get`.
bool BluetoothManager::glink_send_result_if_due() {
    if (!glink_command_gate.read_snapshot(&glink_snapshot_)) return false;
    const GlinkResultInfo& result = glink_snapshot_.last_result;
    if (!glink_result_feed.due(glink_snapshot_.has_result, result.seq)) return false;
    char* out = glink_tx.scratch();
    if (!out) return false;
    const size_t n = glink_encode_result_broadcast(out, glink_tx.scratch_capacity(), result);
    glink_result_feed.mark_sent(result.seq);  // also when it did not fit: never loop on it
    if (n == 0) return false;
    tx_failed = false;
    glink_tx.commit(n, glink_tx_sequence++);
    log("GLink: grindResult seq %u sent\n",static_cast<unsigned>(result.seq));
    return true;
}

void BluetoothManager::glink_pump_tx(uint32_t now_ms) {
    if (glink_tx.idle()) return;
    if (!glink_tx_characteristic || !glink_session.subscribed()) {
        glink_tx.abort();  // nobody listens: the message is dropped, a fresh one follows when someone does
        return;
    }
    if (tx_failed.exchange(false)) {
        glink_tx.abort();
        return;
    }

    static uint32_t size_logged_epoch = 0;  // one line per connection: the packet size the first message uses
    if (size_logged_epoch != link_epoch.load()) {
        size_logged_epoch = link_epoch.load();
        log("GLink: packet size %u (ATT MTU %u, callback value %u)\n", static_cast<unsigned>(glink_session.packet_len()),
            static_cast<unsigned>(glink_session.mtu()), static_cast<unsigned>(att_mtu.load()));
    }

    uint8_t packet[GLINK_MAX_PACKET_BYTES];
    uint8_t sent = 0;
    const uint8_t allowance = glink_pacer.allowance(now_ms);
    while (sent < allowance && !glink_tx.idle()) {
        const size_t len = glink_tx.peek(glink_session.packet_len(), packet, sizeof(packet));
        if (len == 0) {
            glink_tx.abort();
            break;
        }
        glink_tx_characteristic->setValue(packet, len);
        glink_tx_characteristic->notify();
        sent++;
        if (tx_failed.exchange(false)) {
            glink_tx.abort();
            break;
        }
        glink_tx.advance();
    }
    if (sent != 0) {
        glink_pacer.note_sent(now_ms, sent);
    }
}

void BluetoothManager::glink_service_tick(uint32_t now_ms) {
    // A connect or disconnect since the last tick: forget everything of the previous client
    const uint32_t epoch = link_epoch.load();
    if (epoch != glink_epoch_seen) {
        glink_epoch_seen = epoch;
        glink_session.reset();
        glink_result_feed.reset();
        glink_tx.abort();
        glink_pacer.reset();
        glink_disconnect_sent = false;
        if (device_connected) {
            // LINK_ONLY: the 10 s clock runs from the connect. FULL clients (grinder.py, web flasher) never send
            // appHello, so there it starts at the first GLink activity and a client without any is never dropped.
            glink_session.on_connect(link_connect_ms.load(), !full_enabled);
        }
    }
    if (!device_connected) {
        glink_poll_pending();  // settles a request filed for a client that has gone (the answer is dropped)
    }
    if (!device_connected || !glink_session.connected() || glink_disconnect_sent) return;

    glink_session.set_subscribed(tx_subscribed.load());
    if (tx_subscribed.load() || rx_activity.exchange(false)) {
        glink_session.note_activity(now_ms);
    }
    // The callback value can be missing (0) although the stack negotiated a larger MTU: then the packets would be 20 bytes
    // and a hello reply alone would be 17 notifications. Ask the stack.
    glink_session.set_mtu(glink_effective_mtu(att_mtu.load(), ble_server ? ble_server->getPeerMTU(connection_id) : 0));

    portENTER_CRITICAL(&glink_rx_mux);
    const bool too_many_bad_fragments = glink_rx.disconnect_requested();
    portEXIT_CRITICAL(&glink_rx_mux);
    if (too_many_bad_fragments) {
        glink_disconnect_client("too many bad fragments");
        return;
    }
    if (glink_session.handshake_timed_out(now_ms)) {
        glink_disconnect_client("no handshake");
        return;
    }

    glink_pump_tx(now_ms);
    glink_poll_pending();
    if (glink_tx.idle()) {
        glink_process_request(now_ms);
    }
    if (glink_tx.idle() && glink_session.base_info_due()) {
        GlinkBaseInfo info;
        char sn[BLE_GLINK_BASE_INFO_SN_HEX_LEN + 1];
        glink_serial_code(sn, sizeof(sn));
        info.release_ver = BUILD_NUMBER;
        info.sn_code = sn;
        info.fw_version = BUILD_FIRMWARE_VERSION;
        info.board = board().name();
        char* out = glink_tx.scratch();
        const size_t n = glink_encode_base_info(out, glink_tx.scratch_capacity(), info);
        if (n != 0) {
            tx_failed = false;
            glink_tx.commit(n, glink_tx_sequence++);
        }
        glink_session.mark_base_info_sent();  // also when it did not fit: never loop on it
    } else if (glink_tx.idle() && glink_session.handshaken() && glink_send_result_if_due()) {
        // grindResult broadcast committed; the periodInfo it changed (resultSeq) follows on the next tick
    } else if (glink_tx.idle()) {
        GlinkPeriodInfo info;
        glink_fill_period(info);
        const GlinkPeriodKey key = glink_period_key(info);
        const bool weight_changed = info.weight_valid != glink_sent_weight_valid_ ||
                                    (info.weight_valid && info.weight_tenths != glink_sent_weight_tenths_);
        if (glink_session.period_due(now_ms, key, weight_changed)) {
            char* out = glink_tx.scratch();
            const size_t n = glink_encode_period_info(out, glink_tx.scratch_capacity(), info);
            if (n != 0) {
                tx_failed = false;
                glink_tx.commit(n, glink_tx_sequence++);
            }
            glink_session.mark_period_sent(now_ms, key);
            glink_sent_weight_valid_ = info.weight_valid;
            glink_sent_weight_tenths_ = info.weight_tenths;
        }
    }
    glink_pump_tx(now_ms);
}

String BluetoothManager::check_ota_failure_after_boot() {
    return ota_handler.check_ota_failure_after_boot();
}

void BluetoothManager::refresh_system_info() {
    if (!full_enabled || !device_connected) return;
    
    update_system_info();
    update_performance_info();
    update_hardware_info();
}

void BluetoothManager::update_system_info() {
    if (!sysinfo_system_characteristic) return;
    
    // Create JSON-like structure for system info
    char buffer[BLE_SYSINFO_MAX_PAYLOAD_BYTES];
    unsigned long uptime_ms = millis();
    unsigned long uptime_seconds = uptime_ms / 1000;
    unsigned long uptime_minutes = uptime_seconds / 60;
    unsigned long uptime_hours = uptime_minutes / 60;
    
    // Get ESP32 system information
    size_t heap_free = ESP.getFreeHeap();
    size_t heap_total = ESP.getHeapSize();
    size_t heap_used = heap_total - heap_free;
    uint32_t flash_size = ESP.getFlashChipSize();
    float heap_usage_percent = (float(heap_used) / float(heap_total)) * 100.0f;
    
    snprintf(buffer, sizeof(buffer),
        "{"
        "\"version\":\"%s\","
        "\"build\":%d,"
        "\"uptime_h\":%lu,"
        "\"uptime_m\":%lu,"
        "\"uptime_s\":%lu,"
        "\"heap_free\":%u,"
        "\"heap_total\":%u,"
        "\"heap_used_pct\":%.1f,"
        "\"flash_size\":%u,"
        "\"cpu_freq\":%u,"
        "\"board\":\"%s\","
        "\"board_src\":\"%s\""
        "}",
        BUILD_FIRMWARE_VERSION,
        BUILD_NUMBER,
        uptime_hours,
        uptime_minutes % 60,
        uptime_seconds % 60,
        (unsigned int)heap_free,
        (unsigned int)heap_total,
        heap_usage_percent,
        (unsigned int)flash_size,
        (unsigned int)ESP.getCpuFreqMHz(),
        board().name(),
        board().source_name()
    );
    
    sysinfo_system_characteristic->setValue(buffer);
    sysinfo_system_characteristic->notify();
}

void BluetoothManager::update_performance_info() {
    if (!sysinfo_performance_characteristic) return;
    
    // Get performance metrics from the performance monitor
    char buffer[BLE_SYSINFO_MAX_PAYLOAD_BYTES];
    
    // For now, create a simple performance summary
    // In a full implementation, we'd extract actual performance data
    snprintf(buffer, sizeof(buffer),
        "{"
        "\"tasks_registered\":6,"
        "\"system_healthy\":true,"
        "\"load_cell_freq_hz\":50,"
        "\"grind_control_freq_hz\":50,"
        "\"ui_freq_hz\":10,"
        "\"bluetooth_freq_hz\":20,"
        "\"debug_freq_hz\":1"
        "}"
    );
    
    sysinfo_performance_characteristic->setValue(buffer);
    sysinfo_performance_characteristic->notify();
}

void BluetoothManager::update_hardware_info() {
    if (!sysinfo_hardware_characteristic) return;
    
    char buffer[BLE_SYSINFO_MAX_PAYLOAD_BYTES];

    // Mirrors UIManager::motor_available()
    extern HardwareManager hardware_manager;
    const bool motor_available = motor_enabled() && hardware_manager.get_grinder()->is_initialized();

    // Never include the SSID, password or token here
    char wifi_ip[16];
    wifi_manager.get_ip(wifi_ip, sizeof(wifi_ip));

    snprintf(buffer, sizeof(buffer),
        "{"
        "\"load_cell_active\":true,"
        "\"motor_available\":%s,"
        "\"display_active\":true,"
        "\"touch_active\":true,"
        "\"ble_enabled\":true,"
        "\"wifi_available\":true,"
        "\"wifi\":%s,"
        "\"ip\":\"%s\","
        "\"host\":\"%s.local\","
        "\"flash_available\":true"
        "}",
        motor_available ? "true" : "false",
        wifi_manager.is_connected() ? "true" : "false",
        wifi_ip,
        wifi_manager.hostname()
    );
    
    sysinfo_hardware_characteristic->setValue(buffer);
    sysinfo_hardware_characteristic->notify();
}

bool BluetoothManager::update_sessions_info() {
    if (!sysinfo_sessions_characteristic) return false;

    if (ota_handler.is_ota_active()) {
        LOG_BLE_DEBUG("System info: skipping session count while OTA is active\n");
        return false;
    }
    
    char buffer[BLE_SYSINFO_MAX_PAYLOAD_BYTES];
    uint16_t session_count = data_stream.get_total_sessions();
    
    snprintf(buffer, sizeof(buffer),
        "{"
        "\"total_sessions\":%u,"
        "\"data_available\":%s,"
        "\"export_active\":%s,"
        "\"last_export_time\":0"
        "}",
        session_count,
        session_count > 0 ? "true" : "false",
        data_export_in_progress ? "true" : "false"
    );
    
    sysinfo_sessions_characteristic->setValue(buffer);
    if (device_connected) {
        sysinfo_sessions_characteristic->notify();
    }

    return true;
}

void BluetoothManager::process_sessions_info_updates() {
    if (!full_enabled || !device_connected || !sysinfo_sessions_characteristic) {
        return;
    }

    uint32_t storage_version = grind_logger.get_session_storage_version();
    bool version_changed = (storage_version != last_session_storage_version);
    bool export_changed = (data_export_in_progress != last_reported_export_state);

    if (!sessions_info_dirty && !version_changed && !export_changed) {
        return;
    }

    if (!update_sessions_info()) {
        // Keep dirty so we'll retry once OTA finishes or characteristic is ready
        sessions_info_dirty = true;
        return;
    }

    last_session_storage_version = storage_version;
    last_reported_export_state = data_export_in_progress;
    sessions_info_dirty = false;
}

void BluetoothManager::mark_sessions_info_dirty() {
    sessions_info_dirty = true;
}

void BluetoothManager::generate_diagnostic_report() {
    LOG_BLE("=== DIAGNOSTICS: generate_diagnostic_report() CALLED ===\n");

    if (!debug_tx_characteristic) {
        LOG_BLE("ERROR: debug_tx_characteristic is NULL\n");
        return;
    }

    LOG_BLE("DEBUG: debug_tx_characteristic is valid, starting report generation\n");

    // Access global instances
    extern HardwareManager hardware_manager;
    extern GrindController grind_controller;

    char buf[512];

    // Helper lambda to send chunk and flush in flow-controlled slices
    auto send_chunk = [this](const char* chunk) {
        if (!debug_tx_characteristic || !chunk) return;
        size_t len = strlen(chunk);
        if (len == 0) return;

        const TickType_t chunk_delay = pdMS_TO_TICKS(BLE_DEBUG_CHUNK_DELAY_MS);

        size_t offset = 0;
        while (offset < len) {
            size_t part_len = std::min(static_cast<size_t>(BLE_DEBUG_MAX_CHUNK_BYTES), len - offset);
            LOG_BLE("TX: %zu bytes\n", part_len);
            debug_tx_characteristic->setValue(reinterpret_cast<const uint8_t*>(chunk + offset), part_len);
            debug_tx_characteristic->notify();
            vTaskDelay(chunk_delay); // Give BLE stack time to drain queue
            offset += part_len;
        }
    };

    // Section 1: Header & Firmware Info
    unsigned long uptime_ms = millis();
    unsigned long uptime_s = uptime_ms / 1000;
    unsigned long uptime_h = uptime_s / 3600;
    unsigned long uptime_m = (uptime_s % 3600) / 60;
    unsigned long uptime_sec = uptime_s % 60;

    snprintf(buf, sizeof(buf),
        "=== SMART GRIND BY WEIGHT - DIAGNOSTIC REPORT ===\n"
        "Generated: %s\n"
        "\n"
        "[FIRMWARE]\n"
        "  Version: %s\n"
        "  Build: #%d\n"
        "  Git: %s (%s)\n"
        "  Built: %s\n"
        "\n",
        get_build_datetime(),
        BUILD_FIRMWARE_VERSION,
        BUILD_NUMBER,
        get_git_commit_id(),
        get_git_branch(),
        BUILD_TIMESTAMP
    );
    send_chunk(buf);

    // Section 2: System Runtime
    size_t heap_free = ESP.getFreeHeap();
    size_t heap_total = ESP.getHeapSize();
    float heap_used_pct = (float(heap_total - heap_free) / float(heap_total)) * 100.0f;
    uint32_t flash_size = ESP.getFlashChipSize();

    const char* driver_type =
#ifdef MOCK_BUILD
        "MOCK";
#else
        "REAL";
#endif

    snprintf(buf, sizeof(buf),
        "[SYSTEM]\n"
        "  Uptime: %02lu:%02lu:%02lu\n"
        "  CPU: %lu MHz\n"
        "  Heap: %u KB / %u KB (%.1f%% used)\n"
        "  Flash: %u MB\n"
        "  Driver: %s\n"
        "\n",
        uptime_h, uptime_m, uptime_sec,
        (unsigned long)ESP.getCpuFreqMHz(),
        (unsigned int)(heap_free / 1024),
        (unsigned int)(heap_total / 1024),
        heap_used_pct,
        (unsigned int)(flash_size / 1024 / 1024),
        driver_type
    );
    send_chunk(buf);

    // Section 3: Runtime Diagnostics
    WeightSensor* weight_sensor = hardware_manager.get_weight_sensor();

    if (weight_sensor) {
        // Get load cell noise measurements
        float std_dev_g = weight_sensor->get_standard_deviation_g(GRIND_SCALE_PRECISION_SETTLING_TIME_MS);
        int32_t std_dev_adc = weight_sensor->get_standard_deviation_adc(GRIND_SCALE_PRECISION_SETTLING_TIME_MS);
        bool noise_acceptable = weight_sensor->noise_level_diagnostic();
        float cal_factor = weight_sensor->get_calibration_factor();
        bool is_calibrated = weight_sensor->is_calibrated();

        // Get motor latency
        float motor_latency = grind_controller.get_motor_response_latency();

        snprintf(buf, sizeof(buf),
            "[RUNTIME DIAGNOSTICS]\n"
            "  Load Cell Status: %s\n"
            "  Calibration Factor: %.2f\n"
            "  Std Dev (g): %.4f\n"
            "  Std Dev (ADC): %ld\n"
            "  Noise Level: %s\n"
            "  Motor Latency: %.0f ms\n"
            "\n",
            is_calibrated ? "Calibrated" : "NOT CALIBRATED",
            cal_factor,
            std_dev_g,
            (long)std_dev_adc,
            noise_acceptable ? "OK" : "Too High",
            motor_latency
        );
        send_chunk(buf);
    }

    // Section 4: Profiles (requires access to hardware manager's preferences)
    // This will be populated at runtime - for now show defaults
    snprintf(buf, sizeof(buf),
        "[COMPILE-TIME PARAMETERS - PROFILES]\n"
        "  USER_PROFILE_COUNT: %d\n"
        "  USER_SINGLE_ESPRESSO_WEIGHT_G: %.1f\n"
        "  USER_DOUBLE_ESPRESSO_WEIGHT_G: %.1f\n"
        "  USER_CUSTOM_PROFILE_WEIGHT_G: %.1f\n"
        "  USER_SINGLE_ESPRESSO_TIME_S: %.1f\n"
        "  USER_DOUBLE_ESPRESSO_TIME_S: %.1f\n"
        "  USER_CUSTOM_PROFILE_TIME_S: %.1f\n"
        "\n",
        USER_PROFILE_COUNT,
        USER_SINGLE_ESPRESSO_WEIGHT_G,
        USER_DOUBLE_ESPRESSO_WEIGHT_G,
        USER_CUSTOM_PROFILE_WEIGHT_G,
        USER_SINGLE_ESPRESSO_TIME_S,
        USER_DOUBLE_ESPRESSO_TIME_S,
        USER_CUSTOM_PROFILE_TIME_S
    );
    send_chunk(buf);

    // Section 5: User.h Compile Constants (Part 1)
    snprintf(buf, sizeof(buf),
        "[COMPILE-TIME PARAMETERS - USER.H PART 1]\n"
        "  USER_MIN_TARGET_WEIGHT_G: %.1f\n"
        "  USER_MAX_TARGET_WEIGHT_G: %.1f\n"
        "  USER_MIN_TARGET_TIME_S: %.1f\n"
        "  USER_MAX_TARGET_TIME_S: %.1f\n"
        "  USER_FINE_WEIGHT_ADJUSTMENT_G: %.1f\n"
        "  USER_FINE_TIME_ADJUSTMENT_S: %.1f\n"
        "  USER_CALIBRATION_REFERENCE_WEIGHT_G: %.1f\n"
        "  USER_DEFAULT_CALIBRATION_FACTOR: %.1f\n"
        "\n",
        USER_MIN_TARGET_WEIGHT_G,
        USER_MAX_TARGET_WEIGHT_G,
        USER_MIN_TARGET_TIME_S,
        USER_MAX_TARGET_TIME_S,
        USER_FINE_WEIGHT_ADJUSTMENT_G,
        USER_FINE_TIME_ADJUSTMENT_S,
        USER_CALIBRATION_REFERENCE_WEIGHT_G,
        USER_DEFAULT_CALIBRATION_FACTOR
    );
    send_chunk(buf);

    // Section 6: User.h Compile Constants (Part 2 - Screen & Auto)
    snprintf(buf, sizeof(buf),
        "[COMPILE-TIME PARAMETERS - USER.H PART 2]\n"
        "  USER_SCREEN_AUTO_DIM_TIMEOUT_MS (default dim time): %lu\n"
        "  USER_SCREEN_BRIGHTNESS_NORMAL: %.2f\n"
        "  USER_SCREEN_BRIGHTNESS_DIMMED: %.2f\n"
        "  USER_WEIGHT_ACTIVITY_THRESHOLD_G: %.1f\n"
        "  USER_AUTO_GRIND_TRIGGER_DELTA_G: %.1f\n"
        "  USER_AUTO_GRIND_TRIGGER_WINDOW_MS: %lu\n"
        "  USER_AUTO_GRIND_TRIGGER_SETTLING_MS: %lu\n"
        "  USER_AUTO_GRIND_REARM_DELAY_MS: %lu\n"
        "\n",
        (unsigned long)USER_SCREEN_AUTO_DIM_TIMEOUT_MS,
        USER_SCREEN_BRIGHTNESS_NORMAL,
        USER_SCREEN_BRIGHTNESS_DIMMED,
        USER_WEIGHT_ACTIVITY_THRESHOLD_G,
        USER_AUTO_GRIND_TRIGGER_DELTA_G,
        (unsigned long)USER_AUTO_GRIND_TRIGGER_WINDOW_MS,
        (unsigned long)USER_AUTO_GRIND_TRIGGER_SETTLING_MS,
        (unsigned long)USER_AUTO_GRIND_REARM_DELAY_MS
    );
    send_chunk(buf);

    // Section 7: Grind Control Constants (Part 1 - Core)
    snprintf(buf, sizeof(buf),
        "[COMPILE-TIME PARAMETERS - GRIND_CONTROL.H PART 1]\n"
        "  GRIND_ACCURACY_TOLERANCE_G: %.3f\n"
        "  GRIND_TIMEOUT_SEC: %d\n"
        "  GRIND_MAX_PULSE_ATTEMPTS: %d\n"
        "  GRIND_FLOW_DETECTION_THRESHOLD_GPS: %.1f\n"
        "  GRIND_UNDERSHOOT_TARGET_G: %.1f\n"
        "  GRIND_LATENCY_TO_COAST_RATIO: %.1f\n"
        "  GRIND_SCALE_SETTLING_TOLERANCE_G: %.3f\n"
        "  GRIND_TIME_PULSE_DURATION_MS: %d\n"
        "\n",
        GRIND_ACCURACY_TOLERANCE_G,
        GRIND_TIMEOUT_SEC,
        GRIND_MAX_PULSE_ATTEMPTS,
        GRIND_FLOW_DETECTION_THRESHOLD_GPS,
        GRIND_UNDERSHOOT_TARGET_G,
        GRIND_LATENCY_TO_COAST_RATIO,
        GRIND_SCALE_SETTLING_TOLERANCE_G,
        GRIND_TIME_PULSE_DURATION_MS
    );
    send_chunk(buf);

    // Section 8: Grind Control Constants (Part 2 - Flow & Motor)
    snprintf(buf, sizeof(buf),
        "[COMPILE-TIME PARAMETERS - GRIND_CONTROL.H PART 2]\n"
        "  GRIND_FLOW_RATE_MIN_SANE_GPS: %.1f\n"
        "  GRIND_FLOW_RATE_MAX_SANE_GPS: %.1f\n"
        "  GRIND_PULSE_FLOW_RATE_FALLBACK_GPS: %.1f\n"
        "  GRIND_MOTOR_RESPONSE_LATENCY_DEFAULT_MS: %.1f\n"
        "  GRIND_MOTOR_MAX_PULSE_DURATION_MS: %.1f\n"
        "  GRIND_MOTOR_SETTLING_TIME_MS: %d\n"
        "  GRIND_MECHANICAL_DROP_THRESHOLD_G: %.1f\n"
        "  GRIND_MECHANICAL_EVENT_COOLDOWN_MS: %d\n"
        "  GRIND_MECHANICAL_EVENT_REQUIRED_COUNT: %d\n"
        "\n",
        GRIND_FLOW_RATE_MIN_SANE_GPS,
        GRIND_FLOW_RATE_MAX_SANE_GPS,
        GRIND_PULSE_FLOW_RATE_FALLBACK_GPS,
        GRIND_MOTOR_RESPONSE_LATENCY_DEFAULT_MS,
        GRIND_MOTOR_MAX_PULSE_DURATION_MS,
        GRIND_MOTOR_SETTLING_TIME_MS,
        GRIND_MECHANICAL_DROP_THRESHOLD_G,
        GRIND_MECHANICAL_EVENT_COOLDOWN_MS,
        GRIND_MECHANICAL_EVENT_REQUIRED_COUNT
    );
    send_chunk(buf);

    // Section 9: Grind Control Constants (Part 3 - Settling & Calibration)
    snprintf(buf, sizeof(buf),
        "[COMPILE-TIME PARAMETERS - GRIND_CONTROL.H PART 3]\n"
        "  GRIND_SCALE_PRECISION_SETTLING_TIME_MS: %d\n"
        "  GRIND_SCALE_SETTLING_TIMEOUT_MS: %d\n"
        "  GRIND_TARE_SAMPLE_WINDOW_MS: %d\n"
        "  GRIND_TARE_TIMEOUT_MS: %d\n"
        "  GRIND_CALIBRATION_SAMPLE_WINDOW_MS: %d\n"
        "  GRIND_CALIBRATION_TIMEOUT_MS: %d\n"
        "\n",
        GRIND_SCALE_PRECISION_SETTLING_TIME_MS,
        GRIND_SCALE_SETTLING_TIMEOUT_MS,
        GRIND_TARE_SAMPLE_WINDOW_MS,
        GRIND_TARE_TIMEOUT_MS,
        GRIND_CALIBRATION_SAMPLE_WINDOW_MS,
        GRIND_CALIBRATION_TIMEOUT_MS
    );
    send_chunk(buf);

    // Section 10: Autotune Constants
    snprintf(buf, sizeof(buf),
        "[COMPILE-TIME PARAMETERS - AUTOTUNE]\n"
        "  GRIND_AUTOTUNE_LATENCY_MIN_MS: %.1f\n"
        "  GRIND_AUTOTUNE_LATENCY_MAX_MS: %.1f\n"
        "  GRIND_AUTOTUNE_PRIMING_PULSE_MS: %d\n"
        "  GRIND_AUTOTUNE_TARGET_ACCURACY_MS: %.1f\n"
        "  GRIND_AUTOTUNE_SUCCESS_RATE: %.2f\n"
        "  GRIND_AUTOTUNE_VERIFICATION_PULSES: %d\n"
        "  GRIND_AUTOTUNE_MAX_ITERATIONS: %d\n"
        "  GRIND_AUTOTUNE_COLLECTION_DELAY_MS: %d\n"
        "  GRIND_AUTOTUNE_SETTLING_TIMEOUT_MS: %d\n"
        "  GRIND_AUTOTUNE_WEIGHT_THRESHOLD_G: %.3f\n"
        "\n",
        GRIND_AUTOTUNE_LATENCY_MIN_MS,
        GRIND_AUTOTUNE_LATENCY_MAX_MS,
        GRIND_AUTOTUNE_PRIMING_PULSE_MS,
        GRIND_AUTOTUNE_TARGET_ACCURACY_MS,
        GRIND_AUTOTUNE_SUCCESS_RATE,
        GRIND_AUTOTUNE_VERIFICATION_PULSES,
        GRIND_AUTOTUNE_MAX_ITERATIONS,
        GRIND_AUTOTUNE_COLLECTION_DELAY_MS,
        GRIND_AUTOTUNE_SETTLING_TIMEOUT_MS,
        GRIND_AUTOTUNE_WEIGHT_THRESHOLD_G
    );
    send_chunk(buf);

    // Section 11: Statistics
    uint32_t total_grinds = statistics_manager.get_total_grinds();
    uint32_t single_shots = statistics_manager.get_single_shots();
    uint32_t double_shots = statistics_manager.get_double_shots();
    uint32_t custom_shots = statistics_manager.get_custom_shots();
    uint64_t motor_runtime_ms = statistics_manager.get_motor_runtime_ms();
    uint32_t motor_hrs = (uint32_t)(motor_runtime_ms / 3600000ULL);
    uint32_t motor_min = (uint32_t)((motor_runtime_ms % 3600000ULL) / 60000ULL);
    uint32_t device_uptime_hrs = statistics_manager.get_device_uptime_hrs();
    uint32_t device_uptime_min = statistics_manager.get_device_uptime_min_remainder();
    float total_weight_kg = statistics_manager.get_total_weight_kg();
    uint32_t weight_grinds = statistics_manager.get_weight_mode_grinds();
    uint32_t time_grinds = statistics_manager.get_time_mode_grinds();
    float avg_accuracy = statistics_manager.get_avg_accuracy_g();
    uint32_t total_pulses = statistics_manager.get_total_pulses();
    float avg_pulses = statistics_manager.get_avg_pulses();
    uint32_t time_pulses = statistics_manager.get_time_pulses();

    snprintf(buf, sizeof(buf),
        "[STATISTICS]\n"
        "  Total Grinds: %lu\n"
        "  Shots: %lu Single / %lu Double / %lu Custom\n"
        "  Motor Runtime: %luh %lum\n"
        "  Device Uptime: %luh %lum\n"
        "\n",
        total_grinds,
        single_shots, double_shots, custom_shots,
        motor_hrs, motor_min,
        device_uptime_hrs, device_uptime_min
    );
    send_chunk(buf);

    snprintf(buf, sizeof(buf),
        "  Total Weight: %.2f kg\n"
        "  Mode Grinds: %lu Weight / %lu Time\n"
        "  Avg Accuracy: ±%.2f g\n"
        "  Total Pulses: %lu (avg %.1f)\n"
        "  Time Pulses: %lu\n"
        "\n",
        total_weight_kg,
        weight_grinds, time_grinds,
        avg_accuracy,
        total_pulses, avg_pulses,
        time_pulses
    );
    send_chunk(buf);

    // Section 12: NVM Stored Preferences (Auto-Detected)
    snprintf(buf, sizeof(buf), "[NVM STORED PREFERENCES]\n");
    send_chunk(buf);

    // Use NVS iterator to enumerate all entries
    nvs_iterator_t it = nullptr;
    esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);

    if (res != ESP_OK) {
        snprintf(buf, sizeof(buf), "  [ERROR] Failed to create NVS iterator (code %d)\n\n", res);
        send_chunk(buf);
    } else {
        char current_namespace[16] = "";
        bool namespace_started = false;
        int entry_count = 0;

        while (res == ESP_OK && it != nullptr) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);

            // Check if we've moved to a new namespace
            if (strcmp(current_namespace, info.namespace_name) != 0) {
                if (namespace_started) {
                    // End previous namespace
                    snprintf(buf, sizeof(buf), "\n");
                    send_chunk(buf);
                }

                // Start new namespace
                strncpy(current_namespace, info.namespace_name, sizeof(current_namespace) - 1);
                current_namespace[sizeof(current_namespace) - 1] = '\0';
                snprintf(buf, sizeof(buf), "  Namespace: %s\n", current_namespace);
                send_chunk(buf);
                namespace_started = true;
            }

            // Open preferences for this namespace to read the value
            Preferences pref;
            if (pref.begin(info.namespace_name, true)) {
                // Format based on type
                switch (info.type) {
                    case NVS_TYPE_U8: {
                        // Could be bool or uint8
                        uint8_t val = pref.getUChar(info.key, 0);
                        if (val <= 1) {
                            // Likely a boolean
                            snprintf(buf, sizeof(buf), "    %s: %s (bool)\n", info.key, val ? "true" : "false");
                        } else {
                            snprintf(buf, sizeof(buf), "    %s: %u (uint8)\n", info.key, val);
                        }
                        break;
                    }
                    case NVS_TYPE_I8: {
                        int8_t val = pref.getChar(info.key, 0);
                        snprintf(buf, sizeof(buf), "    %s: %d (int8)\n", info.key, val);
                        break;
                    }
                    case NVS_TYPE_U16: {
                        uint16_t val = pref.getUShort(info.key, 0);
                        snprintf(buf, sizeof(buf), "    %s: %u (uint16)\n", info.key, val);
                        break;
                    }
                    case NVS_TYPE_I16: {
                        int16_t val = pref.getShort(info.key, 0);
                        snprintf(buf, sizeof(buf), "    %s: %d (int16)\n", info.key, val);
                        break;
                    }
                    case NVS_TYPE_U32: {
                        uint32_t val = pref.getUInt(info.key, 0);
                        snprintf(buf, sizeof(buf), "    %s: %lu (uint32)\n", info.key, (unsigned long)val);
                        break;
                    }
                    case NVS_TYPE_I32: {
                        int32_t val = pref.getInt(info.key, 0);
                        snprintf(buf, sizeof(buf), "    %s: %ld (int32)\n", info.key, (long)val);
                        break;
                    }
                    case NVS_TYPE_U64: {
                        uint64_t val = pref.getULong64(info.key, 0);
                        snprintf(buf, sizeof(buf), "    %s: %llu (uint64)\n", info.key, val);
                        break;
                    }
                    case NVS_TYPE_I64: {
                        int64_t val = pref.getLong64(info.key, 0);
                        snprintf(buf, sizeof(buf), "    %s: %lld (int64)\n", info.key, val);
                        break;
                    }
                    case NVS_TYPE_STR: {
                        String val = pref.getString(info.key, "");
                        snprintf(buf, sizeof(buf), "    %s: \"%s\" (string)\n", info.key, val.c_str());
                        break;
                    }
                    case NVS_TYPE_BLOB: {
                        size_t len = pref.getBytesLength(info.key);
                        // Check if it's a float (4 bytes)
                        if (len == sizeof(float)) {
                            float val = pref.getFloat(info.key, 0.0f);
                            snprintf(buf, sizeof(buf), "    %s: %.2f (float)\n", info.key, val);
                        } else if (len == sizeof(double)) {
                            double val = pref.getDouble(info.key, 0.0);
                            snprintf(buf, sizeof(buf), "    %s: %.2f (double)\n", info.key, val);
                        } else {
                            snprintf(buf, sizeof(buf), "    %s: <blob %u bytes>\n", info.key, (unsigned int)len);
                        }
                        break;
                    }
                    default: {
                        snprintf(buf, sizeof(buf), "    %s: <unknown type %d>\n", info.key, info.type);
                        break;
                    }
                }
                send_chunk(buf);
                pref.end();
            }

            entry_count++;
            res = nvs_entry_next(&it);
        }

        nvs_release_iterator(it);

        if (entry_count == 0) {
            snprintf(buf, sizeof(buf), "  [EMPTY] No preferences stored\n");
            send_chunk(buf);
        }

        snprintf(buf, sizeof(buf), "\n");
        send_chunk(buf);
    }

    // Section 13: Session Data
    uint16_t session_count = data_stream.get_total_sessions();

    snprintf(buf, sizeof(buf),
        "[SESSION DATA]\n"
        "  Sessions: %u\n"
        "  Events: %lu\n"
        "  Measurements: %lu\n"
        "\n",
        session_count,
        grind_logger.count_total_events_in_flash(),
        grind_logger.count_total_measurements_in_flash()
    );
    send_chunk(buf);

    // Section 14: Last 5 Grind Sessions Detail
    snprintf(buf, sizeof(buf), "[LAST 5 GRIND SESSIONS]\n");
    send_chunk(buf);

    if (LittleFS.exists(GRIND_SESSIONS_DIR)) {
        File dir = LittleFS.open(GRIND_SESSIONS_DIR);
        if (dir && dir.isDirectory()) {
            // Collect all session IDs
            const int MAX_SESSIONS = 100;
            const size_t session_buf_bytes = MAX_SESSIONS * sizeof(uint32_t);
            uint32_t* session_ids = static_cast<uint32_t*>(malloc(session_buf_bytes));
            int count = 0;

            if (session_ids) {
                File file = dir.openNextFile();
                while (file && count < MAX_SESSIONS) {
                    String filename = file.name();
                    if ((filename.startsWith("session_") || filename.indexOf("/session_") != -1) && filename.endsWith(".bin")) {
                        int start_pos = filename.indexOf('_') + 1;
                        int end_pos = filename.lastIndexOf('.');
                        if (start_pos > 0 && end_pos > start_pos) {
                            session_ids[count++] = filename.substring(start_pos, end_pos).toInt();
                        }
                    }
                    file = dir.openNextFile();
                }
                file.close();
                dir.close();

                // Sort session IDs descending (highest/newest first)
                for (int i = 0; i < count - 1; i++) {
                    for (int j = i + 1; j < count; j++) {
                        if (session_ids[i] < session_ids[j]) {
                            uint32_t temp = session_ids[i];
                            session_ids[i] = session_ids[j];
                            session_ids[j] = temp;
                        }
                    }
                }

                // Read and output last 5 sessions
                int sessions_to_show = (count < 5) ? count : 5;
                for (int i = 0; i < sessions_to_show; i++) {
                    char filename[64];
                    snprintf(filename, sizeof(filename), SESSION_FILE_FORMAT, session_ids[i]);

                    File sessionFile = LittleFS.open(filename, "r");
                    if (sessionFile) {
                        TimeSeriesSessionHeader header;
                        GrindSession session;

                        if (sessionFile.read((uint8_t*)&header, sizeof(header)) == sizeof(header) &&
                            sessionFile.read((uint8_t*)&session, sizeof(session)) == sizeof(session)) {

                            const char* mode_name = (session.grind_mode == 0) ? "WEIGHT" : "TIME";
                            const char* term_names[] = {"COMPLETED", "TIMEOUT", "OVERSHOOT", "MAX_PULSES", "UNKNOWN"};
                            const char* term_name = (session.termination_reason < 4) ? term_names[session.termination_reason] : term_names[4];

                            snprintf(buf, sizeof(buf),
                                "\n--- Session #%lu ---\n"
                                "  Mode: %s | Profile: %u | Status: %.16s\n"
                                "  Target: %.1fg | Final: %.1fg | Error: %+.2fg\n"
                                "  Total Time: %.1fs | Motor Time: %.1fs | Pulses: %u\n"
                                "  Termination: %s\n",
                                session.session_id,
                                mode_name, session.profile_id, session.result_status,
                                session.target_weight, session.final_weight, session.error_grams,
                                session.total_time_ms / 1000.0f, session.total_motor_on_time_ms / 1000.0f, session.pulse_count,
                                term_name
                            );
                            send_chunk(buf);

                            // Read and output events
                            if (header.event_count > 0) {
                                snprintf(buf, sizeof(buf), "  Events (%u):\n", header.event_count);
                                send_chunk(buf);

                                static const char* const phase_names[] = {
                                    "IDLE", "INITIALIZING", "SETUP", "TARING", "TARE_CONFIRM",
                                    "PREDICTIVE", "PULSE_DECISION", "PULSE_EXECUTE", "PULSE_SETTLING",
                                    "FINAL_SETTLING", "TIME_GRINDING", "TIME_ADDITIONAL_PULSE", "COMPLETED", "TIMEOUT",
                                    "PRIME", "PRIME_SETTLING", "PURGE_CONFIRM"
                                };
                                const size_t phase_name_count = sizeof(phase_names) / sizeof(phase_names[0]);

                                for (uint16_t e = 0; e < header.event_count; e++) {
                                    GrindEvent event;
                                    if (sessionFile.read((uint8_t*)&event, sizeof(event)) == sizeof(event)) {
                                        const char* phase_name = (event.phase_id < phase_name_count) ? phase_names[event.phase_id] : "UNKNOWN";

                                        // Calculate event yield (delta)
                                        float event_yield = event.end_weight - event.start_weight;

                                        // Build base event string
                                        char base_str[256];
                                        if (event.pulse_attempt_number > 0) {
                                            snprintf(base_str, sizeof(base_str),
                                                "    [%lums] %s (pulse #%u): %.2fg -> %.2fg (%+.2fg) (%.1fms pulse)",
                                                event.timestamp_ms,
                                                phase_name,
                                                event.pulse_attempt_number,
                                                event.start_weight,
                                                event.end_weight,
                                                event_yield,
                                                event.pulse_duration_ms
                                            );
                                        } else {
                                            snprintf(base_str, sizeof(base_str),
                                                "    [%lums] %s: %.2fg -> %.2fg (%+.2fg) (%lums)",
                                                event.timestamp_ms,
                                                phase_name,
                                                event.start_weight,
                                                event.end_weight,
                                                event_yield,
                                                event.duration_ms
                                            );
                                        }

                                        // Build phase-specific metrics suffix
                                        char metrics_str[256] = "";

                                        switch (event.phase_id) {
                                            case 5: // PREDICTIVE
                                                if (event.grind_latency_ms > 0 || event.pulse_flow_rate > 0 || event.motor_stop_target_weight > 0) {
                                                    snprintf(metrics_str, sizeof(metrics_str), " | Latency: %lums, Flow: %.1fg/s, Target: %.1fg",
                                                        event.grind_latency_ms,
                                                        event.pulse_flow_rate,
                                                        event.motor_stop_target_weight
                                                    );
                                                }
                                                break;

                                            case 7: // PULSE_EXECUTE
                                                if (event.pulse_flow_rate > 0 || event.motor_stop_target_weight > 0) {
                                                    snprintf(metrics_str, sizeof(metrics_str), " | Flow: %.1fg/s, Target: %.1fg",
                                                        event.pulse_flow_rate,
                                                        event.motor_stop_target_weight
                                                    );
                                                }
                                                break;

                                            case 8: // PULSE_SETTLING
                                                if (event.settling_duration_ms > 0 || event.motor_stop_target_weight > 0) {
                                                    snprintf(metrics_str, sizeof(metrics_str), " | Settled: %lums, Target: %.1fg",
                                                        event.settling_duration_ms,
                                                        event.motor_stop_target_weight
                                                    );
                                                }
                                                break;

                                            case 9: // FINAL_SETTLING
                                                if (event.settling_duration_ms > 0) {
                                                    snprintf(metrics_str, sizeof(metrics_str), " | Settled: %lums",
                                                        event.settling_duration_ms
                                                    );
                                                }
                                                break;

                                            case 10: // TIME_GRINDING
                                                if (event.pulse_flow_rate > 0) {
                                                    snprintf(metrics_str, sizeof(metrics_str), " | Flow: %.1fg/s",
                                                        event.pulse_flow_rate
                                                    );
                                                }
                                                break;

                                            case 11: // TIME_ADDITIONAL_PULSE
                                                if (event.pulse_flow_rate > 0) {
                                                    snprintf(metrics_str, sizeof(metrics_str), " | Flow: %.1fg/s",
                                                        event.pulse_flow_rate
                                                    );
                                                }
                                                break;
                                        }

                                        // Combine base and metrics, add newline
                                        snprintf(buf, sizeof(buf), "%s%s\n", base_str, metrics_str);
                                        send_chunk(buf);
                                    }
                                }
                            }
                        }
                        sessionFile.close();
                    }
                }

                free(session_ids);
            } else {
                dir.close();
                snprintf(buf, sizeof(buf), "  [ERROR] Unable to allocate session buffer (%u bytes)\n",
                    (unsigned int)session_buf_bytes);
                send_chunk(buf);
            }
        } else {
            snprintf(buf, sizeof(buf), "  [NONE] No session files found\n");
            send_chunk(buf);
        }
    } else {
        snprintf(buf, sizeof(buf), "  [NONE] Sessions directory does not exist\n");
        send_chunk(buf);
    }

    snprintf(buf, sizeof(buf), "\n");
    send_chunk(buf);

    // Section 15: Autotune Results
    snprintf(buf, sizeof(buf), "[AUTOTUNE RESULTS]\n");
    send_chunk(buf);

    if (LittleFS.exists("/autotune.log")) {
        File autotuneFile = LittleFS.open("/autotune.log", "r");
        if (autotuneFile) {
            // Stream file contents in chunks
            while (autotuneFile.available()) {
                size_t bytesToRead = autotuneFile.available();
                if (bytesToRead > sizeof(buf) - 1) {
                    bytesToRead = sizeof(buf) - 1;
                }
                size_t bytesRead = autotuneFile.readBytes(buf, bytesToRead);
                buf[bytesRead] = '\0';
                send_chunk(buf);
            }
            autotuneFile.close();

            // Add newline after file contents
            snprintf(buf, sizeof(buf), "\n");
            send_chunk(buf);
        } else {
            snprintf(buf, sizeof(buf), "  [ERROR] Failed to open autotune.log\n\n");
            send_chunk(buf);
        }
    } else {
        snprintf(buf, sizeof(buf), "  [NOT RUN] Autotune has not been executed yet\n\n");
        send_chunk(buf);
    }

    // Final marker
    LOG_BLE("DEBUG: Sending final marker\n");
    snprintf(buf, sizeof(buf), "=== END OF REPORT ===\n");
    send_chunk(buf);
    LOG_BLE("=== DIAGNOSTICS: Report generation COMPLETED ===\n");
}
