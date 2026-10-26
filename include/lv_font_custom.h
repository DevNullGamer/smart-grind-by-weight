#pragma once

/*
 * Custom fonts declared to LVGL (LV_FONT_CUSTOM_INCLUDE in lv_conf.h). The sources live in src/font/.
 * Included from C and C++ translation units, so keep it to plain C.
 */
#define LV_FONT_CUSTOM_DECLARE \
    LV_FONT_DECLARE(lv_font_montserrat_56) \
    LV_FONT_DECLARE(lv_font_montserrat_60)
