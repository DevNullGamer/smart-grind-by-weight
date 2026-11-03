#pragma once

//==============================================================================
// WEB PANEL (INTERNAL PORTAL) CONFIGURATION
//==============================================================================
// Plain #defines only: this file is also included by the host-testable network/portal_api and
// network/portal_guard modules (no Arduino dependencies). Contract: docs/PORTAL-API.md.

#define PORTAL_BODY_MAX 512                                                    // Largest accepted PATCH/command body (bytes)
#define PORTAL_MAX_FIELDS 24                                                   // Most members one request object may carry (keeps the duplicate check bounded)
#define PORTAL_KEY_MAX_LEN 32                                                  // Longest accepted object key
#define PORTAL_STRING_MAX_LEN 24                                               // Longest accepted string value (enum choices, command names, device name)
#define PORTAL_DEVICE_NAME_MAX_LEN 24                                          // Longest device name (mDNS host); WiFiManager itself accepts up to WIFI_HOSTNAME_MAX_LEN
#define PORTAL_HEADER_MAX_LEN 128                                              // Longest Host/Origin/Content-Type value looked at; longer is refused
#define PORTAL_CUSTOM_HEADER_NAME "X-Grinder"                                  // State-changing calls must carry this header ...
#define PORTAL_CUSTOM_HEADER_VALUE "1"                                         // ... with this value (forces a CORS preflight the device never answers)
#define PORTAL_LEAD_IN_MS 3000                                                 // Remote start: touch-to-cancel window before the grind starts

// Reply buffers (the HTTP task has a 6 KB stack, so these live on the heap) and the UI-task hand-over timing
#define PORTAL_VALUES_JSON_MAX 1024                                            // GET /api/settings and the PATCH reply
#define PORTAL_SCHEMA_JSON_MAX 4096                                            // GET /api/schema
#define PORTAL_STATUS_JSON_MAX 640                                             // GET /api/status (worst case with all nine warnings is 470 bytes)
#define PORTAL_STATS_JSON_MAX 512                                              // GET /api/stats (worst case with every number at its maximum is 334 bytes)
#define PORTAL_HISTORY_JSON_MAX 2560                                           // GET /api/history (10 worst-case entries are about 2.1 KB, see test_history_worst_case_fits_its_buffer_with_margin)
#define PORTAL_COMMAND_REPLY_MAX 96                                            // POST /api/command reply (the start reply with lead_in_ms is the longest)
#define PORTAL_PICKUP_TIMEOUT_MS 500                                           // A write the UI task has not picked up by then is answered 503 ui_busy
#define PORTAL_RUN_TIMEOUT_MS 2000                                             // A write the UI task picked up but did not finish by then is answered 503 ui_busy
#define PORTAL_WAIT_POLL_MS 10                                                 // The HTTP task polls the mailbox at this interval
#define PORTAL_STATUS_PUBLISH_MS 250                                           // The UI task republishes the status snapshot this often
#define PORTAL_VALUES_REFRESH_MS 10000                                         // ... and re-reads the stored settings this often, after a write and when the menu is left (the touchscreen changes them without telling us; each read opens NVS namespaces)
#define PORTAL_STATS_REFRESH_MS 5000                                           // The UI task re-reads the lifetime statistics this often (and when the screen state changes)
#define PORTAL_STATS_STALE_MS 15000                                            // A statistics snapshot older than this (UI task stuck) is answered 503 ui_busy
#define PORTAL_HISTORY_MAX 10                                                  // Newest grinds the history ring holds and GET /api/history will return
#define GRIND_HISTORY_FILE_RECORDS 16                                          // Records per history file (two files: /hist0.bin, /hist1.bin)
#define PORTAL_HISTORY_REFRESH_MS 5000                                         // The UI task republishes the history snapshot this often (a changed ring is published at once, within one status tick)
#define PORTAL_HISTORY_STALE_MS 15000                                          // A history snapshot older than this (UI task stuck) is answered 503 ui_busy
#define PORTAL_SNAPSHOT_STALE_MS 5000                                         // A snapshot older than this (UI task stuck or UI off) is answered 503 ui_busy

// Embedded page (built from web/portal/index.html by tools/build-scripts/embed_portal.py, gzip, served from flash)
#define PORTAL_PAGE_SOURCE "web/portal/index.html"                             // Relative to the project root; the build fails if it is missing
#define PORTAL_PAGE_BUDGET_BYTES 65536                                         // The build fails when the gzip page is larger (read by embed_portal.py)
#define PORTAL_PAGE_ETAG_MAX_LEN 40                                            // Buffer for the quoted ETag
#define PORTAL_PAGE_CSP "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'"
