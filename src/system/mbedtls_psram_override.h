#pragma once

// chg008 spike experiment F4: stats of the PSRAM mbedTLS allocator override (see mbedtls_psram_override.cpp).
#ifndef DEBUG_SPIKE_TLS_PSRAM
#define DEBUG_SPIKE_TLS_PSRAM 0
#endif

#if DEBUG_SPIKE_TLS_PSRAM
void mbedtls_psram_stats_reset();
void mbedtls_psram_stats_report(const char* tag);
#endif
