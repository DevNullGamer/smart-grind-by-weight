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
    // Degraded display mode (board UNKNOWN): second panel on the other chip select,
    // every strip is sent to both. Only the panel that is really wired answers.
    esp_lcd_panel_io_handle_t panel_io_mirror = nullptr;
    esp_lcd_panel_handle_t panel_handle_mirror = nullptr;
    lv_display_t* pending_flush_display = nullptr;
    lv_display_t* lvgl_display = nullptr;
    lv_indev_t* lvgl_input = nullptr;
    lv_color_t* draw_buffer = nullptr;
    // PSRAM draw buffer mode: strips are copied through two internal DMA staging
    // buffers and sent chunk by chunk. Otherwise draw_buffer itself is DMA-capable.
    uint8_t* staging_buffers[2] = {nullptr, nullptr};
    uint32_t staging_rows = 0;
    SemaphoreHandle_t transfer_done_sem = nullptr;
    bool transfer_pending = false; // a queued staging transfer has not been confirmed complete
    TouchDriver touch_driver;

    uint32_t screen_width = 0;
    uint32_t screen_height = 0;
    uint32_t buffer_size = 0;
    bool initialized = false;

public:
    void init();
    void update();
    void set_brightness(float brightness);

    uint32_t get_width() const { return screen_width; }
    uint32_t get_height() const { return screen_height; }
    // Board detection: brings the panel up on cs_pin, counts edges on te_pin for
    // window_ms, then tears the panel down again. Returns -1 if bring-up fails.
    static int probe_te_edges(int8_t cs_pin, uint8_t te_pin, uint32_t window_ms);

    bool is_initialized() const { return initialized; }
    TouchDriver* get_touch_driver() { return &touch_driver; }

private:
    bool init_panel(int8_t cs_pin, int8_t mirror_cs_pin = -1);
    bool init_mirror_panel(int8_t cs_pin);
    void release_mirror_panel();
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
