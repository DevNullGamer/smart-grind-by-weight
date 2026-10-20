#pragma once
#include "../config/hardware.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "touch_driver.h"
#include "../config/constants.h"

class DisplayManager {
private:
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel_handle = nullptr;
    lv_display_t* pending_flush_display = nullptr;
    lv_display_t* lvgl_display;
    lv_indev_t* lvgl_input;
    lv_color_t* draw_buffer;
    // PSRAM draw buffer mode: strips are copied through two internal DMA staging
    // buffers and sent chunk by chunk. Otherwise draw_buffer itself is DMA-capable.
    uint8_t* staging_buffers[2] = {nullptr, nullptr};
    uint32_t staging_rows = 0;
    SemaphoreHandle_t transfer_done_sem = nullptr;
    TouchDriver touch_driver;

    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t buffer_size;
    bool initialized;

public:
    void init();
    void update();
    void set_brightness(float brightness);

    uint32_t get_width() const { return screen_width; }
    uint32_t get_height() const { return screen_height; }
    bool is_initialized() const { return initialized; }
    TouchDriver* get_touch_driver() { return &touch_driver; }

private:
    bool init_panel();
    void release_panel();
    void free_staging();
    bool alloc_staging();
    void flush_staged(lv_display_t* disp, const lv_area_t* area, const uint8_t* px_map);
    static void display_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);
    static bool color_transfer_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_io_event_data_t* event_data,
                                       void* user_context);
    static void display_rounder_cb(lv_event_t* e);
    static void touchpad_read_cb(lv_indev_t* indev, lv_indev_data_t* data);
    static uint32_t millis_cb();
};

extern DisplayManager* g_display_manager;
