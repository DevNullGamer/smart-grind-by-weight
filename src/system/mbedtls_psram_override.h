#pragma once

#include <atomic>

// Scoped mbedTLS-in-PSRAM override (chg008 spike F4, promoted to production in R6).
//
// The precompiled mbedTLS (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y) allocates every TLS buffer from internal RAM, which
// is too tight next to BLE (a handshake needs ~40 KB). mbedtls_psram_override.cpp replaces esp_mbedtls_mem_calloc /
// esp_mbedtls_mem_free. While at least one TlsPsramScope is alive the calloc is served from PSRAM (internal RAM as
// a fallback); otherwise it behaves exactly like the stock implementation (heap_caps_calloc internal + 8 bit).
// Free is heap_caps_free in both cases, so memory may outlive the scope that allocated it.
//
// The scope is process-wide (a counter), not per task: while the pull job runs, any other mbedTLS user (e.g. the
// WiFi supplicant) also allocates from PSRAM, which is functionally harmless.
//
// UPDATE_PULL_TLS_PSRAM_UNSCOPED=1 (build flag) makes every allocation PSRAM-first all the time, the exact behaviour
// of the spike experiment F4; it is the documented fallback if the scoped variant misbehaves.

// chg008 spike experiment F4: detailed stats of the allocator (big allocations, counts). Spike envs only; the
// spike env (-DDEBUG_SPIKE_TLS_PSRAM=1) keeps its original unscoped behaviour.
#ifndef DEBUG_SPIKE_TLS_PSRAM
#define DEBUG_SPIKE_TLS_PSRAM 0
#endif
#ifndef UPDATE_PULL_TLS_PSRAM_UNSCOPED
#define UPDATE_PULL_TLS_PSRAM_UNSCOPED DEBUG_SPIKE_TLS_PSRAM
#endif

namespace tls_psram_detail {
inline std::atomic<int> g_scope_depth{0};
}  // namespace tls_psram_detail

// RAII: TLS allocations made while an instance is alive come from PSRAM. Nests; not copyable.
class TlsPsramScope {
public:
    TlsPsramScope() { tls_psram_detail::g_scope_depth.fetch_add(1); }
    ~TlsPsramScope() { tls_psram_detail::g_scope_depth.fetch_sub(1); }
    TlsPsramScope(const TlsPsramScope&) = delete;
    TlsPsramScope& operator=(const TlsPsramScope&) = delete;
};

// True while any TlsPsramScope is alive (always true with UPDATE_PULL_TLS_PSRAM_UNSCOPED)
inline bool tls_psram_scope_active() {
#if UPDATE_PULL_TLS_PSRAM_UNSCOPED
    return true;
#else
    return tls_psram_detail::g_scope_depth.load() > 0;
#endif
}

// Allocations made inside a scope since boot: served from PSRAM / had to fall back to internal RAM
// (defined in mbedtls_psram_override.cpp, firmware only)
unsigned tls_psram_scope_psram_allocs();
unsigned tls_psram_scope_fallback_allocs();

#if DEBUG_SPIKE_TLS_PSRAM
void mbedtls_psram_stats_reset();
void mbedtls_psram_stats_report(const char* tag);
#endif
