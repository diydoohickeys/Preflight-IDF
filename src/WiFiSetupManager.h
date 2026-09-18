#pragma once

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"  // For MACSTR, MAC2STR
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include <atomic>
#include <string>
#include <functional>
#include <map>

// Colours for the LVGL boot screen and the web portal; every field has a default.
struct WiFiSetupTheme {
    // LVGL colours, 0xRRGGBB
    uint32_t primaryColor = 0x4A90E2;
    uint32_t backgroundColor = 0x121212;
    uint32_t surfaceColor = 0x1E1E1E;
    uint32_t surfaceLight = 0x2A2A2A;
    uint32_t textColor = 0xE0E0E0;
    uint32_t borderColor = 0x444444;

    // Web colours, CSS "#RRGGBB"
    std::string webPrimaryColor = "#4A90E2";
    std::string webPrimaryDark = "#357ABD";
    std::string webBackgroundColor = "#121212";
    std::string webSurfaceColor = "#1E1E1E";
    std::string webTextColor = "#E0E0E0";
    std::string webTextSecondary = "#B0B0B0";
    std::string webBorderColor = "#444444";

    std::string customCSS = "";

    // CSS variable name (without the leading "--") -> CSS value
    std::map<std::string, std::string> cssVariables;
};

// onConnected() fires on the system event-loop task, where blocking stalls every other
// WiFi/IP event; the scan callbacks can also fire on the httpd task (/get-networks
// rescan). Never block, and marshal work on a single-owner resource (LVGL in
// particular) onto its owner.
class WiFiStatusCallback {
public:
    virtual ~WiFiStatusCallback() = default;

    virtual void onScanStart() = 0;
    virtual void onScanComplete(int networks) = 0;
    virtual void onConnecting(const std::string& ssid) = 0;
    virtual void onConnectionProgress() = 0;
    virtual void onConnected(esp_ip4_addr_t ip) = 0;
    virtual void onAPMode(const std::string& apName, esp_ip4_addr_t ip) = 0;
};

struct WiFiSetupConfig {
    std::string defaultAPName = "ESP32-Setup";
    std::string defaultAPPassword = "setup123";          // min 8 chars; shorter starts an open AP
    std::string preferencesNamespace = "wifi";           // NVS namespace
    std::string deviceNameKey = "host_name";
    std::string ssidKey = "ssid";
    std::string passwordKey = "password";
    uint16_t webServerPort = 80;
    uint16_t dnsPort = 53;
    uint8_t maxConnectionAttempts = 10;
    uint16_t connectionTimeout = 500;                    // ms per attempt
    WiFiStatusCallback* statusCallback = nullptr;
    WiFiSetupTheme* theme = nullptr;                     // nullptr = built-in defaults
    // A new OTA image on probation is kept once the device is reachable (joined WiFi, or
    // serving the setup portal), and rolled back if that hasn't happened within this many ms
    // of begin(). 0 = never roll back on a timeout.
    uint32_t rollbackTimeoutMs = 300000;
};

class WiFiSetupManager {
public:
    explicit WiFiSetupManager(const WiFiSetupConfig& config = WiFiSetupConfig());
    ~WiFiSetupManager();

    WiFiSetupManager(const WiFiSetupManager&) = delete;
    WiFiSetupManager& operator=(const WiFiSetupManager&) = delete;

    // Blocking: connects with the stored credentials, or starts the setup AP if that fails.
    void begin();

    // Call regularly from the main loop: the post-save restart and reconnects run here.
    void update();

    // "SSID|RSSI|secured,..." (secured is 1 or 0), with ',', '|' and '\' inside an SSID backslash-escaped
    std::string getScannedNetworks() const;

    bool isInSetupMode() const;
    bool isConnected() const;

    // STA IP when connected, AP IP in setup mode, else "0.0.0.0"
    std::string getIPAddress() const;

    std::string getHostName() const { return _hostName; }
    std::string getSSID() const { return _wifiSsid; }
    const WiFiSetupTheme* getTheme() const { return _config.theme; }

    // For adding the consumer's own routes
    httpd_handle_t getWebServer() { return _server; }

    // Clears the stored settings; the restart happens on a later update().
    void factoryReset();

private:
    WiFiSetupConfig _config;

    std::string _hostName;
    std::string _wifiSsid;
    std::string _wifiPassword;
    std::string _scannedNetworks;
    bool _initialized;
    std::atomic<bool> _apMode;

    esp_netif_t* _staNetif;
    esp_netif_t* _apNetif;
    httpd_handle_t _server;
    int _dnsSocket;
    TaskHandle_t _dnsTask;
    EventGroupHandle_t _wifiEvents;

    esp_event_handler_instance_t _wifiHandlerInstance;
    esp_event_handler_instance_t _ipHandlerInstance;

    std::atomic<bool> _dnsRunning;
    std::atomic<bool> _dnsTaskAlive;

    // Guards the scan result and its time: shared by the httpd task and the app task.
    SemaphoreHandle_t _stateMutex;
    bool _scanCompleted;
    uint32_t _lastScanMs;

    // Deferred to update() so the response is flushed first and the httpd task is not
    // held for the delay.
    std::atomic<bool> _restartRequested;
    std::atomic<uint32_t> _restartTime;

    bool _staWantsConnection;
    std::atomic<uint32_t> _lastReconnectMs;
    std::atomic<uint8_t> _reconnectFailures;

    bool _firmwarePending = false;
    uint32_t _beginMs = 0;

    static constexpr int WIFI_CONNECTED_BIT = BIT0;
    static constexpr int WIFI_FAIL_BIT = BIT1;

    // Lets the queued response reach the browser before the reboot.
    static constexpr uint32_t RESTART_DELAY_MS = 2000;
    static constexpr uint32_t RECONNECT_BASE_MS = 5000;
    static constexpr uint32_t RECONNECT_MAX_MS = 60000;
    // Outside setup mode, a scan result (empty or not) is served until it is this old.
    static constexpr uint32_t SCAN_MAX_AGE_MS = 15000;

    // Not MAX_SSID_LEN: esp_wifi_types_generic.h #defines that name.
    // 802.11 caps an SSID at 32 bytes, WPA2 a passphrase at 63.
    static constexpr size_t SSID_MAX_BYTES = 32;
    static constexpr size_t PASSWORD_MAX_BYTES = 63;
    static constexpr size_t HOSTNAME_MAX_BYTES = 63;

    // Fits a fully percent-encoded device name + SSID + passphrase while bounding
    // what one POST can make us allocate.
    static constexpr size_t MAX_FORM_BODY = 1024;
    // Each timeout is httpd's recv_wait_timeout (5 s by default).
    static constexpr int MAX_RECV_TIMEOUTS = 3;
    static constexpr int64_t FORM_BODY_DEADLINE_MS = 15000;

    // DNS: one UDP datagram, plus the fixed answer record appended to it.
    static constexpr int DNS_BUFFER_SIZE = 512;
    static constexpr int DNS_HEADER_SIZE = 12;
    static constexpr int DNS_ANSWER_SIZE = 16;
    static constexpr int DNS_RECV_TIMEOUT_MS = 250;
    static constexpr uint16_t DNS_TYPE_A = 1;
    static constexpr uint16_t DNS_TYPE_ANY = 255;

    bool initWifi();
    // manageRadio=false scans on the radio as-is and leaves it running, for a rescan
    // while associated (stopping the radio would drop the connection).
    void scanNetworks(bool manageRadio = true);
    void loadConfiguration();
    bool connectToNetwork();
    void startAPMode();
    void startWebServer();
    void stopWebServer();
    void startDnsServer();
    void stopDnsServer();
    void startMdns();
    void requestRestart();
    void superviseFirmware();
    uint32_t getMillis() const;

    static std::string urlDecode(const std::string& in);
    static std::string jsonEscape(const std::string& in);
    static std::string htmlEscape(const std::string& in);
    static std::string listEscape(const std::string& in);
    static std::string formValue(const std::string& body, const char* key);
    static bool readFormBody(httpd_req_t* req, std::string& body);
    // A failed scan keeps the previous list but still counts as a fresh result.
    void recordScan(bool succeeded, const std::string& networks = std::string());
    bool getFreshScan(std::string& networks) const;

    static bool initNvs();
    std::string nvsGetString(const char* key, const std::string& defaultVal = "");
    bool nvsSetString(const char* key, const std::string& value);
    bool nvsClear();
    bool nvsHasKey(const char* key);

    static esp_err_t handleSetupPage(httpd_req_t* req);
    static esp_err_t handleStyleCss(httpd_req_t* req);
    static esp_err_t handleThemeJs(httpd_req_t* req);
    static esp_err_t handleGetNetworks(httpd_req_t* req);
    static esp_err_t handleGetCurrentSettings(httpd_req_t* req);
    static esp_err_t handleSaveWifi(httpd_req_t* req);
    static esp_err_t handleFactoryReset(httpd_req_t* req);
    static esp_err_t handleCaptiveRedirect(httpd_req_t* req);
    static esp_err_t handleRoot(httpd_req_t* req);

    static void wifiEventHandler(void* arg, esp_event_base_t eventBase,
                                 int32_t eventId, void* eventData);
    static void ipEventHandler(void* arg, esp_event_base_t eventBase,
                               int32_t eventId, void* eventData);

    static void dnsServerTask(void* pvParameters);
    void processDnsRequest(int socket, uint8_t* buffer, int len,
                          struct sockaddr_in* clientAddr, socklen_t addrLen);
};
