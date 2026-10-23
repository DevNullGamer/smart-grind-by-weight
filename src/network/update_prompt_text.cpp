#include "update_prompt_text.h"
#include <stdio.h>

const char* prompt_sanitize(const char* in, size_t max_chars, char* out, size_t out_len) {
    if (!out || out_len == 0) return out;
    size_t n = 0;
    if (in) {
        for (; in[n] != '\0' && n < max_chars && n + 1 < out_len; n++) {
            const unsigned char c = static_cast<unsigned char>(in[n]);
            out[n] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
        }
    }
    out[n] = '\0';
    return out;
}

const char* prompt_format_size(uint32_t bytes, char* out, size_t out_len) {
    if (!out || out_len == 0) return out;
    if (bytes < 999500u) {
        snprintf(out, out_len, "%u KB", static_cast<unsigned>((bytes + 500u) / 1000u));
    } else {
        // Rounded to tenths of a MB; 64-bit so sizes near 4 GB cannot overflow
        const uint32_t tenths = static_cast<uint32_t>((static_cast<uint64_t>(bytes) + 50000u) / 100000u);
        if (tenths % 10 == 0) {
            snprintf(out, out_len, "%u MB", static_cast<unsigned>(tenths / 10));
        } else {
            snprintf(out, out_len, "%u.%u MB", static_cast<unsigned>(tenths / 10), static_cast<unsigned>(tenths % 10));
        }
    }
    return out;
}

const char* prompt_build_message(uint32_t size, int32_t build, const char* version, const char* peer_ip, char* out,
                                 size_t out_len) {
    if (!out || out_len == 0) return out;
    out[0] = '\0';

    char ver[UPDATE_PROMPT_TEXT_MAX + 1];
    char ip[UPDATE_PEER_IP_BUF + 1];
    char size_text[12];
    prompt_sanitize(version, UPDATE_PROMPT_TEXT_MAX, ver, sizeof(ver));
    prompt_sanitize(peer_ip, UPDATE_PEER_IP_BUF - 1, ip, sizeof(ip));
    prompt_format_size(size, size_text, sizeof(size_text));

    size_t used = 0;
    auto append = [&](const char* format, auto... args) {
        if (used + 1 >= out_len) return;
        const int n = snprintf(out + used, out_len - used, format, args...);
        if (n < 0) return;
        used += (static_cast<size_t>(n) < out_len - used) ? static_cast<size_t>(n) : out_len - used - 1;
    };

    append("Install new firmware?\n");
    const bool has_version = ver[0] != '\0';
    const bool has_build = build >= 0;
    const char* v_prefix = (ver[0] >= '0' && ver[0] <= '9') ? "v" : "";
    if (has_version && has_build) {
        append("%s%s (build %ld)\n", v_prefix, ver, static_cast<long>(build));
    } else if (has_version) {
        append("%s%s\n", v_prefix, ver);
    } else if (has_build) {
        append("build %ld\n", static_cast<long>(build));
    }
    if (ip[0] != '\0') {
        append("%s from %s\n", size_text, ip);
    } else {
        append("%s\n", size_text);
    }
    append("The grinder restarts when done.");
    return out;
}
