#include "update_guard.h"
#include "../config/constants.h"
#include "../hardware/touch_driver.h"
#include "../hardware/hardware_manager.h"
#include "../controllers/grind_controller.h"
#include "../tasks/task_manager.h"
#include "../bluetooth/manager.h"
#include <Arduino.h>
#include <esp_task_wdt.h>

namespace {
// Task watchdog timeout in effect before an update extends it; same fallback as the BLE OTA handler
#ifdef CONFIG_ESP_TASK_WDT_TIMEOUT_S
constexpr uint32_t kNormalWdtTimeoutMs = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000;
#else
constexpr uint32_t kNormalWdtTimeoutMs = 5000;
#endif
}  // namespace

std::atomic<bool> UpdateGuard::any_held_{false};

const char* UpdateGuard::result_text(Result result) {
    switch (result) {
        case Result::OK: return "ok";
        case Result::ALREADY_HELD: return "Update refused: another update running";
        case Result::BLE_UPDATE_ACTIVE: return "Update refused: Bluetooth update running";
        case Result::GRINDER_BUSY: return "Update refused: grinder busy";
    }
    return "Update refused";
}

UpdateGuard::Result UpdateGuard::acquire() {
    refusal_reason_ = "";
    // Claim the system-wide slot first; every refusal below gives it back
    bool expected = false;
    if (owns_.load() || !any_held_.compare_exchange_strong(expected, true)) {
        refusal_reason_ = result_text(Result::ALREADY_HELD);
        return Result::ALREADY_HELD;
    }
    auto refuse = [&](Result result) {
        any_held_.store(false);
        refusal_reason_ = result_text(result);
        return result;
    };

    extern BluetoothManager g_bluetooth_manager;
    if (g_bluetooth_manager.is_updating()) {
        LOG_BLE("UPDATE: Refusing update - BLE OTA in progress\n");
        return refuse(Result::BLE_UPDATE_ACTIVE);
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
        LOG_BLE("UPDATE: Refusing update - grinder busy (grind=%d autotune=%d motor=%d)\n",
                grind_controller.is_grind_running(), autotune->is_active(), grinder->is_grinding());
        return refuse(Result::GRINDER_BUSY);
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

    // Flash-intensive work can starve other tasks: extend the task watchdog for the update
    LOG_BLE("UPDATE: Reconfiguring task watchdog timer (%lus timeout)\n", (unsigned long)(UPDATE_WDT_TIMEOUT_MS / 1000));
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = UPDATE_WDT_TIMEOUT_MS,
        .idle_core_mask = (1 << 0) | (1 << 1), // Watch idle tasks on both cores
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&wdt_config);

    LOG_BLE("UPDATE: Suspending hardware tasks...\n");
    task_manager.suspend_hardware_tasks();
    hardware_manager.get_display()->get_touch_driver()->disable();

    owns_.store(true);
    note_activity();
    return Result::OK;
}

// Order matters: tasks must run again (and reset the watchdog) before the 5 s timeout is restored,
// otherwise the watchdog panics.
void UpdateGuard::release() {
    if (!owns_.exchange(false)) return;
    LOG_BLE("UPDATE: Releasing update guard\n");

    task_manager.resume_hardware_tasks();

    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = kNormalWdtTimeoutMs,
        .idle_core_mask = (1 << 0), // Normal operation watches the idle task of CPU0 only
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&wdt_config);

    extern HardwareManager hardware_manager;
    hardware_manager.get_display()->get_touch_driver()->enable();
    // Normally done by resume_hardware_tasks(); explicit so it also holds if tasks weren't suspended
    hardware_manager.get_grinder()->unlock();

    any_held_.store(false);
}

void UpdateGuard::note_activity() {
    last_activity_ms_.store(millis());
}

bool UpdateGuard::check_inactivity_timeout() {
    if (!owns_.load()) return false;
    if (millis() - last_activity_ms_.load() < UPDATE_INACTIVITY_TIMEOUT_MS) return false;
    LOG_BLE("UPDATE: No data for %d ms - releasing update guard\n", UPDATE_INACTIVITY_TIMEOUT_MS);
    release();
    return true;
}
