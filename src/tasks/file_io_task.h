#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../config/constants.h"

/**
 * FileIOTask - Dedicated File I/O and Storage Operations
 *
 * A focused, low-priority task that runs all file system work without blocking
 * the real-time tasks. TaskManager creates the FreeRTOS task and calls task_impl().
 *
 * Responsibilities:
 * - Drain the GrindController flash-operation and log queues (grind session logging)
 * - File system health check and recovery
 *
 * Architecture:
 * - Runs on Core 1 at low priority (1)
 * - Uses vTaskDelayUntil for predictable timing
 * - All blocking LittleFS operations isolated here
 */
class FileIOTask {
private:
    volatile bool task_running;

    // File I/O state
    bool filesystem_available;
    uint32_t last_filesystem_check_time;

    // Performance monitoring
    uint32_t cycle_count;
    uint32_t cycle_time_sum_ms;
    uint32_t cycle_time_min_ms;
    uint32_t cycle_time_max_ms;
    uint32_t last_heartbeat_time;

public:
    FileIOTask();

    // Initialization (mounts the file system)
    void init();

    bool is_running() const { return task_running; }

    // File system status
    bool is_filesystem_available() const { return filesystem_available; }

    // Performance monitoring
    uint32_t get_cycle_count() const { return cycle_count; }
    void print_performance_stats() const;

    // Task implementation (public for TaskManager access)
    void task_impl();

private:
    // File system management
    void check_filesystem_health();
    bool validate_filesystem_access();
    void perform_filesystem_maintenance();

    // Performance tracking
    void record_timing(uint32_t start_time, uint32_t end_time);
    void print_heartbeat() const;
    void reset_performance_metrics();

    // Error handling
    void handle_filesystem_error();
    bool attempt_filesystem_recovery();
};

// Global instance
extern FileIOTask file_io_task;
