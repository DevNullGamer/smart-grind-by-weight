#include "network/portal_page_logic.h"
#include <stdio.h>
#include <string.h>

bool portal_page_within_budget(size_t gz_size) {
    return gz_size > 0 && gz_size <= PORTAL_PAGE_BUDGET_BYTES;
}

size_t portal_page_etag(int32_t build, uint32_t content_crc32, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    const int n = snprintf(out, cap, "\"b%ld-%08lx\"", static_cast<long>(build), static_cast<unsigned long>(content_crc32));
    if (n < 0 || static_cast<size_t>(n) >= cap) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n);
}

bool portal_etag_matches(const char* if_none_match, const char* etag) {
    if (!if_none_match || !etag || etag[0] == '\0') return false;
    const size_t etag_len = strlen(etag);
    const char* p = if_none_match;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (*p == '\0') break;
        const char* start = p;
        while (*p && *p != ',') ++p;
        const char* end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;
        if (end - start == 1 && start[0] == '*') return true;
        if (end - start > 2 && start[0] == 'W' && start[1] == '/') start += 2;  // weak comparison: same tag
        const size_t tag_len = static_cast<size_t>(end - start);
        if (tag_len == etag_len && memcmp(start, etag, etag_len) == 0) return true;
    }
    return false;
}
