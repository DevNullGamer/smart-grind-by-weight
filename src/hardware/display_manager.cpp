#include "display_manager.h"
#include "esp_lcd_sh8601.h"
#include "../config/constants.h"
#include "../config/logging.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <driver/spi_master.h>
#include <string.h>

DisplayManager* g_display_manager = nullptr;

namespace {
constexpr spi_host_device_t kDisplaySpiHost = SPI2_HOST;

// Largest single SPI transfer: one staging chunk or one internal-buffer strip.
// Keeping this small (instead of a full frame) keeps the DMA descriptor
// allocation in scarce internal RAM minimal.
constexpr uint32_t kMaxTransferRows = HW_DISPLAY_STAGING_ROWS > HW_DISPLAY_DRAW_BUF_ROWS_INTERNAL
                                          ? HW_DISPLAY_STAGING_ROWS : HW_DISPLAY_DRAW_BUF_ROWS_INTERNAL;
constexpr size_t kMaxTransferBytes = HW_DISPLAY_WIDTH_PX * sizeof(uint16_t) * kMaxTransferRows;

volatile uint32_t g_te_edge_count = 0;
void IRAM_ATTR te_edge_isr() { g_te_edge_count = g_te_edge_count + 1; }

static const uint8_t kCmdC4[] = {0x80};
static const uint8_t kCmd35[] = {0x00};
static const uint8_t kCmd53[] = {0x20};
static const uint8_t kCmd63[] = {0xFF};
static const uint8_t kBrightnessOff[] = {0x00};
static const uint8_t kBrightnessFull[] = {0xFF};
static const sh8601_lcd_init_cmd_t kPanelInitCommands[] = {
    {0x11, nullptr, 0, 80},
    {0xC4, kCmdC4, sizeof(kCmdC4), 0},
    {0x35, kCmd35, sizeof(kCmd35), 0},
    {0x53, kCmd53, sizeof(kCmd53), 1},
    {0x63, kCmd63, sizeof(kCmd63), 1},
    {0x51, kBrightnessOff, sizeof(kBrightnessOff), 1},
    {0x29, nullptr, 0, 10},
    {0x51, kBrightnessFull, sizeof(kBrightnessFull), 0},
};

sh8601_vendor_config_t make_vendor_config() {
    sh8601_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = kPanelInitCommands;
    vendor_config.init_cmds_size = sizeof(kPanelInitCommands) / sizeof(kPanelInitCommands[0]);
    vendor_config.flags.use_qspi_interface = 1;
    return vendor_config;
}
}

// Brings up the QSPI bus, panel IO and SH8601 panel. Both board revisions use
// this path; only CS pin, X offset and QSPI clock differ (see board()).
// mirror_cs_pin >= 0 additionally brings up a second panel on that chip select
// (degraded display mode); a mirror failure is not fatal.
// On failure everything created so far is released.
bool DisplayManager::init_panel(int8_t cs_pin, int8_t mirror_cs_pin) {
    // Re-init: drop whatever an earlier init left behind
    if (panel_io || panel_handle || panel_io_mirror || panel_handle_mirror ||
        transfer_done_sem || staging_buffers[0]) {
        release_panel();
    }

    spi_bus_config_t bus_config = SH8601_PANEL_BUS_QSPI_CONFIG(
        HW_DISPLAY_SCK_PIN, HW_DISPLAY_D0_PIN, HW_DISPLAY_D1_PIN,
        HW_DISPLAY_D2_PIN, HW_DISPLAY_D3_PIN,
        kMaxTransferBytes);
    esp_err_t err = spi_bus_initialize(kDisplaySpiHost, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        LOG_BLE("[DISPLAY] ERROR: spi_bus_initialize failed: %s\n", esp_err_to_name(err));
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(cs_pin, color_transfer_done_cb, this);
    io_config.pclk_hz = board().qspi_hz;
    err = esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(kDisplaySpiHost), &io_config, &panel_io);
    if (err != ESP_OK) {
        LOG_BLE("[DISPLAY] ERROR: esp_lcd_new_panel_io_spi failed: %s\n", esp_err_to_name(err));
        panel_io = nullptr;
        release_panel();
        return false;
    }

    sh8601_vendor_config_t vendor_config = make_vendor_config();

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = HW_DISPLAY_RESET_PIN;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;

    err = esp_lcd_new_panel_sh8601(panel_io, &panel_config, &panel_handle);
    if (err != ESP_OK) {
        LOG_BLE("[DISPLAY] ERROR: esp_lcd_new_panel_sh8601 failed: %s\n", esp_err_to_name(err));
        panel_handle = nullptr;
        release_panel();
        return false;
    }
    err = esp_lcd_panel_reset(panel_handle);
    if (err != ESP_OK) {
        LOG_BLE("[DISPLAY] ERROR: esp_lcd_panel_reset failed: %s\n", esp_err_to_name(err));
        release_panel();
        return false;
    }
    err = esp_lcd_panel_init(panel_handle);
    if (err != ESP_OK) {
        LOG_BLE("[DISPLAY] ERROR: esp_lcd_panel_init failed: %s\n", esp_err_to_name(err));
        release_panel();
        return false;
    }
    err = esp_lcd_panel_disp_on_off(panel_handle, true);
    if (err != ESP_OK) {
        LOG_BLE("[DISPLAY] ERROR: esp_lcd_panel_disp_on_off failed: %s\n", esp_err_to_name(err));
        release_panel();
        return false;
    }

    if (mirror_cs_pin >= 0 && !init_mirror_panel(mirror_cs_pin)) {
        LOG_BLE("[DISPLAY] WARNING: mirror panel on CS %d failed, continuing on CS %d only\n",
                mirror_cs_pin, cs_pin);
    }
    return true;
}

// Second panel on the same QSPI bus for degraded display mode. It shares the
// reset line with the primary panel, which init_panel() has just pulsed, so no
// reset is issued here (a second pulse would wipe the primary panel).
bool DisplayManager::init_mirror_panel(int8_t cs_pin) {
    esp_lcd_panel_io_spi_config_t io_config =
        SH8601_PANEL_IO_QSPI_CONFIG(cs_pin, color_transfer_done_cb, this);
    io_config.pclk_hz = board().qspi_hz;
    esp_err_t err = esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(kDisplaySpiHost), &io_config, &panel_io_mirror);
    if (err != ESP_OK) {
        LOG_BLE("[DISPLAY] ERROR: mirror esp_lcd_new_panel_io_spi failed: %s\n", esp_err_to_name(err));
        panel_io_mirror = nullptr;
        return false;
    }

    sh8601_vendor_config_t vendor_config = make_vendor_config();
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;

    err = esp_lcd_new_panel_sh8601(panel_io_mirror, &panel_config, &panel_handle_mirror);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel_handle_mirror);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel_handle_mirror, true);
    }
    if (err != ESP_OK) {
        LOG_BLE("[DISPLAY] ERROR: mirror panel bring-up failed: %s\n", esp_err_to_name(err));
        release_mirror_panel();
        return false;
    }
    return true;
}

void DisplayManager::release_mirror_panel() {
    if (panel_handle_mirror) {
        esp_lcd_panel_del(panel_handle_mirror);
        panel_handle_mirror = nullptr;
    }
    if (panel_io_mirror) {
        esp_lcd_panel_io_del(panel_io_mirror);
        panel_io_mirror = nullptr;
    }
}

// Deletes whatever init_panel() created, in reverse order. Safe on partial state.
void DisplayManager::release_panel() {
    // A timed-out strip transfer may still be reading a staging buffer: give it a
    // last chance to finish before the devices and buffers are torn down.
    if (transfer_pending && transfer_done_sem) {
        xSemaphoreTake(transfer_done_sem, pdMS_TO_TICKS(HW_DISPLAY_STAGING_TIMEOUT_MS));
        transfer_pending = false;
    }
    release_mirror_panel();
    if (panel_handle) {
        esp_lcd_panel_del(panel_handle);
        panel_handle = nullptr;
    }
    if (panel_io) {
        esp_lcd_panel_io_del(panel_io);
        panel_io = nullptr;
    }
    spi_bus_free(kDisplaySpiHost);
    pending_flush_display = nullptr;
    free_staging();
}

int DisplayManager::probe_te_edges(int8_t cs_pin, uint8_t te_pin, uint32_t window_ms) {
    DisplayManager probe;
    if (!probe.init_panel(cs_pin)) {
        return -1;
    }
    pinMode(te_pin, INPUT);
    g_te_edge_count = 0;
    attachInterrupt(te_pin, te_edge_isr, CHANGE);
    delay(window_ms);
    detachInterrupt(te_pin);
    const int edges = static_cast<int>(g_te_edge_count);
    probe.release_panel();
    return edges;
}

// Allocates the transfer-done semaphore and two internal DMA staging buffers.
// The row count starts at HW_DISPLAY_STAGING_ROWS and halves on allocation
// failure down to HW_DISPLAY_STAGING_MIN_ROWS. Returns false (nothing held) on failure.
bool DisplayManager::alloc_staging() {
    transfer_pending = false;
    transfer_done_sem = xSemaphoreCreateBinary();
    if (!transfer_done_sem) {
        LOG_BLE("[DISPLAY] ERROR: Failed to create transfer semaphore\n");
        return false;
    }
    const size_t row_bytes = HW_DISPLAY_WIDTH_PX * sizeof(uint16_t);
    for (uint32_t rows = HW_DISPLAY_STAGING_ROWS; rows >= HW_DISPLAY_STAGING_MIN_ROWS; rows /= 2) {
        for (int i = 0; i < 2; i++) {
            staging_buffers[i] = static_cast<uint8_t*>(heap_caps_malloc(
                row_bytes * rows, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        }
        if (staging_buffers[0] && staging_buffers[1]) {
            staging_rows = rows;
            return true;
        }
        for (int i = 0; i < 2; i++) {
            heap_caps_free(staging_buffers[i]);
            staging_buffers[i] = nullptr;
        }
    }
    free_staging();
    return false;
}

void DisplayManager::free_staging() {
    for (int i = 0; i < 2; i++) {
        if (staging_buffers[i]) {
            heap_caps_free(staging_buffers[i]);
            staging_buffers[i] = nullptr;
        }
    }
    staging_rows = 0;
    transfer_pending = false;
    if (transfer_done_sem) {
        vSemaphoreDelete(transfer_done_sem);
        transfer_done_sem = nullptr;
    }
}

void DisplayManager::init() {
    g_display_manager = this;
    initialized = false;

    const int8_t mirror_cs_pin = board_display_mirror_cs_pin(board());
    if (mirror_cs_pin >= 0) {
        LOG_BLE("[DISPLAY] Board unknown: degraded mode, mirroring to CS %d and CS %d\n",
                board().display_cs_pin, mirror_cs_pin);
    }
    if (!init_panel(board().display_cs_pin, mirror_cs_pin)) {
        return;
    }

    // Initialize LVGL
    lv_init();
    lv_tick_set_cb(millis_cb);

    screen_width = HW_DISPLAY_WIDTH_PX;
    screen_height = HW_DISPLAY_HEIGHT_PX;

    // Partial rendering. Preferred: a large LVGL buffer in PSRAM (few, large
    // strips) whose strips are sent through small internal DMA staging buffers,
    // because esp_lcd cannot queue color data straight from PSRAM. Fallback: a
    // small internal DMA draw buffer sent directly (many more strips per redraw).
    const size_t row_bytes = screen_width * sizeof(uint16_t);
    buffer_size = row_bytes * HW_DISPLAY_DRAW_BUF_ROWS_PSRAM;
    buffer_size = (buffer_size + HW_DISPLAY_DRAW_BUF_ALIGN_BYTES - 1) &
                  ~static_cast<size_t>(HW_DISPLAY_DRAW_BUF_ALIGN_BYTES - 1);
    if (alloc_staging()) {
        draw_buffer = static_cast<lv_color_t*>(
            heap_caps_aligned_alloc(HW_DISPLAY_DRAW_BUF_ALIGN_BYTES, buffer_size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!draw_buffer) {
            free_staging();
        }
    }
    if (!draw_buffer) {
        // Mirroring needs the staged flush path (one buffer sent twice)
        if (panel_handle_mirror) {
            LOG_BLE("[DISPLAY] WARNING: no staging buffers, mirror panel disabled\n");
            release_mirror_panel();
        }
        buffer_size = row_bytes * HW_DISPLAY_DRAW_BUF_ROWS_INTERNAL;
        LOG_BLE("[DISPLAY] WARNING: PSRAM draw buffer or staging unavailable, using %u byte internal buffer\n",
                static_cast<unsigned>(buffer_size));
        draw_buffer = static_cast<lv_color_t*>(
            heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, buffer_size,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    }

    if (!draw_buffer) {
        LOG_BLE("[DISPLAY] ERROR: Failed to allocate LVGL draw buffer (%u bytes)\n",
                static_cast<unsigned>(buffer_size));
        release_panel();
        return;
    }

    lvgl_display = lv_display_create(screen_width, screen_height);
    if (!lvgl_display) {
        LOG_BLE("[DISPLAY] ERROR: lv_display_create failed\n");
        heap_caps_free(draw_buffer);
        draw_buffer = nullptr;
        release_panel();
        return;
    }
    lv_display_set_flush_cb(lvgl_display, display_flush_cb);
    lv_display_set_buffers(lvgl_display, draw_buffer, NULL,
                          buffer_size , LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_display_add_event_cb(lvgl_display, display_rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    
    // Initialize touch
    touch_driver.init();
    lvgl_input = lv_indev_create();
    lv_indev_set_type(lvgl_input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvgl_input, touchpad_read_cb);
    
    initialized = true;
}

void DisplayManager::update() {
    if (!initialized) return;
    
    touch_driver.update();
    lv_timer_handler();
}

// Update the refresh area to be full width
// This avoids weird artifacts when partial row updates are used
void DisplayManager::display_rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    
    area->x1 = 0;
    area->x2 = g_display_manager->screen_width - 1;
}

void DisplayManager::display_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    if (!g_display_manager || !g_display_manager->panel_handle) {
        lv_display_flush_ready(disp);
        return;
    }

    if (g_display_manager->staging_buffers[0]) {
        g_display_manager->flush_staged(disp, area, px_map);
        return;
    }

    g_display_manager->pending_flush_display = disp;
    esp_err_t result = esp_lcd_panel_draw_bitmap(
        g_display_manager->panel_handle,
        area->x1 + board().display_x_offset_px, area->y1,
        area->x2 + board().display_x_offset_px + 1, area->y2 + 1,
        px_map);
    if (result != ESP_OK) {
        g_display_manager->pending_flush_display = nullptr;
        lv_display_flush_ready(disp);
    }
}

// Sends a strip rendered in PSRAM through the internal staging buffers. While
// chunk N is on the wire, chunk N+1 is copied into the other buffer; only one
// transfer is ever queued at a time. lv_display_flush_ready is called once, after
// the last chunk completes or on the first error (remaining chunks are dropped).
// A transfer that times out may still be reading a staging buffer, so it is kept
// as transfer_pending and awaited before the buffers are touched again; if it
// never completes, strips are dropped rather than overwriting DMA source memory.
void DisplayManager::flush_staged(lv_display_t* disp, const lv_area_t* area, const uint8_t* px_map) {
    const size_t row_bytes = (area->x2 - area->x1 + 1) * sizeof(uint16_t);
    const TickType_t timeout = pdMS_TO_TICKS(HW_DISPLAY_STAGING_TIMEOUT_MS);

    if (transfer_pending) {
        if (xSemaphoreTake(transfer_done_sem, timeout) != pdTRUE) {
            LOG_BLE("[DISPLAY] ERROR: earlier strip transfer still outstanding, dropping strip\n");
            lv_display_flush_ready(disp);
            return;
        }
        transfer_pending = false;
    }
    xSemaphoreTake(transfer_done_sem, 0); // drop any stale completion (e.g. from a failed draw_bitmap)

    bool in_flight = false;
    bool ok = true;

    // Waits for the queued transfer; a timeout leaves it marked outstanding.
    auto wait_for_transfer = [&](int32_t y) {
        if (!in_flight) return true;
        in_flight = false;
        if (xSemaphoreTake(transfer_done_sem, timeout) != pdTRUE) {
            LOG_BLE("[DISPLAY] ERROR: strip transfer timed out (y=%d)\n", static_cast<int>(y));
            transfer_pending = true;
            return false;
        }
        return true;
    };
    // Queues one strip on a panel; only one transfer is ever queued at a time.
    auto send_strip = [&](esp_lcd_panel_handle_t panel, int32_t y, uint32_t rows, const uint8_t* data) {
        esp_err_t result = esp_lcd_panel_draw_bitmap(
            panel,
            area->x1 + board().display_x_offset_px, y,
            area->x2 + board().display_x_offset_px + 1, y + rows,
            data);
        if (result != ESP_OK) {
            LOG_BLE("[DISPLAY] ERROR: draw_bitmap failed: %s\n", esp_err_to_name(result));
            return false;
        }
        in_flight = true;
        return true;
    };

    for (int32_t y = area->y1, chunk = 0; y <= area->y2 && ok; y += staging_rows, chunk++) {
        const uint32_t rows = (area->y2 - y + 1 < static_cast<int32_t>(staging_rows))
                                  ? (area->y2 - y + 1) : staging_rows;
        uint8_t* staging = staging_buffers[chunk & 1];
        memcpy(staging, px_map + static_cast<size_t>(y - area->y1) * row_bytes, rows * row_bytes);

        ok = wait_for_transfer(y) && send_strip(panel_handle, y, rows, staging);
        // Degraded mode: the same staging buffer goes out again on the other chip select
        if (ok && panel_handle_mirror) {
            ok = wait_for_transfer(y) && send_strip(panel_handle_mirror, y, rows, staging);
        }
    }
    if (!wait_for_transfer(area->y2)) {
        LOG_BLE("[DISPLAY] ERROR: final strip transfer timed out\n");
    }
    lv_display_flush_ready(disp);
}

bool DisplayManager::color_transfer_done_cb(esp_lcd_panel_io_handle_t,
                                            esp_lcd_panel_io_event_data_t*,
                                            void* user_context) {
    DisplayManager* manager = static_cast<DisplayManager*>(user_context);
    if (manager && manager->staging_buffers[0] && manager->transfer_done_sem) {
        BaseType_t higher_priority_woken = pdFALSE;
        xSemaphoreGiveFromISR(manager->transfer_done_sem, &higher_priority_woken);
        return higher_priority_woken == pdTRUE;
    }
    if (manager && manager->pending_flush_display) {
        lv_display_t* display = manager->pending_flush_display;
        manager->pending_flush_display = nullptr;
        lv_display_flush_ready(display);
    }
    return false;
}

void DisplayManager::touchpad_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (!g_display_manager) return;
    
    TouchData touch = g_display_manager->touch_driver.get_touch_data();
    
    if (touch.pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = touch.x;
        data->point.y = touch.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

uint32_t DisplayManager::millis_cb() {
    return millis();
}

void DisplayManager::set_brightness(float brightness) {
    if (!initialized || !panel_io) return;
    
    // Clamp brightness to valid hardware range [0.0, 1.0]
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;
    
    uint8_t brightness_value = (uint8_t)(brightness * 255.0f);
    uint32_t brightness_command = (0x02UL << 24) | (0x51UL << 8);
    esp_lcd_panel_io_tx_param(panel_io, brightness_command,
                              &brightness_value, sizeof(brightness_value));
    if (panel_io_mirror) {
        esp_lcd_panel_io_tx_param(panel_io_mirror, brightness_command,
                                  &brightness_value, sizeof(brightness_value));
    }
}
