#pragma once

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstddef>
#include <cstdint>

// The device's admin password, the browser session it signs in, and an API key for scripts.
// Every Preflight page and endpoint is gated by it, and a consumer gates its own handlers with
// allow(). The password is chosen in the setup portal; until one exists, only the setup routes
// answer and everything gated is refused. State lives in NVS namespace "pf_access", so it
// survives reboots and OTA, and factory reset clears it with the WiFi settings.
//
// Sessions: one token, shared by every signed-in browser, in an HttpOnly SameSite=Lax cookie.
// Lax keeps it off cross-site subresource requests, WebSocket handshakes included, so a page on
// another site can neither use nor forge a session. Signing out, or changing the password,
// issues a new token and so signs out every browser.
// Scripts send "Authorization: Bearer <api key>" instead; the key is shown on /setup.
class AccessGuardClass {
public:
    enum class LoginResult { Ok, Wrong, LockedOut };

    static constexpr size_t MIN_PASSWORD = 8;
    static constexpr size_t MAX_PASSWORD = 64;

    // Loads the stored state. NVS must be initialised (WiFiSetupManager::begin does this first).
    void begin();

    bool hasPassword();
    // MIN_PASSWORD..MAX_PASSWORD bytes. Issues a new session token, so every browser signs out.
    bool setPassword(const char* password);
    // retryAfterS is set on LockedOut.
    LoginResult checkPassword(const char* password, uint32_t* retryAfterS = nullptr);
    // Forgets the password, session and API key (factory reset).
    void clear();

    // A signed-in browser or a valid API key. Sends nothing.
    bool isAuthorized(httpd_req_t* req);
    // The gate for a handler: true to proceed. Otherwise the refusal is already sent (a page
    // navigation is redirected to /login, or /setup when no password exists; anything else gets
    // 401/403) and the handler returns ESP_OK, or ESP_FAIL if the request body is unread.
    bool allow(httpd_req_t* req);
    // For the setup routes: open until a password exists, then the same as allow().
    bool allowSetup(httpd_req_t* req);

    // /login, /logout, /api/access, /api/access/password, /api/access/api-key.
    void registerEndpoints(httpd_handle_t server);
    // Marks the session cookie Secure. Set when the server is HTTPS (WiFiSetupManager does this).
    void setHttps(bool https) { _https = https; }

private:
    static constexpr size_t SALT_BYTES = 16;
    static constexpr size_t HASH_BYTES = 32;
    static constexpr size_t TOKEN_BYTES = 16;
    static constexpr size_t TOKEN_HEX = TOKEN_BYTES * 2;
    static constexpr uint32_t PBKDF2_ITERATIONS = 10000;
    // After this many wrong passwords in a row, one attempt per LOCKOUT_S.
    static constexpr uint32_t FREE_ATTEMPTS = 5;
    static constexpr uint32_t LOCKOUT_S = 30;

    SemaphoreHandle_t _mutex = nullptr;
    bool _https = false;
    bool _hasPassword = false;
    uint8_t _salt[SALT_BYTES] = {};
    uint8_t _hash[HASH_BYTES] = {};
    char _session[TOKEN_HEX + 1] = {};
    char _apiKey[TOKEN_HEX + 1] = {};
    uint32_t _failures = 0;
    int64_t _lastFailureUs = 0;

    bool matchesSession(const char* token);
    bool matchesApiKey(const char* token);
    // Copies the current token out under the lock.
    void sessionToken(char out[TOKEN_HEX + 1]);
    void apiKey(char out[TOKEN_HEX + 1]);
    bool rotateSession();
    bool regenerateApiKey();

    bool persist();
    static bool derive(const char* password, const uint8_t* salt, uint8_t out[HASH_BYTES]);
    static void randomHex(char out[TOKEN_HEX + 1]);
    static bool equalConstantTime(const uint8_t* a, const uint8_t* b, size_t len);

    void refuse(httpd_req_t* req);
    // nullptr token = an expired cookie, which signs this browser out.
    void formatCookie(char* out, size_t len, const char* token) const;

    static esp_err_t handleLoginPage(httpd_req_t* req);
    static esp_err_t handleLogin(httpd_req_t* req);
    static esp_err_t handleLogout(httpd_req_t* req);
    static esp_err_t handleAccessInfo(httpd_req_t* req);
    static esp_err_t handleChangePassword(httpd_req_t* req);
    static esp_err_t handleRegenerateApiKey(httpd_req_t* req);
};

extern AccessGuardClass Access;
