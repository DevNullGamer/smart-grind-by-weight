#pragma once

//==============================================================================
// DEBUG AND DEVELOPMENT CONFIGURATION
//==============================================================================
// This file contains all debug-related configuration constants including
// mock hardware settings, debug output controls, and performance monitoring.

//------------------------------------------------------------------------------
// MOST IMPORTANT DEBUG PARAMETERS
//------------------------------------------------------------------------------
// Mock hardware (most critical for testing vs production)
#ifndef DEBUG_ENABLE_LOADCELL_MOCK
    #define DEBUG_ENABLE_LOADCELL_MOCK 0                                              // Default: use physical HX711, override with build flag
#endif

// UI visual feedback
#ifndef DEBUG_ENABLE_GRINDER_BACKGROUND_INDICATOR
    #define DEBUG_ENABLE_GRINDER_BACKGROUND_INDICATOR 0                             // Default: disabled, override with build flag
#endif

// RMT pulse accuracy self-test (measurement only). When 1, setup() pulses the motor output
// at a range of durations and reports the measured vs requested time. DISCONNECT THE GRINDER
// FROM MAINS before enabling. Override with build flag.
#ifndef DEBUG_RMT_PULSE_SELFTEST
    #define DEBUG_RMT_PULSE_SELFTEST 0
#endif

// WiFi heap spike (measurement only). When 1, setup() starts a one-shot task ~30 s after boot that
// measures internal heap with WiFi, httpd, mDNS and an HTTPS pull alongside BLE. Needs
// include/spike_secrets.h. Override with build flag.
#ifndef DEBUG_WIFI_HEAP_SPIKE
    #define DEBUG_WIFI_HEAP_SPIKE 0
#endif

// BLE enable/disable stress experiment (measurement only). When 1, setup() starts a task that cycles
// BluetoothManager::disable()+enable() 150 times from a Core 1 task and then from a Core 0 task, logging
// "[BLESTRESS]" lines and surviving panics via RTC memory. Override with build flag.
#ifndef DEBUG_BLE_ENABLE_STRESS
    #define DEBUG_BLE_ENABLE_STRESS 0
#endif

// Restore the old hard BLE teardown (BLEDevice::deinit in disable(), full re-init in enable()) for
// comparison measurements. Normal builds soft-disable: the stack stays initialised because deinit/init
// leaks ~11 KB of internal heap per cycle. Override with build flag.
#ifndef DEBUG_BLE_HARD_DEINIT
    #define DEBUG_BLE_HARD_DEINIT 0
#endif

// Dev-only WiFi credentials. When 1, boot stores SPIKE_WIFI_SSID / SPIKE_WIFI_PASSWORD from the gitignored
// include/spike_secrets.h into NVS if no credentials are stored, and enables the push endpoint flag.
// The built image then contains the password in plaintext: do not share it. Override with build flag.
#ifndef DEBUG_WIFI_DEV_CREDS
    #define DEBUG_WIFI_DEV_CREDS 0
#endif

// Motor deadman test. When 1, the continuous-run deadman fires 3 s after motor start instead of
// GRIND_TIMEOUT_SEC + margin, so any continuous run longer than 3 s exercises the trip path.
// Override with build flag.
#ifndef DEBUG_GRINDER_DEADMAN_TEST
    #define DEBUG_GRINDER_DEADMAN_TEST 0
#endif

// Core debug logging settings
#define DEBUG_SERIAL_OUTPUT 1                                             // Enable serial debug output
#define DEBUG_GRIND_CONTROLLER 0                                          // Enable detailed grind debugging
#define DEBUG_LOAD_CELL 0                                                 // Enable detailed load cell debugging  
#define DEBUG_UI_SYSTEM 0                                                 // Enable detailed UI debugging
#define DEBUG_CALIBRATION 0                                               // Enable detailed calibration debugging
#define DEBUG_WEIGHT_SETTLING 0                                           // Enable weight settling debugging

// Touch I2C polling creates expected NACKs when the controller has no data.
// Suppress noisy low-level logs while keeping higher-level drivers quiet.
#define DEBUG_SUPPRESS_TOUCH_I2C_ERRORS 1

//------------------------------------------------------------------------------
// MOCK HARDWARE DETAILED CONFIGURATION
//------------------------------------------------------------------------------
#define DEBUG_MOCK_FLOW_RATE_GPS 1.9f                                             // Simulated continuous flow rate in grams per second
#define DEBUG_MOCK_CAL_FACTOR -7050.0f                                            // Fixed calibration factor used during mocking
#define DEBUG_MOCK_BASELINE_RAW 0x700000                                          // Baseline raw count around mid-scale for tare offset
#define DEBUG_MOCK_IDLE_NOISE_RAW 60.0f                                           // Peak raw noise when idle (counts)
#define DEBUG_MOCK_GRIND_NOISE_RAW 400.0f                                         // Peak raw noise when grinding (counts)
#define DEBUG_MOCK_FLOW_RAMP_MS 350                                               // Ramp duration for simulated flow transitions
#define DEBUG_MOCK_START_DELAY_MS 500                                             // Delay from motor start command to weight increase (continuous grind)
#define DEBUG_MOCK_STOP_DELAY_MS 400                                              // Delay from motor stop command to weight stop
#define DEBUG_MOCK_MOTOR_LATENCY_MS 42.0f                                         // Hidden minimum pulse duration to produce grounds (for auto-tune testing)

