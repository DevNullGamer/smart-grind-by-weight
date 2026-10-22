// chg008 spike experiment F4: route mbedTLS allocations to PSRAM.
// The precompiled libmbedcrypto.a (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y) calls esp_mbedtls_mem_calloc /
// esp_mbedtls_mem_free (member esp_mem.c.obj, which defines only these two symbols). Defining them here
// means the linker never pulls that archive member, so there is no duplicate-symbol clash.
// Compiled only with -DDEBUG_SPIKE_TLS_PSRAM=1.

#include "mbedtls_psram_override.h"

#if DEBUG_SPIKE_TLS_PSRAM

#include <Arduino.h>
#include <esp_heap_caps.h>

namespace {

constexpr size_t kLogMinBytes = 4096;
constexpr int kLogMaxEntries = 24;  // first N allocations >= 4 KB per TLS attempt

struct BigAlloc {
    uint32_t bytes;
    bool psram;
};

BigAlloc g_big[kLogMaxEntries];
volatile int g_big_count = 0;  // number of big allocations seen since reset (may exceed kLogMaxEntries)
volatile uint32_t g_psram_allocs = 0;
volatile uint32_t g_fallback_allocs = 0;
volatile uint32_t g_failed_allocs = 0;
volatile uint32_t g_psram_bytes = 0;
volatile uint32_t g_fallback_bytes = 0;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

extern "C" void* esp_mbedtls_mem_calloc(size_t n, size_t size) {
    bool psram = true;
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        psram = false;
        p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    size_t total = n * size;
    portENTER_CRITICAL(&g_mux);
    if (!p) {
        g_failed_allocs = g_failed_allocs + 1;
    } else if (psram) {
        g_psram_allocs = g_psram_allocs + 1;
        g_psram_bytes = g_psram_bytes + static_cast<uint32_t>(total);
    } else {
        g_fallback_allocs = g_fallback_allocs + 1;
        g_fallback_bytes = g_fallback_bytes + static_cast<uint32_t>(total);
    }
    if (total >= kLogMinBytes) {
        int i = g_big_count;
        if (i < kLogMaxEntries) g_big[i] = {static_cast<uint32_t>(total), p && psram};
        g_big_count = i + 1;
    }
    portEXIT_CRITICAL(&g_mux);
    return p;
}

extern "C" void esp_mbedtls_mem_free(void* ptr) {
    heap_caps_free(ptr);
}

void mbedtls_psram_stats_reset() {
    portENTER_CRITICAL(&g_mux);
    g_big_count = 0;
    g_psram_allocs = 0;
    g_fallback_allocs = 0;
    g_failed_allocs = 0;
    g_psram_bytes = 0;
    g_fallback_bytes = 0;
    portEXIT_CRITICAL(&g_mux);
}

void mbedtls_psram_stats_report(const char* tag) {
    int n = g_big_count;
    int shown = n < kLogMaxEntries ? n : kLogMaxEntries;
    for (int i = 0; i < shown; i++) {
        Serial.printf("[SPIKE][TLSPSRAM] %s big alloc #%d: %u bytes from %s\n", tag, i + 1,
                      static_cast<unsigned>(g_big[i].bytes), g_big[i].psram ? "PSRAM" : "INTERNAL (fallback)");
    }
    Serial.printf("[SPIKE][TLSPSRAM] %s summary: PSRAM allocs %u (%u bytes), internal fallback allocs %u (%u bytes), "
                  "failed %u, allocs >= 4 KB %d (first %d listed)\n",
                  tag, static_cast<unsigned>(g_psram_allocs), static_cast<unsigned>(g_psram_bytes),
                  static_cast<unsigned>(g_fallback_allocs), static_cast<unsigned>(g_fallback_bytes),
                  static_cast<unsigned>(g_failed_allocs), n, shown);
}

#endif  // DEBUG_SPIKE_TLS_PSRAM
