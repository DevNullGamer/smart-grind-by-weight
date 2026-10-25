#include "grinder_deadman_test.h"
#include "../config/constants.h"

#if DEBUG_GRINDER_DEADMAN_AUTORUN

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>

namespace {

constexpr uint32_t kBootDelayMs = 6000;      // Let the UI and diagnostics come up first
constexpr uint32_t kPinSampleMs = 1000;      // Mid-run sample, expected HIGH
constexpr uint32_t kTripWaitMs = 3000 + 2000;  // 3 s test deadman + margin
constexpr uint32_t kPollMs = 50;

void deadman_test_task(void* arg) {
    Grinder* grinder = static_cast<Grinder*>(arg);
    LOG_BLE("[DEADMAN-TEST] WARNING: motor will run for ~3 s in 6 s, disconnect the grinder from mains\n");
    vTaskDelay(pdMS_TO_TICKS(kBootDelayMs));

    if (!grinder->is_initialized()) {
        LOG_BLE("[DEADMAN-TEST] skipped: grinder not initialized (board not established or motor disabled)\n");
        vTaskDelete(nullptr);
        return;
    }
    if (grinder->is_grinding() || grinder->is_locked_out() || grinder->deadman_tripped_since_boot()) {
        LOG_BLE("[DEADMAN-TEST] skipped: grinder busy, locked out or already tripped\n");
        vTaskDelete(nullptr);
        return;
    }

    const int pin = board().motor_pin;
    gpio_input_enable((gpio_num_t)pin);  // An RMT output pad reads 0 without the input buffer
    LOG_BLE("[DEADMAN-TEST] starting continuous run (deadman fires in 3 s)\n");
    grinder->start();  // The same call a grind uses; one run only

    if (!grinder->is_grinding()) {
        LOG_BLE("[DEADMAN-TEST] result: FAILED: start refused (grinder not running)\n");
        vTaskDelete(nullptr);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(kPinSampleMs));
    const bool mid_high = digitalRead(pin) == HIGH;
    LOG_BLE("[DEADMAN-TEST] mid-run: pin %d %s\n", pin, mid_high ? "HIGH" : "LOW");

    uint32_t waited_ms = kPinSampleMs;
    while (!grinder->deadman_tripped_since_boot() && waited_ms < kTripWaitMs) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        waited_ms += kPollMs;
    }
    vTaskDelay(pdMS_TO_TICKS(200));  // Let the trip handler finish

    // Read-only checks: deadman_tripped_since_boot() is what diagnostics turns into MOTOR_FAULT.
    // The consume flag is left alone (the grind controller owns it).
    const bool tripped = grinder->deadman_tripped_since_boot();
    const bool pin_low = digitalRead(pin) == LOW;
    const bool stopped = !grinder->is_grinding();
    const bool pass = mid_high && tripped && pin_low && stopped;
    if (pass) {
        LOG_BLE("[DEADMAN-TEST] result: deadman tripped, fault latched, pin LOW\n");
    } else if (!mid_high) {
        LOG_BLE("[DEADMAN-TEST] result: INCONCLUSIVE: pin read did not see HIGH while running\n");
    } else {
        LOG_BLE("[DEADMAN-TEST] result: FAILED: mid_high=%d tripped=%d pin_low=%d grinding_cleared=%d\n",
                (int)mid_high, (int)tripped, (int)pin_low, (int)stopped);
    }
    if (!pass) {
        // A failed or inconclusive test must not leave the motor running
        grinder->stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        LOG_BLE("[DEADMAN-TEST] cleanup: stop() issued, pin %d %s\n", pin, digitalRead(pin) ? "HIGH" : "LOW");
    }
    vTaskDelete(nullptr);
}

}  // namespace

void grinder_deadman_test_start(Grinder* grinder) {
    xTaskCreate(deadman_test_task, "deadman_test", 8192, grinder, 1, nullptr);
}

#endif  // DEBUG_GRINDER_DEADMAN_AUTORUN
