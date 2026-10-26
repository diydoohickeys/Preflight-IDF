#ifndef WIFI_SETUP_FORM_BODY_H
#define WIFI_SETUP_FORM_BODY_H

#include "esp_http_server.h"
#include "esp_timer.h"
#include <string>

namespace wifisetup {

// Fits a fully percent-encoded device name + SSID + passphrase while bounding what one
// POST can make us allocate.
constexpr size_t MAX_FORM_BODY = 1024;
// Each timeout is httpd's recv_wait_timeout (5 s by default).
constexpr int MAX_RECV_TIMEOUTS = 3;
constexpr int64_t FORM_BODY_DEADLINE_MS = 15000;

// application/x-www-form-urlencoded: '+' is a space and every reserved byte is %XX.
inline std::string urlDecode(const std::string& in) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < in.size()) {
            int hi = hexVal(in[i + 1]);
            int lo = hexVal(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

// Reads all of content_len: httpd_req_recv() can return short, and anything unread is
// left on the socket. On false the 400 is sent; the caller returns ESP_FAIL to close it.
// maxBytes is raised only by routes that carry PEM (a certificate and its CA).
inline bool readFormBody(httpd_req_t* req, std::string& body, size_t maxBytes = MAX_FORM_BODY) {
    if (req->content_len == 0 || req->content_len > maxBytes) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request body");
        return false;
    }

    body.resize(req->content_len);
    const int64_t deadlineUs = esp_timer_get_time() + FORM_BODY_DEADLINE_MS * 1000LL;
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, &body[got], req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            timeouts++;
        } else if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
            return false;
        } else {
            got += (size_t)r;
            timeouts = 0;
        }
        // Consecutive timeouts bound a stalled client; the deadline bounds a trickling one.
        if (got < req->content_len &&
            (timeouts >= MAX_RECV_TIMEOUTS || esp_timer_get_time() > deadlineUs)) {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Request body timed out");
            return false;
        }
    }
    return true;
}

inline std::string formValue(const std::string& body, const char* key) {
    size_t pos = 0;
    while (pos < body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        size_t eq = body.find('=', pos);
        if (eq != std::string::npos && eq < amp && urlDecode(body.substr(pos, eq - pos)) == key) {
            return urlDecode(body.substr(eq + 1, amp - eq - 1));
        }
        pos = amp + 1;
    }
    return std::string();
}

}  // namespace wifisetup

#endif  // WIFI_SETUP_FORM_BODY_H
