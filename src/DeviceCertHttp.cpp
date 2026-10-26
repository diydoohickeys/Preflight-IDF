#include "DeviceCert.h"
#include "AccessGuard.h"
#include "FormBody.h"
#include "HttpdRoute.h"
#include "SameOrigin.h"
#include "Logger.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <cstdio>
#include <string>

namespace wifisetup {

namespace {

// A certificate and its CA, both PEM, percent-encoded by the form: ~2 x 2 x 900 bytes.
constexpr size_t MAX_CERT_BODY = 8192;
// Long enough for the response to leave before the server goes down with the restart.
constexpr uint64_t RESTART_DELAY_US = 1500 * 1000;

esp_err_t sendJson(httpd_req_t* req, const char* status, const std::string& json) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), json.size());
}

// Only for strings we made: PEM, hostnames, addresses and our own messages.
std::string jsonString(const std::string& in) {
    std::string out = "\"";
    for (char c : in) {
        if (c == '\n') {
            out += "\\n";
        } else if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if ((unsigned char)c >= 0x20) {
            out += c;
        }
    }
    return out + "\"";
}

std::string error(const std::string& message) {
    return "{\"error\":" + jsonString(message) + "}";
}

std::string staAddress() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return std::string();
    }
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    return ip;
}

// The new certificate is only read when the server starts, and the server can't be restarted
// from inside one of its own handlers.
void scheduleRestart() {
    static esp_timer_handle_t timer = nullptr;
    if (!timer) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) { esp_restart(); };
        args.name = "pf_cert_restart";
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_start_once(timer, RESTART_DELAY_US);
}

esp_err_t handleCsr(httpd_req_t* req) {
    if (!Access.allow(req)) {
        return ESP_OK;
    }
    std::string csr;
    std::string hostname;
    std::string message;
    if (!makeDeviceCsr(csr, hostname, message)) {
        return sendJson(req, "409 Conflict", error(message));
    }
    return sendJson(req, "200 OK",
                    "{\"csr\":" + jsonString(csr) + ",\"hostname\":" + jsonString(hostname) +
                        ",\"ip\":" + jsonString(staAddress()) + "}");
}

esp_err_t handleInfo(httpd_req_t* req) {
    if (!Access.allow(req)) {
        return ESP_OK;
    }
    const std::string https = std::string("{\"https\":") + (httpsChosen() ? "true" : "false");
    DeviceCertInfo info;
    if (!deviceCertInfo(info)) {
        return sendJson(req, "200 OK", https + ",\"present\":false}");
    }
    return sendJson(req, "200 OK",
                    https + ",\"present\":true,\"signedByCa\":" + (info.signedByCa ? "true" : "false") +
                        ",\"subject\":" + jsonString(info.subject) + ",\"issuer\":" + jsonString(info.issuer) +
                        ",\"validTo\":" + jsonString(info.validTo) + "}");
}

esp_err_t handleInstall(httpd_req_t* req) {
    if (!isSameOrigin(req)) {
        sendCrossOriginRefused(req);
        return ESP_FAIL;
    }
    if (!Access.allow(req)) {
        return ESP_FAIL;
    }
    std::string body;
    if (!readFormBody(req, body, MAX_CERT_BODY)) {
        return ESP_FAIL;
    }
    const std::string cert = formValue(body, "cert");
    const std::string ca = formValue(body, "ca");
    if (cert.empty() || ca.empty()) {
        return sendJson(req, "400 Bad Request", error("Send both cert and ca (PEM)"));
    }
    std::string message;
    if (!installSignedDeviceCert(cert, ca, message)) {
        Logger.warning("HTTPS: certificate refused: %s", message.c_str());
        return sendJson(req, "400 Bad Request", error(message));
    }
    sendJson(req, "200 OK", "{\"ok\":true,\"restarting\":true}");
    Logger.info("HTTPS: restarting to serve the new certificate");
    scheduleRestart();
    return ESP_OK;
}

esp_err_t handleHttps(httpd_req_t* req) {
    if (!isSameOrigin(req)) {
        sendCrossOriginRefused(req);
        return ESP_FAIL;
    }
    if (!Access.allow(req)) {
        return ESP_FAIL;
    }
    std::string body;
    if (!readFormBody(req, body)) {
        return ESP_FAIL;
    }
    const std::string value = formValue(body, "enabled");
    const bool on = value == "true" || value == "1";
    if (!setHttpsChosen(on)) {
        return sendJson(req, "500 Internal Server Error", error("Could not store the setting"));
    }
    sendJson(req, "200 OK", "{\"ok\":true,\"restarting\":true}");
    Logger.info("HTTPS turned %s; restarting", on ? "on" : "off");
    scheduleRestart();
    return ESP_OK;
}

esp_err_t handleReset(httpd_req_t* req) {
    if (!isSameOrigin(req)) {
        return sendCrossOriginRefused(req);
    }
    if (!Access.allow(req)) {
        return ESP_OK;
    }
    revertToSelfSigned();
    sendJson(req, "200 OK", "{\"ok\":true,\"restarting\":true}");
    Logger.info("HTTPS: back to a self-signed certificate; restarting");
    scheduleRestart();
    return ESP_OK;
}

}  // namespace

void registerDeviceCertEndpoints(httpd_handle_t server) {
    const httpd_uri_t routes[] = {
        route("/api/access/https", HTTP_POST, handleHttps, nullptr),
        route("/api/access/csr", HTTP_GET, handleCsr, nullptr),
        route("/api/access/certificate", HTTP_GET, handleInfo, nullptr),
        route("/api/access/certificate", HTTP_POST, handleInstall, nullptr),
        route("/api/access/certificate/reset", HTTP_POST, handleReset, nullptr),
    };
    for (const httpd_uri_t& r : routes) {
        if (httpd_register_uri_handler(server, &r) != ESP_OK) {
            Logger.error("Access: could not register %s (handler table full or route taken)", r.uri);
        }
    }
}

}  // namespace wifisetup
