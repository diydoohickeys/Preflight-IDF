#include "AccessGuard.h"
#include "FormBody.h"
#include "HttpdRoute.h"
#include "SameOrigin.h"
#include "Logger.h"
#include "html/login_html.h"  // generated from extras/html_source/login.html by extras/convert_html.py
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// A browser keeps a Secure cookie set over HTTPS and refuses to let plain HTTP overwrite it
// or receive it, so one shared name traps a device whose HTTPS was turned off in a sign-in
// loop. Each scheme gets its own name and never sees the other's.
constexpr const char* COOKIE_NAME_HTTP = "pf_session_http";
constexpr const char* COOKIE_NAME_HTTPS = "pf_session_https";

esp_err_t sendJson(httpd_req_t* req, const char* status, const char* json) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

esp_err_t redirect(httpd_req_t* req, const char* location) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, nullptr, 0);
}

// A browser navigating to a page, as opposed to a fetch, a script or a WebSocket.
bool isPageNavigation(httpd_req_t* req) {
    if (req->method != HTTP_GET) {
        return false;
    }
    char accept[256];
    const esp_err_t err = httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof(accept));
    return (err == ESP_OK || err == ESP_ERR_HTTPD_RESULT_TRUNC) && strstr(accept, "text/html");
}

std::string urlEncode(const char* in) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (const char* p = in; *p; p++) {
        const unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out += (char)c;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

void wipe(std::string& s) {
    std::fill(s.begin(), s.end(), '\0');
}

}  // namespace

bool AccessGuardClass::isAuthorized(httpd_req_t* req) {
    char header[16 + TOKEN_HEX];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK &&
        strncmp(header, "Bearer ", 7) == 0) {
        return matchesApiKey(header + 7);
    }

    char token[TOKEN_HEX + 1];
    size_t len = sizeof(token);
    const char* name = _https ? COOKIE_NAME_HTTPS : COOKIE_NAME_HTTP;
    return httpd_req_get_cookie_val(req, name, token, &len) == ESP_OK && matchesSession(token);
}

bool AccessGuardClass::allow(httpd_req_t* req) {
    if (isAuthorized(req)) {
        return true;
    }
    refuse(req);
    return false;
}

bool AccessGuardClass::allowSetup(httpd_req_t* req) {
    return !hasPassword() || allow(req);
}

void AccessGuardClass::refuse(httpd_req_t* req) {
    const bool page = isPageNavigation(req);
    if (!hasPassword()) {
        if (page) {
            redirect(req, "/setup");
        } else {
            sendJson(req, "403 Forbidden", "{\"error\":\"No device password yet - finish setup at /setup\"}");
        }
        return;
    }
    if (page) {
        const std::string location = "/login?next=" + urlEncode(req->uri);
        redirect(req, location.c_str());
        return;
    }
    sendJson(req, "401 Unauthorized", "{\"error\":\"Sign in required\"}");
}

void AccessGuardClass::formatCookie(char* out, size_t len, const char* token) const {
    const char* secure = _https ? "; Secure" : "";
    const char* name = _https ? COOKIE_NAME_HTTPS : COOKIE_NAME_HTTP;
    if (token) {
        snprintf(out, len, "%s=%s; Path=/; Max-Age=31536000; HttpOnly; SameSite=Lax%s", name, token, secure);
    } else {
        snprintf(out, len, "%s=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax%s", name, secure);
    }
}

esp_err_t AccessGuardClass::handleLoginPage(httpd_req_t* req) {
    if (!Access.hasPassword()) {
        return redirect(req, "/setup");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, LOGIN_HTML, HTTPD_RESP_USE_STRLEN);
}

// Same-origin and auth refusals before the body is read return ESP_FAIL, so httpd closes the
// socket instead of draining it.
esp_err_t AccessGuardClass::handleLogin(httpd_req_t* req) {
    // Refused so another site can't sign this browser in to a session of its choosing.
    if (!wifisetup::isSameOrigin(req)) {
        wifisetup::sendCrossOriginRefused(req);
        return ESP_FAIL;
    }
    std::string body;
    if (!wifisetup::readFormBody(req, body)) {
        return ESP_FAIL;
    }
    std::string password = wifisetup::formValue(body, "password");
    wipe(body);

    uint32_t retryAfter = 0;
    const LoginResult result = Access.checkPassword(password.c_str(), &retryAfter);
    wipe(password);

    if (result == LoginResult::LockedOut) {
        char seconds[12];
        snprintf(seconds, sizeof(seconds), "%u", (unsigned)retryAfter);
        httpd_resp_set_hdr(req, "Retry-After", seconds);
        char json[80];
        snprintf(json, sizeof(json), "{\"error\":\"Too many attempts\",\"retryAfter\":%u}", (unsigned)retryAfter);
        return sendJson(req, "429 Too Many Requests", json);
    }
    if (result == LoginResult::Wrong) {
        Logger.debug("Access: sign-in refused (wrong password)");
        return sendJson(req, "401 Unauthorized", "{\"error\":\"Wrong password\"}");
    }

    char token[TOKEN_HEX + 1];
    Access.sessionToken(token);
    char cookie[128];
    Access.formatCookie(cookie, sizeof(cookie), token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    Logger.info("Access: signed in");
    return sendJson(req, "200 OK", "{\"ok\":true}");
}

// Signs every browser out: they all share the one session token.
esp_err_t AccessGuardClass::handleLogout(httpd_req_t* req) {
    if (!wifisetup::isSameOrigin(req)) {
        return wifisetup::sendCrossOriginRefused(req);
    }
    if (Access.isAuthorized(req)) {
        Access.rotateSession();
        Logger.info("Access: signed out");
    }
    char cookie[96];
    Access.formatCookie(cookie, sizeof(cookie), nullptr);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return sendJson(req, "200 OK", "{\"ok\":true}");
}

esp_err_t AccessGuardClass::handleAccessInfo(httpd_req_t* req) {
    if (!Access.allow(req)) {
        return ESP_OK;
    }
    char key[TOKEN_HEX + 1];
    Access.apiKey(key);
    char json[64 + TOKEN_HEX];
    snprintf(json, sizeof(json), "{\"apiKey\":\"%s\"}", key);
    return sendJson(req, "200 OK", json);
}

esp_err_t AccessGuardClass::handleChangePassword(httpd_req_t* req) {
    if (!wifisetup::isSameOrigin(req)) {
        wifisetup::sendCrossOriginRefused(req);
        return ESP_FAIL;
    }
    if (!Access.allow(req)) {
        return ESP_FAIL;
    }
    std::string body;
    if (!wifisetup::readFormBody(req, body)) {
        return ESP_FAIL;
    }
    std::string current = wifisetup::formValue(body, "current");
    std::string next = wifisetup::formValue(body, "new");
    wipe(body);

    uint32_t retryAfter = 0;
    const LoginResult result = Access.checkPassword(current.c_str(), &retryAfter);
    wipe(current);
    if (result != LoginResult::Ok) {
        wipe(next);
        return result == LoginResult::LockedOut
                   ? sendJson(req, "429 Too Many Requests", "{\"error\":\"Too many attempts - try again shortly\"}")
                   : sendJson(req, "401 Unauthorized", "{\"error\":\"Current password is wrong\"}");
    }

    const bool changed = Access.setPassword(next.c_str());
    wipe(next);
    if (!changed) {
        return sendJson(req, "400 Bad Request", "{\"error\":\"The new password must be 8-64 characters\"}");
    }

    // setPassword issued a new token; this browser stays signed in on it.
    char token[TOKEN_HEX + 1];
    Access.sessionToken(token);
    char cookie[128];
    Access.formatCookie(cookie, sizeof(cookie), token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return sendJson(req, "200 OK", "{\"ok\":true}");
}

esp_err_t AccessGuardClass::handleRegenerateApiKey(httpd_req_t* req) {
    if (!wifisetup::isSameOrigin(req)) {
        return wifisetup::sendCrossOriginRefused(req);
    }
    if (!Access.allow(req)) {
        return ESP_OK;
    }
    if (!Access.regenerateApiKey()) {
        return sendJson(req, "500 Internal Server Error", "{\"error\":\"Could not store the new key\"}");
    }
    Logger.info("Access: API key regenerated");
    return handleAccessInfo(req);
}

void AccessGuardClass::registerEndpoints(httpd_handle_t server) {
    if (!server) {
        return;
    }
    const httpd_uri_t routes[] = {
        wifisetup::route("/login", HTTP_GET, handleLoginPage, nullptr),
        wifisetup::route("/login", HTTP_POST, handleLogin, nullptr),
        wifisetup::route("/logout", HTTP_POST, handleLogout, nullptr),
        wifisetup::route("/api/access", HTTP_GET, handleAccessInfo, nullptr),
        wifisetup::route("/api/access/password", HTTP_POST, handleChangePassword, nullptr),
        wifisetup::route("/api/access/api-key", HTTP_POST, handleRegenerateApiKey, nullptr),
    };
    for (const httpd_uri_t& r : routes) {
        if (httpd_register_uri_handler(server, &r) != ESP_OK) {
            Logger.error("Access: could not register %s (handler table full or route taken)", r.uri);
        }
    }
}
