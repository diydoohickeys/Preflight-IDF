#include "ServerHost.h"
#include "DeviceCert.h"
#include "HttpdRoute.h"
#include "Logger.h"
#include "sdkconfig.h"
#include "lwip/sockets.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#if CONFIG_ESP_HTTPS_SERVER_ENABLE
#include "esp_https_server.h"
#include "esp_tls.h"
#include "esp_log.h"
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wifisetup {

namespace {

// esp_https_server's own default: the handshake runs on the server task.
constexpr size_t TLS_MIN_STACK = 10240;
constexpr uint16_t REDIRECT_SERVER_SOCKETS = 1;
// The redirect server and the main HTTPS one are both httpd instances; each needs its own
// control port. HTTPD_SSL_CONFIG_DEFAULT takes the default + 1, so this one takes + 2.
constexpr uint16_t REDIRECT_CTRL_PORT_OFFSET = 2;

// Nagle off on every connection. httpd sends a chunked response as three small writes per chunk
// (size, data, CRLF), and over TLS each becomes its own record and segment; with Nagle on, each
// waits for the client's delayed ACK (~200 ms on Windows). A 9 KB chunked stylesheet took 2.8 s
// that way, against 13 ms for a 1.5 KB response sent whole.
void disableNagle(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

esp_err_t openPlainSession(httpd_handle_t, int fd) {
    disableNagle(fd);
    return ESP_OK;
}

#if CONFIG_ESP_HTTPS_SERVER_ENABLE
// esp_https_server owns open_fn for its TLS setup; its session hook is where the socket is reachable.
void onTlsSession(esp_https_server_user_cb_arg_t* arg) {
    if (!arg || arg->user_cb_state != HTTPD_SSL_USER_CB_SESS_CREATE || !arg->tls) {
        return;
    }
    int fd = -1;
    if (esp_tls_get_conn_sockfd(arg->tls, &fd) == ESP_OK && fd >= 0) {
        disableNagle(fd);
    }
}
#endif

void applyCommon(httpd_config_t& config, const ServerOptions& options) {
    config.max_uri_handlers = options.maxUriHandlers;
    config.stack_size = options.stackSize;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = options.maxOpenSockets;
    // The LRU purge reclaims the oldest idle socket at the cap, so browser keep-alives can't
    // squeeze out a long-lived WebSocket.
    config.lru_purge_enable = true;
}

}  // namespace

bool ServerHost::start(const ServerOptions& options) {
    if (_main) {
        return true;
    }
    if (options.https && startHttps(options)) {
        return true;
    }
    return startHttp(options);
}

void ServerHost::stop() {
    if (_redirect) {
        httpd_stop(_redirect);
        _redirect = nullptr;
    }
    if (_main) {
#if CONFIG_ESP_HTTPS_SERVER_ENABLE
        if (_https) {
            httpd_ssl_stop(_main);
        } else {
            httpd_stop(_main);
        }
#else
        httpd_stop(_main);
#endif
        _main = nullptr;
    }
    _https = false;
}

bool ServerHost::startHttp(const ServerOptions& options) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    applyCommon(config, options);
    config.server_port = options.httpPort;
    config.open_fn = openPlainSession;
    if (httpd_start(&_main, &config) != ESP_OK) {
        _main = nullptr;
        Logger.error("Failed to start web server");
        return false;
    }
    _https = false;
    Logger.info("Web server started on port %u (HTTP)", (unsigned)options.httpPort);
    return true;
}

bool ServerHost::startHttps(const ServerOptions& options) {
#if CONFIG_ESP_HTTPS_SERVER_ENABLE
    if (!loadOrCreateDeviceCert(options.hostname, _certPem, _keyPem)) {
        Logger.warning("HTTPS unavailable - serving plain HTTP");
        return false;
    }

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    applyCommon(config.httpd, options);
    config.httpd.stack_size = std::max(options.stackSize, TLS_MIN_STACK);
    config.httpd.max_open_sockets = options.maxOpenSockets > 4 ? options.maxOpenSockets - 3 : 1;
    config.port_secure = options.httpsPort;
    config.servercert = reinterpret_cast<const uint8_t*>(_certPem.c_str());
    config.servercert_len = _certPem.size() + 1;
    config.prvtkey_pem = reinterpret_cast<const uint8_t*>(_keyPem.c_str());
    config.prvtkey_len = _keyPem.size() + 1;
    config.user_cb = onTlsSession;
    // esp_https_server logs "performing session handshake" at INFO for every connection; a page
    // load opens several, which floods a log ring with lines that say nothing went wrong.
    esp_log_level_set("esp_https_server", ESP_LOG_WARN);
#if CONFIG_ESP_TLS_SERVER_SESSION_TICKETS
    // A returning browser resumes from a ticket instead of a full handshake (~0.5 s on an S3,
    // which has no ECC accelerator).
    config.session_tickets = true;
#endif

    if (httpd_ssl_start(&_main, &config) != ESP_OK) {
        _main = nullptr;
        Logger.error("Failed to start the HTTPS server - serving plain HTTP");
        return false;
    }
    _https = true;
    _httpsPort = options.httpsPort;
    Logger.info("Web server started on port %u (HTTPS)", (unsigned)options.httpsPort);

    if (!startRedirect(options.httpPort)) {
        Logger.warning("Port %u redirect unavailable - browse to https:// directly", (unsigned)options.httpPort);
    }
    return true;
#else
    (void)options;
    Logger.error("HTTPS requested but CONFIG_ESP_HTTPS_SERVER_ENABLE is off - serving plain HTTP");
    return false;
#endif
}

bool ServerHost::startRedirect(uint16_t httpPort) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = httpPort;
    config.ctrl_port = (uint16_t)(config.ctrl_port + REDIRECT_CTRL_PORT_OFFSET);
    config.max_open_sockets = REDIRECT_SERVER_SOCKETS;
    config.max_uri_handlers = 2;
    config.stack_size = 4096;
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
    // It only answers 302s and never writes flash, so its stack can leave internal RAM.
    config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.open_fn = openPlainSession;
    if (httpd_start(&_redirect, &config) != ESP_OK) {
        _redirect = nullptr;
        return false;
    }
    httpd_uri_t any = route("/*", HTTP_GET, handleRedirect, this);
    httpd_register_uri_handler(_redirect, &any);
    return true;
}

// Same host, https scheme, same path: a bookmark or a typed hostname lands on the HTTPS server.
// 302, not 301: browsers cache a 301 forever, which would outlive turning HTTPS off.
esp_err_t ServerHost::handleRedirect(httpd_req_t* req) {
    auto* self = static_cast<ServerHost*>(req->user_ctx);

    char host[96] = {};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK || host[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Host header");
        return ESP_OK;
    }
    // Drop a port, keeping an IPv6 literal's brackets intact.
    char* colon = strrchr(host, ':');
    if (colon && !strchr(colon, ']')) {
        *colon = '\0';
    }

    std::string location = "https://";
    location += host;
    if (self && self->_httpsPort != 443) {
        char port[8];
        snprintf(port, sizeof(port), ":%u", (unsigned)self->_httpsPort);
        location += port;
    }
    location += req->uri;

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location.c_str());
    return httpd_resp_send(req, nullptr, 0);
}

}  // namespace wifisetup
