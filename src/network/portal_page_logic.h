#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../config/portal.h"

// Pure helpers of the embedded web panel page (no Arduino/ESP dependencies, unit tested on the host).

// True when a gzip page of this size fits PORTAL_PAGE_BUDGET_BYTES (the build script refuses a bigger one,
// the firmware asserts the same at compile time).
bool portal_page_within_budget(size_t gz_size);

// Quoted strong ETag "b<build>-<8 hex of the gzip crc32>": keyed to the build number, so a new firmware always
// revalidates, and to the content, so a rebuilt page under an unchanged number cannot be served stale. Returns
// the length written (0 if cap is too small; out is then empty). Needs PORTAL_PAGE_ETAG_MAX_LEN bytes.
size_t portal_page_etag(int32_t build, uint32_t content_crc32, char* out, size_t cap);

// If-None-Match check for a 304: true when the header (a comma separated list of entity tags, weak "W/" prefix
// ignored) names etag, or is "*". nullptr / empty is false.
bool portal_etag_matches(const char* if_none_match, const char* etag);
