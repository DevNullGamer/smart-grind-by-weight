// Scoped mbedTLS-in-PSRAM allocator (see mbedtls_psram_override.h).
//
// The precompiled libmbedcrypto.a (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y) calls esp_mbedtls_mem_calloc /
// esp_mbedtls_mem_free, which live in its archive member esp_mem.c.obj. That member defines exactly these two symbols,
// so defining them here means the linker never pulls it: no duplicate-symbol clash, no -Wl,--allow-multiple-definition.
//
// Stock behaviour, read from the disassembly of esp_mem.c.obj in the installed framework libs (the ESP-IDF C source
// is not part of the install):
//   esp_mbedtls_mem_calloc(n, size) = heap_caps_calloc(n, size, 0x804)  // MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
//   esp_mbedtls_mem_free(ptr)       = heap_caps_free(ptr)
// Outside a TlsPsramScope this file does exactly that.

#include "mbedtls_psram_override.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

namespace {

std::atomic<unsigned> g_psram_allocs{0};
std::atomic<unsigned> g_fallback_allocs{0};

#if DEBUG_SPIKE_TLS_PSRAM
constexpr size_t kLogMinBytes = 4096;
constexpr int kLogMaxEntries = 24;  // first N allocations >= 4 KB per TLS attempt

struct BigAlloc {
    uint32_t bytes;
    bool psram;
};

BigAlloc g_big[kLogMaxEntries];
volatile int g_big_count = 0;  // number of big allocations seen since reset (may exceed kLogMaxEntries)
volatile uint32_t g_spike_psram_allocs = 0;
volatile uint32_t g_spike_fallback_allocs = 0;
volatile uint32_t g_spike_failed_allocs = 0;
volatile uint32_t g_spike_psram_bytes = 0;
volatile uint32_t g_spike_fallback_bytes = 0;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

void spike_record(size_t total, bool psram, void* p) {
    portENTER_CRITICAL(&g_mux);
    if (!p) {
        g_spike_failed_allocs = g_spike_failed_allocs + 1;
    } else if (psram) {
        g_spike_psram_allocs = g_spike_psram_allocs + 1;
        g_spike_psram_bytes = g_spike_psram_bytes + static_cast<uint32_t>(total);
    } else {
        g_spike_fallback_allocs = g_spike_fallback_allocs + 1;
        g_spike_fallback_bytes = g_spike_fallback_bytes + static_cast<uint32_t>(total);
    }
    if (total >= kLogMinBytes) {
        int i = g_big_count;
        if (i < kLogMaxEntries) g_big[i] = {static_cast<uint32_t>(total), p && psram};
        g_big_count = i + 1;
    }
    portEXIT_CRITICAL(&g_mux);
}
#endif

}  // namespace

extern "C" void* esp_mbedtls_mem_calloc(size_t n, size_t size) {
    if (!tls_psram_scope_active()) {
        return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);  // identical to the stock function
    }
    bool psram = true;
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        psram = false;
        p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        g_fallback_allocs.fetch_add(1);
    } else {
        g_psram_allocs.fetch_add(1);
    }
#if DEBUG_SPIKE_TLS_PSRAM
    spike_record(n * size, psram, p);
#else
    (void)psram;
#endif
    return p;
}

extern "C" void esp_mbedtls_mem_free(void* ptr) {
    heap_caps_free(ptr);
}

unsigned tls_psram_scope_psram_allocs() {
    return g_psram_allocs.load();
}

unsigned tls_psram_scope_fallback_allocs() {
    return g_fallback_allocs.load();
}

#if DEBUG_SPIKE_TLS_PSRAM
void mbedtls_psram_stats_reset() {
    portENTER_CRITICAL(&g_mux);
    g_big_count = 0;
    g_spike_psram_allocs = 0;
    g_spike_fallback_allocs = 0;
    g_spike_failed_allocs = 0;
    g_spike_psram_bytes = 0;
    g_spike_fallback_bytes = 0;
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
                  tag, static_cast<unsigned>(g_spike_psram_allocs), static_cast<unsigned>(g_spike_psram_bytes),
                  static_cast<unsigned>(g_spike_fallback_allocs), static_cast<unsigned>(g_spike_fallback_bytes),
                  static_cast<unsigned>(g_spike_failed_allocs), n, shown);
}
#endif  // DEBUG_SPIKE_TLS_PSRAM
