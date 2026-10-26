#pragma once

/*
 * LVGL assert handler (LV_ASSERT_CUSTOM_INCLUDE in lv_conf.h).
 *
 * LVGL prints the failed assertion through its log first, then runs this statement. The LVGL default is
 * `while(1);`, which would freeze the UI task (Core 1: UI, BLE and WiFi) with no watchdog reset. abort() reboots
 * the device instead, and an unconfirmed new image is rolled back by boot_health.
 *
 * Included from C and C++ translation units, so keep it to plain C.
 */
#include <stdlib.h>

#define LV_ASSERT_HANDLER abort();
