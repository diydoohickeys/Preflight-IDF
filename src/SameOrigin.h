#ifndef WIFI_SETUP_SAME_ORIGIN_H
#define WIFI_SETUP_SAME_ORIGIN_H

#include <cstring>
#include <strings.h>
#include "esp_http_server.h"

namespace wifisetup {

// Guards state-changing POSTs against cross-site requests. Browsers send Origin (or at
// least Referer) on a cross-site POST; tools and scripts send neither, so an absent header
// passes. An "Origin: null" (sandboxed frame, file://) has no authority and is refused.
inline bool isSameOrigin(httpd_req_t* req) {
    char source[256];
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Origin", source, sizeof(source));
    if (err == ESP_ERR_NOT_FOUND) {
        err = httpd_req_get_hdr_value_str(req, "Referer", source, sizeof(source));
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return true;
    }
    // A long Referer is truncated to the buffer; only its leading authority is needed.
    if (err != ESP_OK && err != ESP_ERR_HTTPD_RESULT_TRUNC) {
        return false;
    }

    char host[128];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }

    const char* authority = strstr(source, "://");
    if (!authority) {
        return false;
    }
    authority += 3;
    const size_t length = strcspn(authority, "/");
    if (authority[length] == '\0' && err == ESP_ERR_HTTPD_RESULT_TRUNC) {
        return false;
    }
    return length == strlen(host) && strncasecmp(authority, host, length) == 0;
}

inline esp_err_t sendCrossOriginRefused(httpd_req_t* req) {
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Cross-origin request refused");
}

}  // namespace wifisetup

#endif  // WIFI_SETUP_SAME_ORIGIN_H
