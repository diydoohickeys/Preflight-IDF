#ifndef WIFI_SETUP_SERVER_HOST_H
#define WIFI_SETUP_SERVER_HOST_H

#include "esp_http_server.h"
#include <cstdint>
#include <string>

namespace wifisetup {

struct ServerOptions {
    uint16_t httpPort = 80;
    uint16_t httpsPort = 443;
    bool https = false;
    std::string hostname;          // names the certificate
    uint16_t maxUriHandlers = 176;
    size_t stackSize = 8192;
    uint16_t maxOpenSockets = 12;
};

// The one server every route is registered on: plain HTTP, or HTTPS plus a tiny port-80
// server that answers every request with a redirect to it. HTTPS falls back to HTTP (logged)
// when the certificate can't be made or CONFIG_ESP_HTTPS_SERVER_ENABLE is off.
//
// In HTTPS mode the main server gets three fewer sockets than maxOpenSockets: the redirect
// server costs one session plus its listen and control sockets, so the pair draws the same
// total from CONFIG_LWIP_MAX_SOCKETS as HTTP alone. A TLS socket also costs far more RAM.
class ServerHost {
public:
    ServerHost() = default;
    ~ServerHost() { stop(); }
    ServerHost(const ServerHost&) = delete;
    ServerHost& operator=(const ServerHost&) = delete;

    bool start(const ServerOptions& options);
    void stop();

    httpd_handle_t handle() const { return _main; }
    bool isHttps() const { return _https; }

private:
    httpd_handle_t _main = nullptr;
    httpd_handle_t _redirect = nullptr;
    bool _https = false;
    uint16_t _httpsPort = 443;
    // esp_https_server parses these on every handshake, so they live as long as the server.
    std::string _certPem;
    std::string _keyPem;

    bool startHttp(const ServerOptions& options);
    bool startHttps(const ServerOptions& options);
    bool startRedirect(uint16_t httpPort);
    static esp_err_t handleRedirect(httpd_req_t* req);
};

}  // namespace wifisetup

#endif  // WIFI_SETUP_SERVER_HOST_H
