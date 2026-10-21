// Custom LVGL memory backend (LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM).
//
// LVGL widgets, styles and draw buffers are allocated from PSRAM so they do
// not exhaust internal RAM, which BLE and the FreeRTOS task stacks need.
// If PSRAM is full or unavailable, allocation falls back to internal RAM.

#include <lvgl.h>
#include <esp_heap_caps.h>

static constexpr uint32_t LVGL_CAPS_PSRAM = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
static constexpr uint32_t LVGL_CAPS_INTERNAL = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

extern "C" {

void lv_mem_init(void)
{
    // Nothing to init: the ESP-IDF heap is already running
}

void lv_mem_deinit(void)
{
    // Nothing to deinit
}

lv_mem_pool_t lv_mem_add_pool(void * mem, size_t bytes)
{
    // Not supported (no pool-based allocator)
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    // Not supported
    LV_UNUSED(pool);
}

void * lv_malloc_core(size_t size)
{
    void * p = heap_caps_malloc(size, LVGL_CAPS_PSRAM);
    return p != NULL ? p : heap_caps_malloc(size, LVGL_CAPS_INTERNAL);
}

void * lv_realloc_core(void * p, size_t new_size)
{
    void * q = heap_caps_realloc(p, new_size, LVGL_CAPS_PSRAM);
    return q != NULL ? q : heap_caps_realloc(p, new_size, LVGL_CAPS_INTERNAL);
}

void lv_free_core(void * p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t * mon_p)
{
    // Not supported
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    // Not supported
    return LV_RESULT_OK;
}

} // extern "C"
