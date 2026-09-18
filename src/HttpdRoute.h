#ifndef WIFI_SETUP_HTTPD_ROUTE_H
#define WIFI_SETUP_HTTPD_ROUTE_H

#include "esp_http_server.h"

namespace wifisetup {

// Value-initialised rather than designated: httpd_uri_t only has its WebSocket fields when
// CONFIG_HTTPD_WS_SUPPORT is on, so naming them breaks every project that leaves it off.
inline httpd_uri_t route(const char* uri, httpd_method_t method,
                         esp_err_t (*handler)(httpd_req_t*), void* userCtx) {
    httpd_uri_t r = {};
    r.uri = uri;
    r.method = method;
    r.handler = handler;
    r.user_ctx = userCtx;
    return r;
}

}  // namespace wifisetup

#endif  // WIFI_SETUP_HTTPD_ROUTE_H
