#include "ble_enable_stress.h"
#include "../config/constants.h"

#if DEBUG_BLE_ENABLE_STRESS

// Measurement-only experiment: BluetoothManager::disable()+enable() cycles, first from a Core 1 task, then
// from a task pinned to Core 0. Each cycle logs the internal heap and the stack high-water marks of the
// ipc0/ipc1 tasks (the 1 KB tasks that run BT controller interrupt allocation).
//
// Crash survival: the run state is kept in RTC_NOINIT memory (survives a panic reset, not a power cycle).
// "in_progress" is set before each cycle and cleared after it, so a panic leaves it set and the next boot
// reports which cycle/core died and continues with the following cycle. Only panic/watchdog resets continue
// a run; any other reset reason (power-on, flash, reset button) starts a fresh run.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "../bluetooth/manager.h"

extern BluetoothManager g_bluetooth_manager;

namespace {

constexpr uint32_t kStartDelayMs = 20000;
constexpr int kCyclesPerPhase = 150;
constexpr uint32_t kSettleMs = 3000;
constexpr uint32_t kBleTimeoutMs = 30UL * 60UL * 1000UL;  // keep the auto-disable timeout out of the way
constexpr uint32_t kRtcMagic = 0xB1E57E55;
constexpr int kPhaseCore1 = 0;
constexpr int kPhaseCore0 = 1;
constexpr int kPhaseDone = 2;
constexpr uint32_t kCore1StackBytes = 8192;
constexpr uint32_t kCore0StackBytes = 6144;  // >= 4 KB as required for the Core 0 phase

struct StressState {
    uint32_t magic;
    int phase;            // kPhaseCore1 / kPhaseCore0 / kPhaseDone
    int last_cycle;     // last cycle that was started in this phase (1-based, 0 = none yet)
    int in_progress;    // 1 while a cycle is running (still 1 after a panic)
    int crashes[2];     // crashes seen per phase
    int min_ipc0_hwm;   // lowest ipc0 stack high-water seen over the whole run (bytes, -1 = unknown)
    int min_free;      // lowest internal free heap seen at the end of a cycle
};

RTC_NOINIT_ATTR StressState g_state;

const char* phase_name(int phase) {
    return phase == kPhaseCore1 ? "Core 1 task" : (phase == kPhaseCore0 ? "Core 0 task" : "done");
}

bool is_crash_reset(esp_reset_reason_t rr) {
    return rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT;
}

// Stack high-water mark (bytes, minimum free ever) of a named task; -1 when the task is not found
int stack_hwm(const char* name) {
    TaskHandle_t h = xTaskGetHandle(name);
    if (!h) return -1;
    return static_cast<int>(uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t));
}

void log_state(const char* what, int cycle) {
    int ipc0 = stack_hwm("ipc0");
    int ipc1 = stack_hwm("ipc1");
    Serial.printf("[BLESTRESS] %s phase=%s cycle=%d/%d core=%d free=%u largest=%u ipc0_hwm=%d ipc1_hwm=%d ble=%s\n",
                  what, phase_name(g_state.phase), cycle, kCyclesPerPhase, xPortGetCoreID(),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                  ipc0, ipc1, g_bluetooth_manager.is_enabled() ? "on" : "off");
}

void stress_task(void* arg);

void start_phase_task(int phase) {
    const bool on_core0 = phase == kPhaseCore0;
    BaseType_t ok = xTaskCreatePinnedToCore(stress_task, on_core0 ? "BleStressC0" : "BleStressC1",
                                            on_core0 ? kCore0StackBytes : kCore1StackBytes, nullptr,
                                            on_core0 ? 2 : 3, nullptr, on_core0 ? 0 : 1);
    Serial.printf("[BLESTRESS] %s task %s\n", phase_name(phase), ok == pdPASS ? "created" : "create FAILED");
}

void print_summary() {
    Serial.printf("[BLESTRESS] SUMMARY: crashes core1-phase=%d core0-phase=%d, lowest ipc0 stack high-water=%d bytes, "
                  "lowest internal free=%d bytes\n",
                  g_state.crashes[kPhaseCore1], g_state.crashes[kPhaseCore0], g_state.min_ipc0_hwm, g_state.min_free);
    Serial.println("[BLESTRESS] RUN COMPLETE (a reset/flash starts a new run)");
}

void stress_task(void*) {
    const int phase = g_state.phase;
    BluetoothManager& ble = g_bluetooth_manager;
    for (int cycle = g_state.last_cycle + 1; cycle <= kCyclesPerPhase; cycle++) {
        g_state.last_cycle = cycle;
        g_state.in_progress = 1;
        log_state("cycle begin", cycle);

        if (ble.is_enabled()) ble.disable();
        log_state("after disable", cycle);
        ble.enable(kBleTimeoutMs);
        log_state("after enable", cycle);

        int ipc0 = stack_hwm("ipc0");
        int free_now = static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        if (ipc0 >= 0 && (g_state.min_ipc0_hwm < 0 || ipc0 < g_state.min_ipc0_hwm)) g_state.min_ipc0_hwm = ipc0;
        if (g_state.min_free < 0 || free_now < g_state.min_free) g_state.min_free = free_now;
        if (!ble.is_enabled()) Serial.printf("[BLESTRESS] WARNING cycle %d: BLE did not come up\n", cycle);

        g_state.in_progress = 0;
        Serial.printf("[BLESTRESS] cycle %d OK free=%d\n", cycle, free_now);
        vTaskDelay(pdMS_TO_TICKS(kSettleMs));
    }

    if (phase == kPhaseCore1) {
        Serial.println("[BLESTRESS] Core 1 phase complete, starting Core 0 phase");
        g_state.phase = kPhaseCore0;
        g_state.last_cycle = 0;
        start_phase_task(kPhaseCore0);
    } else {
        g_state.phase = kPhaseDone;
        print_summary();
    }
    vTaskDelete(nullptr);
}

void init_or_resume_state() {
    const esp_reset_reason_t rr = esp_reset_reason();
    const bool valid = g_state.magic == kRtcMagic && g_state.phase >= kPhaseCore1 && g_state.phase <= kPhaseDone;
    if (valid && is_crash_reset(rr)) {
        if (g_state.in_progress) {
            const int phase = g_state.phase;
            const int cycle = g_state.last_cycle;
            g_state.crashes[phase]++;
            g_state.in_progress = 0;
            Serial.printf("[BLESTRESS] CRASH DETECTED: crash during cycle %d from core %d (%s), reset reason %d; "
                          "crashes so far: core1-phase=%d core0-phase=%d. Continuing with the next cycle\n",
                          cycle, phase == kPhaseCore1 ? 1 : 0, phase_name(phase), static_cast<int>(rr),
                          g_state.crashes[kPhaseCore1], g_state.crashes[kPhaseCore0]);
            if (cycle >= kCyclesPerPhase) {  // the crashed cycle was the last one of its phase
                g_state.phase = phase + 1;
                g_state.last_cycle = 0;
            }
        } else {
            Serial.printf("[BLESTRESS] panic reset (reason %d) outside a stress cycle, resuming\n", static_cast<int>(rr));
        }
        return;
    }
    g_state.magic = kRtcMagic;
    g_state.phase = kPhaseCore1;
    g_state.last_cycle = 0;
    g_state.in_progress = 0;
    g_state.crashes[kPhaseCore1] = 0;
    g_state.crashes[kPhaseCore0] = 0;
    g_state.min_ipc0_hwm = -1;
    g_state.min_free = -1;
    Serial.printf("[BLESTRESS] fresh run (reset reason %d)\n", static_cast<int>(rr));
}

void launcher_task(void*) {
    Serial.printf("[BLESTRESS] starting in %u s: %d cycles Core 1 task, then %d cycles Core 0 task, %u ms settle\n",
                  static_cast<unsigned>(kStartDelayMs / 1000), kCyclesPerPhase, kCyclesPerPhase,
                  static_cast<unsigned>(kSettleMs));
    Serial.printf("[BLESTRESS] config: DEBUG_BLE_HARD_DEINIT=%d\n", DEBUG_BLE_HARD_DEINIT);
    vTaskDelay(pdMS_TO_TICKS(kStartDelayMs));
    if (g_state.phase == kPhaseDone) {
        print_summary();
    } else {
        start_phase_task(g_state.phase);
    }
    vTaskDelete(nullptr);
}

}  // namespace

void ble_enable_stress_start() {
    init_or_resume_state();
    BaseType_t ok = xTaskCreatePinnedToCore(launcher_task, "BleStressInit", 3072, nullptr, 1, nullptr, 1);
    Serial.printf("[BLESTRESS] launcher %s\n", ok == pdPASS ? "created" : "create FAILED");
}

#endif  // DEBUG_BLE_ENABLE_STRESS
