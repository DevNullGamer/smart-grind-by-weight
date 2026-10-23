#include <Arduino.h>
#include <LittleFS.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include "hardware/board_detect.h"
#include "hardware/hardware_manager.h"
#include "system/state_machine.h"
#include "system/statistics_manager.h"
#include "controllers/profile_controller.h"
#include "controllers/grind_controller.h"
#include "ui/ui_manager.h"
#include "config/constants.h"
#include "bluetooth/manager.h"
#include "network/wifi_manager.h"
#include "tasks/task_manager.h"
#include "tasks/weight_sampling_task.h"
#include "tasks/grind_control_task.h"
#include "tasks/file_io_task.h"
#if DEBUG_WIFI_HEAP_SPIKE
#include "system/wifi_heap_spike.h"
#endif
#if DEBUG_BLE_ENABLE_STRESS
#include "system/ble_enable_stress.h"
#endif

HardwareManager hardware_manager;
StateMachine state_machine;
ProfileController profile_controller;
GrindController grind_controller;
UIManager ui_manager;
BluetoothManager g_bluetooth_manager;
BluetoothManager& bluetooth_manager = g_bluetooth_manager;

#if SYS_ENABLE_REALTIME_HEARTBEAT
// Core 1 timing metrics (global scope for main loop access)
static uint32_t core1_cycle_count_10s = 0;
static uint32_t core1_cycle_time_sum_ms = 0;
static uint32_t core1_cycle_time_min_ms = UINT32_MAX;
static uint32_t core1_cycle_time_max_ms = 0;
static uint32_t core1_last_heartbeat_time = 0;
#endif

static void log_internal_heap(const char* stage) {
    LOG_BLE("[HEAP] %s: free %u, largest block %u\n", stage,
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

void setup() {
    // Before anything else: motor pins to a safe state while the board is unknown
    board_park_pins();

    Serial.begin(HW_SERIAL_BAUD_RATE);
#ifdef UI_DEBUG_SERIAL_DELAY_MS
    delay(UI_DEBUG_SERIAL_DELAY_MS);
#endif
    
    // Log reset reason to help diagnose unexpected resets/freeze scenarios
    esp_reset_reason_t rr = esp_reset_reason();
    const char* rr_str = "UNKNOWN";
    switch (rr) {
        case ESP_RST_POWERON: rr_str = "POWERON"; break;
        case ESP_RST_EXT: rr_str = "EXT (Reset Pin)"; break;
        case ESP_RST_SW: rr_str = "SW (esp_restart)"; break;
        case ESP_RST_PANIC: rr_str = "PANIC (Exception)"; break;
        case ESP_RST_INT_WDT: rr_str = "INT_WDT"; break;
        case ESP_RST_TASK_WDT: rr_str = "TASK_WDT"; break;
        case ESP_RST_WDT: rr_str = "WDT"; break;
        case ESP_RST_DEEPSLEEP: rr_str = "DEEPSLEEP"; break;
        case ESP_RST_BROWNOUT: rr_str = "BROWNOUT"; break;
        case ESP_RST_SDIO: rr_str = "SDIO"; break;
        default: break;
    }
    LOG_BLE("[STARTUP] Reset reason: %s (%d)\n", rr_str, rr);
    
    
    // Early startup heartbeat - helps capture initialization sequence
    LOG_BLE("[STARTUP] Initializing ESP32-S3 Coffee Scale - Build %d - Core1 active\n", BUILD_NUMBER);
    // Must run before display init and before any task starts
    detect_board();
    LOG_BLE("[STARTUP] Board: %s (CS %d, HX711 SCK %d, motor %d)\n",
            board().name(), board().display_cs_pin, board().loadcell_sck_pin, board().motor_pin);
    
    // Initialize LittleFS once - format if necessary
    if (!LittleFS.begin(true)) {
        LOG_BLE("ERROR: LittleFS mount failed - continuing without filesystem\n");
    } else {
        LOG_BLE("✅ LittleFS mounted successfully\n");
    }
    
    hardware_manager.init();
#if DEBUG_RMT_PULSE_SELFTEST
    hardware_manager.get_grinder()->run_pulse_selftest();
#endif
    LOG_BLE("[STARTUP] Free internal heap after hardware init: %u bytes\n",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    profile_controller.init(hardware_manager.get_preferences());
    statistics_manager.init(hardware_manager.get_preferences());
    grind_controller.init(hardware_manager.get_load_cell(), hardware_manager.get_grinder(), hardware_manager.get_preferences());
    
    // Set up the reference so HardwareManager can query GrindController state
    hardware_manager.set_grind_controller(&grind_controller);
    
    bluetooth_manager.init(hardware_manager.get_preferences());
    
    // Check for OTA failure to determine initial state
    String failed_ota_build = bluetooth_manager.check_ota_failure_after_boot();
    bool ota_failed = !failed_ota_build.isEmpty();

    // Check calibration status to determine initial screen
    bool is_calibrated = hardware_manager.get_weight_sensor()->is_calibrated();

    if (ota_failed) {
        LOG_BLE("BOOT: Starting in OTA failure state for expected build %s\n", failed_ota_build.c_str());
        state_machine.init(UIState::OTA_UPDATE_FAILED);
    } else if (!is_calibrated) {
        LOG_BLE("BOOT: Device not calibrated - starting in CALIBRATION state\n");
        state_machine.init(UIState::CALIBRATION);
    } else {
        state_machine.init(UIState::READY);
    }
    
    log_internal_heap("before UI init");
    ui_manager.init(&hardware_manager, &state_machine, &profile_controller, &grind_controller, &bluetooth_manager);
    
    // Store OTA failure info in ui_manager if needed
    if (ota_failed) {
        if (auto* ota = ui_manager.get_ota_data_export_controller()) {
            ota->set_failure_info(failed_ota_build.c_str());
        }
    }
    
    // Set up UI status callback to avoid circular dependency
    bluetooth_manager.set_ui_status_callback([](const char* status) {
        if (auto* ota = ui_manager.get_ota_data_export_controller()) {
            ota->update_status(status);
        }
    });
    
    // Enable BLE by default during bootup with the BLE_BOOTUP_AUTO_DISABLE_TIMEOUT_MS timeout (5 minutes)
    // (Previously disabled by default for security, now enabled for user convenience)
    log_internal_heap("after UI init");
    bluetooth_manager.enable_during_bootup();
    log_internal_heap("after BLE enable");
    
    // Initialize individual task modules BEFORE TaskManager creates FreeRTOS tasks
    // This ensures all task dependencies are ready before tasks start running
    LOG_BLE("[STARTUP] Initializing task module dependencies...\n");
    weight_sampling_task.init(hardware_manager.get_load_cell(), &grind_logger);
    grind_control_task.init(&grind_controller, hardware_manager.get_load_cell(), 
                           hardware_manager.get_grinder(), &grind_logger);
    
    LOG_BLE("✅ Task module dependencies initialized\n");
    
    // Initialize TaskManager with hardware and system interfaces
    LOG_BLE("[STARTUP] Initializing FreeRTOS Task Architecture...\n");
    bool task_init_success = task_manager.init(&hardware_manager, &state_machine, &profile_controller, 
                                              &grind_controller, &bluetooth_manager, &ui_manager);
    
    if (!task_init_success) {
        LOG_BLE("ERROR: Failed to initialize TaskManager - system cannot start\n");
        while (true) {
            delay(1000); // Halt system if task initialization fails
        }
    }
    
    LOG_BLE("✅ TaskManager initialized successfully\n");
    
    // Initialize remaining task modules that depend on TaskManager queues
    file_io_task.init(task_manager.get_file_io_queue());
    
    LOG_BLE("✅ All task modules initialized\n");

    // Last: WiFi starts only when credentials are stored and never blocks boot (tick() runs in the Bluetooth task)
    wifi_manager.begin();
    log_internal_heap("after WiFi begin");

#if DEBUG_WIFI_HEAP_SPIKE
    wifi_heap_spike_start();
#endif

#if DEBUG_BLE_ENABLE_STRESS
    ble_enable_stress_start();
#endif
}

void loop() {


#if SYS_ENABLE_REALTIME_HEARTBEAT
    // Core 1 main loop timing (monitor main loop health)
    uint32_t cycle_start_time = millis();
    core1_cycle_count_10s++;
    if (core1_last_heartbeat_time == 0) core1_last_heartbeat_time = cycle_start_time;
#endif

    // Update device uptime statistics every 15 minutes, reporting back in hours
    static uint32_t last_uptime_update = 0;
    static uint32_t pending_uptime_minutes = 0;
    uint32_t current_time = millis();
    if (last_uptime_update == 0) {
        last_uptime_update = current_time;
    }
    constexpr uint32_t kUptimeIntervalMs = 900000; // 15 minutes
    constexpr uint32_t kUptimeIntervalMinutes = 15;
    uint32_t elapsed_ms = current_time - last_uptime_update;
    if (elapsed_ms >= kUptimeIntervalMs) {
        uint32_t intervals = elapsed_ms / kUptimeIntervalMs;
        pending_uptime_minutes += intervals * kUptimeIntervalMinutes;
        last_uptime_update += intervals * kUptimeIntervalMs;

        if (pending_uptime_minutes > 0) {
            statistics_manager.update_uptime(pending_uptime_minutes);
            pending_uptime_minutes = 0;
        }
    }
    
    // Hardware task suspend/resume for OTA is owned by OTAHandler (start_ota / cleanup_failed_ota)

    // UI events are now processed inside the UI render FreeRTOS task
    // to serialize all LVGL updates on a single thread.
    
#if SYS_ENABLE_REALTIME_HEARTBEAT
    // Calculate Core 1 main loop timing
    uint32_t cycle_end_time = millis();
    uint32_t cycle_duration = cycle_end_time - cycle_start_time;
    core1_cycle_time_sum_ms += cycle_duration;
    if (cycle_duration < core1_cycle_time_min_ms) core1_cycle_time_min_ms = cycle_duration;
    if (cycle_duration > core1_cycle_time_max_ms) core1_cycle_time_max_ms = cycle_duration;
    
    // Core 1 Main Loop Heartbeat - Monitor main loop health (every 10 seconds)
    if (cycle_end_time - core1_last_heartbeat_time >= SYS_REALTIME_HEARTBEAT_INTERVAL_MS) {
        uint32_t avg_cycle_time = core1_cycle_count_10s > 0 ? core1_cycle_time_sum_ms / core1_cycle_count_10s : 0;
        
        // Get system states
        bool is_grinding = grind_controller.is_active();
        const char* ble_state = bluetooth_manager.is_enabled() ? 
                               (bluetooth_manager.is_connected() ? "CONN" : "ADV") : "OFF";
        const char* grinder_state = is_grinding ? "ACTIVE" : "IDLE";
        const char* tasks_status = task_manager.are_tasks_healthy() ? "HEALTHY" : "ERROR";
        size_t free_heap_kb = ESP.getFreeHeap() / 1024;
        
        LOG_BLE("[%lums MAIN_LOOP_HEARTBEAT] Cycles: %lu/10s | Avg: %lums (%lu-%lums) | Tasks: %s | BLE: %s | Grinder: %s | Mem: %zuKB | Build: #%d\n",
               millis(), core1_cycle_count_10s, avg_cycle_time, core1_cycle_time_min_ms, core1_cycle_time_max_ms,
               tasks_status, ble_state, grinder_state, free_heap_kb, BUILD_NUMBER);
        
        // Reset Core 1 metrics for next interval
        core1_cycle_count_10s = 0;
        core1_cycle_time_sum_ms = 0;
        core1_cycle_time_min_ms = UINT32_MAX;
        core1_cycle_time_max_ms = 0;
        core1_last_heartbeat_time = cycle_end_time;
    }
#endif
    
    // The main loop now runs much lighter since FreeRTOS tasks handle all the heavy work
    // Just yield to allow FreeRTOS scheduler to run other tasks efficiently
    vTaskDelay(pdMS_TO_TICKS(10)); // Small delay to prevent starving other tasks
}
