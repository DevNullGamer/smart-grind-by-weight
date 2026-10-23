#pragma once

// BLE enable/disable stress experiment. Only compiled with -DDEBUG_BLE_ENABLE_STRESS=1.
// Starts its own task ~20 s after boot, cycles BluetoothManager::disable()+enable() and logs "[BLESTRESS]"
// lines to Serial. Phase and cycle counters live in RTC memory so a panic reboot continues the run and
// reports "crash during cycle N from core X". Never touches the grinder.
void ble_enable_stress_start();
