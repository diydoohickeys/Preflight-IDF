#ifndef WIFI_SETUP_DEVICE_CERT_H
#define WIFI_SETUP_DEVICE_CERT_H

#include "esp_http_server.h"
#include <string>

namespace wifisetup {

// The HTTPS server's certificate, kept in NVS namespace "pf_tls" (encrypted when the consumer
// enables NVS encryption). Until the user signs it, it is self-signed ECDSA P-256 made on the
// device, valid for <hostname> and <hostname>.local, and remade when the hostname changes.
// tools/preflight-certs.ps1 replaces it with one signed by the user's own CA: it fetches a CSR for
// the device's key (the key never leaves the device), signs it and uploads the certificate.
// Both strings come back NUL-terminated PEM, as esp_https_server wants (length + 1).
bool loadOrCreateDeviceCert(const std::string& hostname, std::string& certPem, std::string& keyPem);

// Forgets everything here, the HTTPS choice included (factory reset).
void clearDeviceCert();
// Drops a CA-signed certificate and its CA; a self-signed one is made at the next start.
void revertToSelfSigned();

// The user's choice on /setup, off until they turn it on. Read when the server starts.
bool httpsChosen();
bool setHttpsChosen(bool on);

struct DeviceCertInfo {
    bool signedByCa = false;
    std::string subject;
    std::string issuer;
    std::string validTo;  // YYYY-MM-DD
};
// The certificate the server presents; false before HTTPS has first started.
bool deviceCertInfo(DeviceCertInfo& out);

// A PKCS#10 request for the device's own key, named <hostname>.local. The CA adds the names and
// addresses it vouches for.
bool makeDeviceCsr(std::string& csrPem, std::string& hostname, std::string& error);

// Installs a CA-signed certificate for the device's key, with the CA's certificate, which becomes
// the device's trust anchor for its own outbound TLS (MQTT). Used from the next server start.
bool installSignedDeviceCert(const std::string& certPem, const std::string& caPem, std::string& error);

// The CA installed with a signed certificate; false while the device is self-signed.
bool loadTrustedCa(std::string& caPem);

// POST /api/access/https, GET /api/access/csr, GET|POST /api/access/certificate,
// POST /api/access/certificate/reset. Each change restarts the device. Registered by
// WiFiSetupManager when the consumer offers HTTPS (WiFiSetupConfig::offerHttps).
void registerDeviceCertEndpoints(httpd_handle_t server);

}  // namespace wifisetup

#endif  // WIFI_SETUP_DEVICE_CERT_H
