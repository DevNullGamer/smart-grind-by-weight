#include "file_io_task.h"
#include "../controllers/grind_controller.h"
#include "../config/constants.h"
#include <Arduino.h>
#include <LittleFS.h>

// Global instance
FileIOTask file_io_task;

FileIOTask::FileIOTask() {
    task_running = false;
    
    // Initialize file I/O state
    filesystem_available = false;
    last_filesystem_check_time = 0;
    
    // Initialize performance metrics
    cycle_count = 0;
    cycle_time_sum_ms = 0;
    cycle_time_min_ms = UINT32_MAX;
    cycle_time_max_ms = 0;
    last_heartbeat_time = 0;
}

void FileIOTask::init() {
    // Check initial filesystem availability
    filesystem_available = LittleFS.begin(true);
    if (filesystem_available) {
        LOG_BLE("FileIOTask: LittleFS filesystem available\n");
    } else {
        LOG_BLE("FileIOTask: LittleFS filesystem unavailable\n");
    }
}

void FileIOTask::task_impl() {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(SYS_TASK_FILE_IO_INTERVAL_MS);
    
    LOG_BLE("FileIOTask started on Core %d at %dHz\n", 
            xPortGetCoreID(), 1000 / SYS_TASK_FILE_IO_INTERVAL_MS);
    
    // Set the run flag so the loop executes.
    task_running = true;
    
    // Reset performance metrics
    reset_performance_metrics();
    
    // Main file I/O processing loop
    while (task_running) {
        uint32_t cycle_start_time = millis();
        
        // Drain the GrindController queues here so flash operations
        // (start/end session) and queued log lines run on Core 1 in this low-priority task
        extern GrindController grind_controller;
        grind_controller.process_queued_flash_operations();
        grind_controller.process_queued_log_messages();

        // Periodic filesystem health check
        if (cycle_start_time - last_filesystem_check_time >= 30000) { // Every 30 seconds
            check_filesystem_health();
            last_filesystem_check_time = cycle_start_time;
        }
        
        uint32_t cycle_end_time = millis();
        
        // Record performance metrics
        record_timing(cycle_start_time, cycle_end_time);
        
        // Use vTaskDelayUntil for predictable timing (eliminates busy-wait)
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
    
    LOG_BLE("FileIOTask: I/O processing loop stopped\n");
}

void FileIOTask::check_filesystem_health() {
    bool fs_available = validate_filesystem_access();
    
    if (fs_available != filesystem_available) {
        filesystem_available = fs_available;
        LOG_BLE("FileIOTask: Filesystem availability changed to %s\n", fs_available ? "AVAILABLE" : "UNAVAILABLE");
        
        if (!fs_available) {
            handle_filesystem_error();
        }
    }
    
    if (filesystem_available) {
        perform_filesystem_maintenance();
    }
}

bool FileIOTask::validate_filesystem_access() {
    // Try a simple filesystem operation to validate access
    File test_file = LittleFS.open("/test_access", "w");
    if (test_file) {
        test_file.println("test");
        test_file.close();
        LittleFS.remove("/test_access");
        return true;
    }
    return false;
}

void FileIOTask::perform_filesystem_maintenance() {
    // Placeholder for filesystem maintenance operations
    // Could include:
    // - Log rotation
    // - Old file cleanup
    // - Filesystem defragmentation
    // - Storage usage monitoring
}

void FileIOTask::handle_filesystem_error() {
    LOG_BLE("FileIOTask: Filesystem error detected\n");
    
    // Attempt filesystem recovery
    if (attempt_filesystem_recovery()) {
        LOG_BLE("FileIOTask: Filesystem recovery successful\n");
    } else {
        LOG_BLE("ERROR: FileIOTask: Filesystem recovery failed\n");
    }
}

bool FileIOTask::attempt_filesystem_recovery() {
    LOG_BLE("FileIOTask: Attempting filesystem recovery...\n");
    
    // Try to remount the filesystem
    LittleFS.end();
    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second
    
    bool recovery_success = LittleFS.begin(true);
    if (recovery_success) {
        filesystem_available = true;
    }
    
    return recovery_success;
}

void FileIOTask::record_timing(uint32_t start_time, uint32_t end_time) {
    uint32_t cycle_duration = end_time - start_time;
    
    cycle_count++;
    cycle_time_sum_ms += cycle_duration;
    
    if (cycle_duration < cycle_time_min_ms) {
        cycle_time_min_ms = cycle_duration;
    }
    if (cycle_duration > cycle_time_max_ms) {
        cycle_time_max_ms = cycle_duration;
    }
    
#if SYS_ENABLE_REALTIME_HEARTBEAT
    // Print task heartbeat every 10 seconds
    if (last_heartbeat_time == 0) {
        last_heartbeat_time = start_time;
    }
    
    if (end_time - last_heartbeat_time >= SYS_REALTIME_HEARTBEAT_INTERVAL_MS) {
        print_heartbeat();
        reset_performance_metrics();
        last_heartbeat_time = end_time;
    }
#endif
}

void FileIOTask::print_heartbeat() const {
#if SYS_ENABLE_REALTIME_HEARTBEAT
    uint32_t avg_cycle_time = cycle_count > 0 ? cycle_time_sum_ms / cycle_count : 0;
    const char* fs_status = filesystem_available ? "OK" : "ERROR";
    
    LOG_BLE("[%lums FILE_IO_HEARTBEAT] Cycles: %lu/10s | Avg: %lums (%lu-%lums) | FS: %s | Build: #%d\n",
           millis(), cycle_count, avg_cycle_time, cycle_time_min_ms, cycle_time_max_ms,
           fs_status, BUILD_NUMBER);
#endif
}

void FileIOTask::reset_performance_metrics() {
    cycle_count = 0;
    cycle_time_sum_ms = 0;
    cycle_time_min_ms = UINT32_MAX;
    cycle_time_max_ms = 0;
}

void FileIOTask::print_performance_stats() const {
    LOG_BLE("=== FileIOTask Performance ===\n");
    LOG_BLE("Task running: %s\n", task_running ? "YES" : "NO");
    LOG_BLE("Filesystem available: %s\n", filesystem_available ? "YES" : "NO");
    LOG_BLE("Cycle count: %lu\n", cycle_count);
    
    if (cycle_count > 0) {
        uint32_t avg_cycle_time = cycle_time_sum_ms / cycle_count;
        LOG_BLE("Average cycle time: %lums (%lu-%lums)\n", avg_cycle_time, cycle_time_min_ms, cycle_time_max_ms);
    }
    LOG_BLE("=============================\n");
}
