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
