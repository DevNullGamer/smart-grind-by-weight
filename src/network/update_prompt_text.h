#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/update.h"

// Pure text helpers for the on-device Accept prompt (no Arduino/ESP dependencies, unit tested on the host).
// Everything the client supplied (version, peer address) is sanitised before it reaches the screen.

// Copy of in for display: printable ASCII (0x20-0x7E) only, every other byte becomes '?', cut after max_chars
// characters. Null input gives "". Returns out; NUL-terminated for out_len > 0 (out_len - 1 characters at most).
const char* prompt_sanitize(const char* in, size_t max_chars, char* out, size_t out_len);

// "0 KB", "812 KB", "2 MB", "2.2 MB" (decimal units: 1 MB = 1,000,000 bytes, one decimal, ".0" dropped).
// Needs at most 10 bytes. Returns out.
const char* prompt_format_size(uint32_t bytes, char* out, size_t out_len);

// Dialog message, lines separated by '\n':
//   Install new firmware?
//   v1.5.0 (build 245)            only the parts the client sent ("v1.5.0", "build 245"); no line when neither
//   2.2 MB from 192.168.1.20      " from <ip>" only when peer_ip is not empty
//   The grinder restarts when done.
// build < 0 means absent; version and peer_ip may be null or empty. A leading digit in the version gets a "v".
// Never writes more than out_len bytes (UPDATE_PROMPT_MESSAGE_BUF always suffices). Returns out.
const char* prompt_build_message(uint32_t size, int32_t build, const char* version, const char* peer_ip, char* out,
                                 size_t out_len);
