#include "ota_handler.h"
#include "../config/build_info.h"
#include "../config/logging.h"
#include "../hardware/touch_driver.h"
#include "../hardware/hardware_manager.h"
#include "../controllers/grind_controller.h"
#include "../tasks/task_manager.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <climits>

namespace {
// Task watchdog timeout in effect before an OTA extends it; restored on every failure path
#ifdef CONFIG_ESP_TASK_WDT_TIMEOUT_S
constexpr uint32_t kNormalWdtTimeoutMs = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000;
#else
constexpr uint32_t kNormalWdtTimeoutMs = 5000;
#endif
constexpr uint32_t kOtaWdtTimeoutMs = 1800000;

// Scoped take of the OTA operation mutex; a missing mutex (before init) counts as held
class OtaLock {
public:
    OtaLock(SemaphoreHandle_t mutex, TickType_t wait) : mutex_(mutex), held_(true) {
        if (mutex_) held_ = (xSemaphoreTake(mutex_, wait) == pdTRUE);
    }
    ~OtaLock() {
        if (mutex_ && held_) xSemaphoreGive(mutex_);
    }
    bool held() const { return held_; }
    OtaLock(const OtaLock&) = delete;
    OtaLock& operator=(const OtaLock&) = delete;
private:
    SemaphoreHandle_t mutex_;
    bool held_;
};
}

OTAHandler::OTAHandler()
    : ota_in_progress(false)
    , patch_size(0)
    , received_size(0)
    , current_status(BLE_OTA_IDLE)
    , current_firmware_build_number("")
    , is_full_update(false)
    , preferences(nullptr)
    , power_state(NORMAL_POWER)
    , normal_cpu_freq_mhz(BLE_NORMAL_CPU_FREQ_MHZ)
    , op_mutex(nullptr)
    , last_activity_ms(0)
    , failure_reason(nullptr) {
}

OTAHandler::~OTAHandler() {
    if (ota_in_progress) {
        abort_ota();
    }
    restore_normal_power_mode();
}

void OTAHandler::init(Preferences* prefs) {
    preferences = prefs;
    if (!op_mutex) {
        op_mutex = xSemaphoreCreateMutex();
    }
    LOG_BLE("OTA: Handler initialized\n");
    
    // Get current firmware build number
    current_firmware_build_number = String(BUILD_NUMBER);
    
    // Log initial power state
    LOG_BLE("OTA Power: Initial state - CPU: %luMHz, Power mode: %s\n",
                 (unsigned long)getCpuFrequencyMhz(),
                 (power_state == NORMAL_POWER) ? "NORMAL" : "REDUCED");
}

void OTAHandler::enable_ble_power_mode() {
    reduce_power_for_ble();
}

void OTAHandler::restore_normal_power_mode() {
    restore_normal_power();
}

void OTAHandler::reduce_power_for_ble() {
    if (power_state == BLE_REDUCED_POWER) return;
    
    // Store current CPU frequency
    normal_cpu_freq_mhz = getCpuFrequencyMhz();
    
    LOG_BLE("OTA Power: Switching from %luMHz to %dMHz\n", 
                 (unsigned long)normal_cpu_freq_mhz, BLE_REDUCED_CPU_FREQ_MHZ);
    
    // Lower CPU frequency for power savings
    if (!setCpuFrequencyMhz(BLE_REDUCED_CPU_FREQ_MHZ)) {
        LOG_BLE("OTA Power: WARNING - Failed to reduce CPU frequency\n");
    }
    
    // Verify the frequency change
    uint32_t actual_freq = getCpuFrequencyMhz();
    if (actual_freq != BLE_REDUCED_CPU_FREQ_MHZ) {
        LOG_BLE("OTA Power: WARNING - CPU frequency is %luMHz, expected %dMHz\n", 
                     (unsigned long)actual_freq, BLE_REDUCED_CPU_FREQ_MHZ);
    }
    
    power_state = BLE_REDUCED_POWER;
    LOG_BLE("OTA Power: Reduced power mode enabled\n");
}

void OTAHandler::restore_normal_power() {
    if (power_state == NORMAL_POWER) return;
    
    LOG_BLE("OTA Power: Restoring CPU to %luMHz\n", 
                 (unsigned long)normal_cpu_freq_mhz);
    
    // Restore original CPU frequency
    if (!setCpuFrequencyMhz(normal_cpu_freq_mhz)) {
        LOG_BLE("OTA Power: WARNING - Failed to restore CPU frequency\n");
        // Try to set to default frequency as fallback
        if (!setCpuFrequencyMhz(BLE_NORMAL_CPU_FREQ_MHZ)) {
            LOG_BLE("OTA Power: ERROR - Failed to set fallback CPU frequency\n");
        }
    }
    
    // Verify the frequency change
    uint32_t actual_freq = getCpuFrequencyMhz();
    if (actual_freq != normal_cpu_freq_mhz) {
        LOG_BLE("OTA Power: WARNING - CPU frequency is %luMHz, expected %luMHz\n", 
                     (unsigned long)actual_freq, (unsigned long)normal_cpu_freq_mhz);
    }
    
    power_state = NORMAL_POWER;
    LOG_BLE("OTA Power: Normal power mode restored\n");
}

bool OTAHandler::start_ota(uint32_t size, const String& expected_build_number, bool is_full_update, const String& expected_firmware_version) {
    OtaLock lock(op_mutex, portMAX_DELAY);
    return start_ota_locked(size, expected_build_number, is_full_update, expected_firmware_version);
}

bool OTAHandler::start_ota_locked(uint32_t size, const String& expected_build_number, bool is_full_update, const String& expected_firmware_version) {
    LOG_OTA_DEBUG("start_ota() called - size=%lu, build=%s, full=%d\n",
                  (unsigned long)size, expected_build_number.c_str(), is_full_update);

    failure_reason = nullptr;

    if (ota_in_progress) {
        LOG_BLE("OTA: Update already in progress\n");
        LOG_OTA_DEBUG("start_ota() FAILED - already in progress\n");
        return false;
    }

    // Validate the announced size before touching anything (nothing to clean up on refusal)
    const esp_partition_t* patch_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "patch");
    if (size == 0 || size > (uint32_t)INT_MAX || !patch_partition || size > patch_partition->size) {
        LOG_BLE("OTA: Refusing update - invalid patch size %lu (patch partition %lu)\n",
                (unsigned long)size, patch_partition ? (unsigned long)patch_partition->size : 0UL);
        failure_reason = "Update refused: invalid size";
        current_status = BLE_OTA_ERROR;
        return false;
    }

    // Refuse without side effects while anything is driving the motor: lock_out() would cut it
    extern HardwareManager hardware_manager;
    extern GrindController grind_controller;
    Grinder* grinder = hardware_manager.get_grinder();
    AutoTuneController* autotune = hardware_manager.get_autotune_controller();
    auto grinder_busy = [&]() {
        return grind_controller.is_grind_running() || autotune->is_active() || grinder->is_grinding();
    };
    auto refuse_busy = [&]() {
        LOG_BLE("OTA: Refusing update - grinder busy (grind=%d autotune=%d motor=%d)\n",
                grind_controller.is_grind_running(), autotune->is_active(), grinder->is_grinding());
        failure_reason = "Update refused: grinder busy";
        current_status = BLE_OTA_ERROR;
        return false;
    };
    if (grinder_busy()) {
        return refuse_busy();
    }

    // Block new starts, then re-check: a grind may have started between the check and the lock
    grinder->lock_out();
    if (grinder_busy()) {
        grinder->unlock();
        return refuse_busy();
    }

    patch_size = size;
    received_size = 0;
    this->is_full_update = is_full_update;

    LOG_BLE("OTA: Starting %s update (%lu KB)\n", is_full_update ? "full" : "delta", (unsigned long)patch_size / 1024);
    LOG_OTA_DEBUG("patch_size=%lu, received_size=%lu, is_full_update=%d\n",
                  (unsigned long)patch_size, (unsigned long)received_size, this->is_full_update);

    // Reconfigure task watchdog for OTA process with extended timeout
    // This is a CPU and flash-intensive operation that can starve other tasks
    LOG_BLE("OTA: Reconfiguring task watchdog timer for OTA process (%lus timeout)...\n",
            (unsigned long)(kOtaWdtTimeoutMs / 1000));
    LOG_OTA_DEBUG("Configuring watchdog - timeout_ms=%lu, cores=0x3\n", (unsigned long)kOtaWdtTimeoutMs);
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = kOtaWdtTimeoutMs,
        .idle_core_mask = (1 << 0) | (1 << 1), // Watch idle tasks on both cores
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&wdt_config);
    LOG_OTA_DEBUG("Watchdog reconfigured successfully\n");

    // Suspend hardware tasks to prevent watchdog timeouts during OTA
    LOG_BLE("OTA: Suspending hardware tasks...\n");
    task_manager.suspend_hardware_tasks();

    LOG_OTA_DEBUG("Calling start_update()...\n");
    if (!start_update()) {
        LOG_OTA_DEBUG("start_update() FAILED\n");
        failure_reason = "Update failed to start";
        patch_size = 0;
        cleanup_failed_ota();
        current_status = BLE_OTA_ERROR;
        return false;
    }
    LOG_OTA_DEBUG("start_update() SUCCESS\n");

    // Store expected build number and firmware version for post-reboot verification.
    // Only after the update has really started, so a refused/failed start leaves no stale keys.
    if (!expected_build_number.isEmpty() && preferences) {
        preferences->putString("new_build_nr", expected_build_number);
        LOG_OTA_DEBUG("Stored expected build number: %s\n", expected_build_number.c_str());
    } else {
        LOG_OTA_DEBUG("No expected build number to store\n");
    }

    if (!expected_firmware_version.isEmpty() && preferences) {
        preferences->putString("new_fw_ver", expected_firmware_version);
        LOG_OTA_DEBUG("Stored expected firmware version: %s\n", expected_firmware_version.c_str());
    } else if (expected_build_number.isEmpty()) {
        LOG_OTA_DEBUG("No expected firmware version to store\n");
    }

    ota_in_progress = true;
    last_activity_ms = millis();
    current_status = BLE_OTA_RECEIVING;
    LOG_OTA_DEBUG("OTA started successfully - status=BLE_OTA_RECEIVING\n");
    return true;
}

bool OTAHandler::process_data_chunk(const uint8_t* data, size_t size) {
    OtaLock lock(op_mutex, portMAX_DELAY);
    if (!ota_in_progress) {
        return false;
    }

    // Never write past the announced size (also keeps received_size <= patch_size)
    if ((uint64_t)received_size + size > patch_size) {
        LOG_BLE("OTA: Chunk exceeds announced size (%lu + %lu > %lu)\n",
                (unsigned long)received_size, (unsigned long)size, (unsigned long)patch_size);
        failure_reason = "Update aborted: too much data";
        abort_ota_locked();
        return false;
    }

    // Write patch data to patch partition
    if (delta_partition_write(&patch_writer, (const char*)data, size) != ESP_OK) {
        LOG_BLE("OTA: Patch write failed at offset %lu\n", (unsigned long)received_size);
        failure_reason = "Update aborted: write failed";
        abort_ota_locked();
        return false;
    }

    received_size += size;
    last_activity_ms = millis();

    // Progress logging every 16KB for better visibility, plus at start and end
    if (received_size % 16384 == 0 || received_size == size || received_size == patch_size) {
        LOG_BLE("OTA: Transfer %lu KB / %lu KB (%.1f%%)\n",
                     (unsigned long)received_size / 1024, (unsigned long)patch_size / 1024,
                     get_progress());
    }

    return true;
}

bool OTAHandler::complete_ota() {
    OtaLock lock(op_mutex, portMAX_DELAY);
    return complete_ota_locked();
}

bool OTAHandler::complete_ota_locked() {
    LOG_OTA_DEBUG("complete_ota() called\n");

    if (!ota_in_progress) {
        LOG_BLE("OTA: No update in progress\n");
        LOG_OTA_DEBUG("complete_ota() FAILED - no update in progress\n");
        return false;
    }

    LOG_BLE("OTA: Finalizing update...\n");
    LOG_OTA_DEBUG("patch_size=%lu, received_size=%lu\n",
                  (unsigned long)patch_size, (unsigned long)received_size);

    // Kamikaze mode: Disable all non-essential systems before flash operations
    LOG_BLE("OTA: Entering kamikaze mode - disabling non-essential systems...\n");
    LOG_OTA_DEBUG("Starting kamikaze mode shutdown sequence...\n");

    // Disable I2C operations (TouchDriver) - access through hardware_manager
    extern HardwareManager hardware_manager;
    LOG_OTA_DEBUG("Disabling TouchDriver I2C operations...\n");
    hardware_manager.get_display()->get_touch_driver()->disable();
    LOG_OTA_DEBUG("TouchDriver disabled\n");

    // Skip BLE deinitialization - causes hang in kamikaze mode
    // BLE stack will be destroyed during system restart anyway
    // BLEDevice::deinit(true);
    LOG_OTA_DEBUG("Skipping BLE deinit (causes hang) - kamikaze restart will handle cleanup\n");

    LOG_OTA_DEBUG("Calling finalize_update()...\n");
    bool success = finalize_update();
    if (success) {
        current_status = BLE_OTA_SUCCESS;
        LOG_OTA_DEBUG("finalize_update() SUCCESS\n");
        LOG_BLE("OTA: Update complete (%lu KB)\n", (unsigned long)received_size / 1024);
        LOG_BLE("OTA: Starting restart sequence...\n");

        // Restart device
        LOG_OTA_DEBUG("Flushing Serial before restart...\n");
        Serial.flush();
        delay(100);

        // Kamikaze restart - no graceful cleanup needed
        LOG_BLE("OTA: Kamikaze restart in 3...2...1\n");
        LOG_OTA_DEBUG("Final countdown before esp_restart()...\n");
        Serial.flush();
        delay(100);

        LOG_OTA_DEBUG("Calling esp_restart()...\n");
        Serial.flush();
        esp_restart();

        // Fallback restart methods
        LOG_OTA_DEBUG("esp_restart() failed, trying ESP.restart()...\n");
        Serial.flush();
        ESP.restart();

        LOG_OTA_DEBUG("ESP.restart() failed, entering infinite loop...\n");
        Serial.flush();
        while(true) delay(1000);
    } else {
        current_status = BLE_OTA_ERROR;
        failure_reason = "Update failed";
        LOG_BLE("OTA: Finalization failed\n");
        LOG_OTA_DEBUG("finalize_update() FAILED\n");

        cleanup_failed_ota();
    }

    ota_in_progress = false;
    LOG_OTA_DEBUG("complete_ota() returning %s\n", success ? "SUCCESS" : "FAILED");
    return success;
}

void OTAHandler::abort_ota() {
    OtaLock lock(op_mutex, portMAX_DELAY);
    abort_ota_locked();
}

void OTAHandler::abort_ota_locked() {
    if (ota_in_progress) {
        LOG_BLE("OTA: Aborting update\n");
        ota_in_progress = false;
        received_size = 0;
        patch_size = 0;
        current_status = BLE_OTA_ERROR;
        cleanup_failed_ota();
    }
}

bool OTAHandler::check_inactivity_timeout() {
    if (!ota_in_progress) return false;

    // Skip when a chunk, finalize or abort is running on another task
    OtaLock lock(op_mutex, 0);
    if (!lock.held() || !ota_in_progress) return false;
    if (millis() - last_activity_ms < BLE_OTA_INACTIVITY_TIMEOUT_MS) return false;

    LOG_BLE("OTA: No data for %d ms - aborting update\n", BLE_OTA_INACTIVITY_TIMEOUT_MS);
    failure_reason = "Update aborted: timed out";
    abort_ota_locked();
    return true;
}

// Undoes everything start_ota() / complete_ota() changed. Order matters: tasks must run again
// (and reset the watchdog) before the 5 s timeout is restored, otherwise the watchdog panics.
void OTAHandler::cleanup_failed_ota() {
    LOG_BLE("OTA: Cleaning up after failed update\n");

    task_manager.resume_hardware_tasks();

#ifdef CONFIG_ESP_TASK_WDT_TIMEOUT_S
    LOG_OTA_DEBUG("Restoring watchdog to %lu ms (CONFIG_ESP_TASK_WDT_TIMEOUT_S)\n", (unsigned long)kNormalWdtTimeoutMs);
#else
    LOG_OTA_DEBUG("Restoring watchdog to fallback %lu ms\n", (unsigned long)kNormalWdtTimeoutMs);
#endif
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = kNormalWdtTimeoutMs,
        .idle_core_mask = (1 << 0), // Normal operation watches the idle task of CPU0 only
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&wdt_config);

    extern HardwareManager hardware_manager;
    hardware_manager.get_display()->get_touch_driver()->enable();

    if (preferences) {
        preferences->remove("new_build_nr");
        preferences->remove("new_fw_ver");
    }

    // Normally done by resume_hardware_tasks(); explicit so it also holds if tasks weren't suspended
    hardware_manager.get_grinder()->unlock();
}

float OTAHandler::get_progress() const {
    if (patch_size == 0) return 0.0f;
    return 100.0f * received_size / patch_size;
}

bool OTAHandler::start_update() {
    // Initialize patch partition for writing
    if (delta_partition_init(&patch_writer, "patch", patch_size) != ESP_OK) {
        LOG_BLE("OTA: Failed to initialize patch partition\n");
        return false;
    }
    return true;
}

bool OTAHandler::finalize_update() {
    LOG_OTA_DEBUG("finalize_update() called\n");
    
    // Verify received size matches expected
    LOG_OTA_DEBUG("Verifying received size: expected=%lu, got=%lu\n", 
                  (unsigned long)patch_size, (unsigned long)received_size);
    if (received_size != patch_size) {
        LOG_BLE("OTA: Size mismatch - expected %lu, got %lu\n", 
                     (unsigned long)patch_size, (unsigned long)received_size);
        LOG_OTA_DEBUG("Size verification FAILED\n");
        return false;
    }
    LOG_OTA_DEBUG("Size verification SUCCESS\n");
    
    // A/B Partition Update Logic
    LOG_OTA_DEBUG("Getting running partition...\n");
    const esp_partition_t* running_partition = esp_ota_get_running_partition();
    if (!running_partition) {
        LOG_BLE("❌ Could not get running partition!\n");
        LOG_OTA_DEBUG("esp_ota_get_running_partition() FAILED\n");
        return false;
    }
    LOG_OTA_DEBUG("Running partition: %s (addr=0x%lx, size=%lu)\n", 
                  running_partition->label, (unsigned long)running_partition->address, 
                  (unsigned long)running_partition->size);

    LOG_OTA_DEBUG("Getting next update partition...\n");
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        LOG_BLE("❌ Could not find a valid OTA update partition!\n");
        LOG_OTA_DEBUG("esp_ota_get_next_update_partition() FAILED\n");
        return false;
    }
    LOG_OTA_DEBUG("Update partition: %s (addr=0x%lx, size=%lu)\n", 
                  update_partition->label, (unsigned long)update_partition->address, 
                  (unsigned long)update_partition->size);
    
    LOG_BLE("OTA Info: Running from '%s', updating to '%s'\n", 
                  running_partition->label, update_partition->label);

    // Set up delta options for the A/B update
    LOG_OTA_DEBUG("Setting up delta options...\n");
    delta_opts_t opts;
    opts.src = running_partition->label;
    opts.dest = update_partition->label;
    opts.patch = "patch";
    opts.is_full_update = this->is_full_update ? 1 : 0;
    LOG_OTA_DEBUG("Delta opts: src=%s, dest=%s, patch=%s, is_full=%d\n", 
                  opts.src, opts.dest, opts.patch, opts.is_full_update);
    
    // Apply the delta patch
    LOG_OTA_DEBUG("Calling delta_check_and_apply() with size=%lu...\n", (unsigned long)patch_size);
    Serial.flush();
    int result = delta_check_and_apply(patch_size, &opts);
    LOG_OTA_DEBUG("delta_check_and_apply() returned: %d\n", result);
    if (result < 0) {
        LOG_BLE("Delta patch failed: %s\n", delta_error_as_string(result));
        LOG_OTA_DEBUG("Delta patch FAILED with error: %s\n", delta_error_as_string(result));
        return false;
    }
    
    LOG_OTA_DEBUG("finalize_update() SUCCESS - delta patch applied\n");
    return true;
}

String OTAHandler::check_ota_failure_after_boot() {
    if (!preferences) {
        return "";
    }

    String expected_build = preferences->getString("new_build_nr", "");
    String expected_version = preferences->getString("new_fw_ver", "");

    if (expected_build.isEmpty() && expected_version.isEmpty()) {
        return "";
    }

    int current_build = BUILD_NUMBER;
    String current_version = BUILD_FIRMWARE_VERSION;

    // Web flasher sends firmware version - use that for verification (more reliable)
    if (!expected_version.isEmpty()) {
        if (expected_version != current_version) {
            LOG_BLE("OTA: Version check failed - expected v%s, got v%s\n",
                         expected_version.c_str(), current_version.c_str());
            preferences->remove("new_build_nr");
            preferences->remove("new_fw_ver");
            return expected_version;  // Return expected version for display
        } else {
            LOG_BLE("OTA: Version check passed - expected v%s, got v%s\n",
                         expected_version.c_str(), current_version.c_str());
            preferences->remove("new_build_nr");
            preferences->remove("new_fw_ver");
            return "";
        }
    }

    // Python flasher sends build number only - use that for verification
    if (!expected_build.isEmpty()) {
        int expected_build_num = expected_build.toInt();
        if (current_build != expected_build_num) {
            LOG_BLE("OTA: Build number check failed - expected #%d, got #%d\n",
                         expected_build_num, current_build);
            preferences->remove("new_build_nr");
            preferences->remove("new_fw_ver");
            return expected_build;
        } else {
            LOG_BLE("OTA: Build number check passed - expected #%d, got #%d\n",
                         expected_build_num, current_build);
            preferences->remove("new_build_nr");
            preferences->remove("new_fw_ver");
            return "";
        }
    }

    // Clean up if we get here (no verification data)
    preferences->remove("new_build_nr");
    preferences->remove("new_fw_ver");
    return "";
}
