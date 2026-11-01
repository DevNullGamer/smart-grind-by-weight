#pragma once
#include "portal_api.h"
#include "portal_guard.h"

struct httpd_req;  // esp_http_server request, kept out of this header

/**
 * Web panel routes on the shared HTTP server (docs/PORTAL-API.md). Runs on the esp_http_server task, so it
 * never calls LVGL, the controllers or NVS: the switches come from the RAM copies in WiFiManager.
 *
 * Every web route calls portal_route_admit() first: route switch (Web interface), then the Host/Origin guards,
 * and for state-changing routes the Content-Type and X-Grinder checks.
 */

// Applies portal_route_enabled() and portal_check_request(). Returns PortalError::NONE when the request may
// proceed; otherwise the refusal (not yet sent, see portal_send_error()).
PortalError portal_route_admit(httpd_req* req, PortalRoute route);

// Sends {"error","reason"} with the status of the error and the API cache header. Returns ESP_FAIL's value
// (-1) so a handler can `return` it: the server then closes the connection.
int portal_send_error(httpd_req* req, PortalError error);

// GET / and GET /index.html: the embedded gzip page (200, or 304 on a matching If-None-Match)
int portal_handle_page(httpd_req* req);

// The JSON API. Reads copy the snapshot the UI task publishes (503 ui_busy when there is none or it is stale);
// the write hands the parsed patch to the UI task through portal_command_gate and waits for its answer. The
// HTTP task never touches LVGL, the controllers, the profiles or NVS.
int portal_handle_status(httpd_req* req);          // GET /api/status
int portal_handle_settings_get(httpd_req* req);    // GET /api/settings
int portal_handle_schema(httpd_req* req);          // GET /api/schema (constant, no snapshot needed)
int portal_handle_settings_patch(httpd_req* req);  // PATCH /api/settings
