#include "WiFiSetupManager.h"
#include "html/setup_html.h"
#include "html/wifi_saved_html.h"
#include "html/factory_reset_html.h"
#include "html/factory_reset_confirm_html.h"
#include "html/style_css.h"
#include "html/theme_js.h"
#include "SameOrigin.h"
#include "HttpdRoute.h"
#include "Logger.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace {

// Resumes after each substitution, so a value containing the token is not rescanned.
void replaceAll(std::string& text, const char* token, const std::string& value) {
    const size_t tokenLen = strlen(token);
    size_t at = 0;
    while ((at = text.find(token, at)) != std::string::npos) {
        text.replace(at, tokenLen, value);
        at += value.size();
    }
}

bool runningImagePendingVerify() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

}  // namespace

WiFiSetupManager::WiFiSetupManager(const WiFiSetupConfig& config)
    : _config(config)
    , _hostName("esp32-device")
    , _initialized(false)
    , _apMode(false)
    , _staNetif(nullptr)
    , _apNetif(nullptr)
    , _server(nullptr)
    , _dnsSocket(-1)
    , _dnsTask(nullptr)
    , _wifiEvents(nullptr)
    , _wifiHandlerInstance(nullptr)
    , _ipHandlerInstance(nullptr)
    , _dnsRunning(false)
    , _dnsTaskAlive(false)
    , _stateMutex(nullptr)
    , _scanCompleted(false)
    , _lastScanMs(0)
    , _restartRequested(false)
    , _restartTime(0)
    , _staWantsConnection(false)
    , _lastReconnectMs(0)
    , _reconnectFailures(0)
{
    _stateMutex = xSemaphoreCreateMutex();
}

WiFiSetupManager::~WiFiSetupManager() {
    stopDnsServer();
    stopWebServer();

    // The event loop holds these with `this` as their argument: a surviving handler
    // use-after-frees on the next WiFi event.
    if (_wifiHandlerInstance) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _wifiHandlerInstance);
        _wifiHandlerInstance = nullptr;
    }
    if (_ipHandlerInstance) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, _ipHandlerInstance);
        _ipHandlerInstance = nullptr;
    }

    if (_wifiEvents) {
        vEventGroupDelete(_wifiEvents);
        _wifiEvents = nullptr;
    }

    if (_stateMutex) {
        vSemaphoreDelete(_stateMutex);
        _stateMutex = nullptr;
    }
}

uint32_t WiFiSetupManager::getMillis() const {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void WiFiSetupManager::begin() {
    if (_initialized) {
        return;
    }

    // Set before anything can fail, so a new image that can't even start WiFi rolls back.
    _beginMs = getMillis();
    _firmwarePending = runningImagePendingVerify();
    if (_firmwarePending) {
        Logger.warning("New firmware on probation: kept once the device is reachable, rolled back otherwise");
    }

    _wifiEvents = xEventGroupCreate();

    if (!initNvs()) {
        Logger.error("NVS unavailable - settings cannot be stored or read");
    }

    if (!initWifi()) {
        Logger.error("WiFi init failed - setup manager inactive");
        return;
    }

    loadConfiguration();

    bool hasConfig = nvsHasKey(_config.deviceNameKey.c_str());

    // Scan only when the portal will show the list: esp_wifi_connect() scans internally,
    // so a scan before connecting would add seconds to every boot.
    if (!hasConfig || _hostName.empty()) {
        scanNetworks();
        startAPMode();
    } else {
        if (!connectToNetwork()) {
            scanNetworks();
            startAPMode();
        } else {
            startWebServer();
        }
    }

    _initialized = true;
    superviseFirmware();
}

void WiFiSetupManager::update() {
    superviseFirmware();

    if (_restartRequested && (getMillis() - _restartTime) > RESTART_DELAY_MS) {
        Logger.warning("Restarting");
        esp_restart();
    }

    if (_staWantsConnection && !_apMode && !isConnected()) {
        uint32_t now = getMillis();
        uint32_t backoff = RECONNECT_BASE_MS;
        for (uint8_t i = 0; i < _reconnectFailures && backoff < RECONNECT_MAX_MS; i++) {
            backoff *= 2;
        }
        if (backoff > RECONNECT_MAX_MS) backoff = RECONNECT_MAX_MS;

        if (now - _lastReconnectMs >= backoff) {
            _lastReconnectMs = now;
            if (_reconnectFailures < 255) _reconnectFailures++;
            Logger.info("Reconnecting to %s (attempt %u)",
                     _wifiSsid.c_str(), (unsigned)_reconnectFailures);
            esp_wifi_connect();
        }
    }
}

void WiFiSetupManager::superviseFirmware() {
    if (!_firmwarePending) {
        return;
    }

    // Reachable is enough: the setup portal and OTA page can repair a device that can't join WiFi.
    if (_server && (isConnected() || _apMode)) {
        _firmwarePending = false;
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            Logger.info("New firmware confirmed");
        } else {
            Logger.error("Confirming new firmware failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (_config.rollbackTimeoutMs && getMillis() - _beginMs >= _config.rollbackTimeoutMs) {
        _firmwarePending = false;
        Logger.error("New firmware never became reachable - rolling back");
        // Returns only on failure (no previous valid image).
        esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
        Logger.error("Rollback failed: %s", esp_err_to_name(err));
    }
}

bool WiFiSetupManager::isConnected() const {
    wifi_ap_record_t ap_info;
    return esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
}

std::string WiFiSetupManager::getIPAddress() const {
    if (isConnected() && _staNetif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(_staNetif, &ip_info) == ESP_OK) {
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            return std::string(ip_str);
        }
    } else if (_apMode && _apNetif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(_apNetif, &ip_info) == ESP_OK) {
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            return std::string(ip_str);
        }
    }
    return "0.0.0.0";
}

bool WiFiSetupManager::isInSetupMode() const {
    return _apMode;
}

void WiFiSetupManager::factoryReset() {
    Logger.warning("Factory reset - clearing WiFi settings");

    nvsClear();

    requestRestart();
}

// The time is stored before the flag: update() reads them in the other order.
void WiFiSetupManager::requestRestart() {
    _restartTime = getMillis();
    _restartRequested = true;
}

std::string WiFiSetupManager::getScannedNetworks() const {
    std::string copy;
    if (_stateMutex && xSemaphoreTake(_stateMutex, portMAX_DELAY) == pdTRUE) {
        copy = _scannedNetworks;
        xSemaphoreGive(_stateMutex);
    }
    return copy;
}

void WiFiSetupManager::recordScan(bool succeeded, const std::string& networks) {
    if (_stateMutex && xSemaphoreTake(_stateMutex, portMAX_DELAY) == pdTRUE) {
        if (succeeded) _scannedNetworks = networks;
        _scanCompleted = true;
        _lastScanMs = getMillis();
        xSemaphoreGive(_stateMutex);
    }
}

// In setup mode a scan never goes stale: scanning while the AP serves the phone hops
// channels, or fails outright on an AP-only radio.
bool WiFiSetupManager::getFreshScan(std::string& networks) const {
    const bool setupMode = isInSetupMode();
    bool fresh = false;
    if (_stateMutex && xSemaphoreTake(_stateMutex, portMAX_DELAY) == pdTRUE) {
        networks = _scannedNetworks;
        fresh = _scanCompleted && (setupMode || (getMillis() - _lastScanMs) < SCAN_MAX_AGE_MS);
        xSemaphoreGive(_stateMutex);
    }
    return fresh;
}

bool WiFiSetupManager::initNvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Unusable as it stands: erasing loses nothing that could still be read.
        Logger.warning("NVS partition needs erasing (%s)", esp_err_to_name(err));
        if (nvs_flash_erase() != ESP_OK) return false;
        err = nvs_flash_init();
    }
    // ESP_ERR_INVALID_STATE means the consumer already initialised it.
    return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}

// Never abort() the host application: only unrecoverable failures return false, and
// ESP_ERR_INVALID_STATE (the consumer already brought that piece up) is success.
bool WiFiSetupManager::initWifi() {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Logger.error("esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Logger.error("esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return false;
    }

    _staNetif = esp_netif_create_default_wifi_sta();
    _apNetif = esp_netif_create_default_wifi_ap();
    if (!_staNetif || !_apNetif) {
        Logger.error("Failed to create default WiFi netifs");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Logger.error("esp_wifi_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, this, &_wifiHandlerInstance);
    if (err != ESP_OK) {
        Logger.error("WiFi event handler register failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &ipEventHandler, this, &_ipHandlerInstance);
    if (err != ESP_OK) {
        Logger.error("IP event handler register failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

void WiFiSetupManager::wifiEventHandler(void* arg, esp_event_base_t eventBase,
                                        int32_t eventId, void* eventData) {
    auto* self = static_cast<WiFiSetupManager*>(arg);

    if (eventId == WIFI_EVENT_STA_START) {
        // No auto-connect: connectToNetwork() connects explicitly, so a scan's STA start
        // does not trigger a connection attempt.
    } else if (eventId == WIFI_EVENT_STA_DISCONNECTED) {
        if (self->_wifiEvents) {
            xEventGroupSetBits(self->_wifiEvents, WIFI_FAIL_BIT);
        }
        // update() owns the retry; retrying here would hammer esp_wifi_connect() from
        // the event loop on every disconnect event.
        self->_lastReconnectMs = self->getMillis();
        Logger.info("WiFi disconnected");
    } else if (eventId == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*)eventData;
        Logger.debug("Station " MACSTR " joined, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (eventId == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*)eventData;
        Logger.debug("Station " MACSTR " left, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

void WiFiSetupManager::ipEventHandler(void* arg, esp_event_base_t eventBase,
                                      int32_t eventId, void* eventData) {
    auto* self = static_cast<WiFiSetupManager*>(arg);
    ip_event_got_ip_t* event = (ip_event_got_ip_t*)eventData;

    Logger.info("Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

    if (self->_wifiEvents) {
        xEventGroupSetBits(self->_wifiEvents, WIFI_CONNECTED_BIT);
    }

    self->_reconnectFailures = 0;

    if (self->_config.statusCallback) {
        self->_config.statusCallback->onConnected(event->ip_info.ip);
    }
}

void WiFiSetupManager::scanNetworks(bool manageRadio) {
    if (_config.statusCallback) {
        _config.statusCallback->onScanStart();
    }

    if (manageRadio) {
        esp_err_t merr = esp_wifi_set_mode(WIFI_MODE_STA);
        if (merr == ESP_OK) merr = esp_wifi_start();
        if (merr != ESP_OK) {
            Logger.error("Could not bring the radio up for a scan: %s", esp_err_to_name(merr));
            recordScan(false);
            if (_config.statusCallback) _config.statusCallback->onScanComplete(0);
            return;
        }

        // Let the radio settle before scanning.
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    wifi_scan_config_t scan_config = {
        .ssid = nullptr,
        .bssid = nullptr,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = { .min = 100, .max = 300 },
            .passive = 0
        },
        .home_chan_dwell_time = 0,
        .channel_bitmap = {0, 0},
        .coex_background_scan = false
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        Logger.error("WiFi scan failed: %s", esp_err_to_name(err));
        recordScan(false);
        if (manageRadio) esp_wifi_stop();
        if (_config.statusCallback) {
            _config.statusCallback->onScanComplete(0);
        }
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) {
        Logger.info("No networks found");
        recordScan(true);
        if (manageRadio) esp_wifi_stop();
        if (_config.statusCallback) {
            _config.statusCallback->onScanComplete(0);
        }
        return;
    }

    // Bounds the allocation below.
    if (ap_count > 20) {
        ap_count = 20;
    }

    wifi_ap_record_t* ap_records = (wifi_ap_record_t*)malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!ap_records) {
        Logger.error("Failed to allocate memory for scan results (%d networks)", ap_count);
        recordScan(false);
        if (manageRadio) esp_wifi_stop();
        if (_config.statusCallback) {
            _config.statusCallback->onScanComplete(0);
        }
        return;
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) {
        Logger.error("Could not read scan results: %s", esp_err_to_name(err));
        free(ap_records);
        recordScan(false);
        if (manageRadio) esp_wifi_stop();
        if (_config.statusCallback) {
            _config.statusCallback->onScanComplete(0);
        }
        return;
    }

    std::string list;
    for (uint16_t i = 0; i < ap_count; i++) {
        if (i > 0) list += ",";
        char ssid[33] = {0};
        memcpy(ssid, ap_records[i].ssid, sizeof(ap_records[i].ssid) < 32
                                             ? sizeof(ap_records[i].ssid) : 32);
        list += listEscape(std::string(ssid));
        list += "|";
        list += std::to_string(ap_records[i].rssi);
        list += ap_records[i].authmode == WIFI_AUTH_OPEN ? "|0" : "|1";
    }
    recordScan(true, list);

    free(ap_records);
    if (manageRadio) esp_wifi_stop();

    Logger.info("Found %d networks", ap_count);

    if (_config.statusCallback) {
        _config.statusCallback->onScanComplete(ap_count);
    }
}

void WiFiSetupManager::loadConfiguration() {
    _hostName = nvsGetString(_config.deviceNameKey.c_str(), "esp32-device");
    _wifiSsid = nvsGetString(_config.ssidKey.c_str(), "");
    _wifiPassword = nvsGetString(_config.passwordKey.c_str(), "");

    Logger.info("Loaded config: hostname=%s, ssid=%s",
             _hostName.c_str(), _wifiSsid.c_str());
}

bool WiFiSetupManager::connectToNetwork() {
    if (_wifiSsid.empty()) {
        Logger.warning("No SSID configured");
        return false;
    }

    if (_config.statusCallback) {
        _config.statusCallback->onConnecting(_wifiSsid);
    }

    Logger.info("Connecting to WiFi: %s", _wifiSsid.c_str());

    // Reconfiguring a running radio can fail with ESP_ERR_WIFI_STATE.
    esp_wifi_stop();

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, _wifiSsid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, _wifiPassword.c_str(), sizeof(wifi_config.sta.password) - 1);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        Logger.error("Could not configure STA mode: %s", esp_err_to_name(err));
        return false;
    }

    // Before esp_wifi_start(), so DHCP carries it.
    if (!_hostName.empty() && _staNetif) {
        esp_err_t err = esp_netif_set_hostname(_staNetif, _hostName.c_str());
        if (err == ESP_OK) {
            Logger.info("Hostname set to: %s", _hostName.c_str());
        } else {
            Logger.warning("Failed to set hostname: %s", esp_err_to_name(err));
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        Logger.error("Could not start the radio: %s", esp_err_to_name(err));
        return false;
    }

    esp_wifi_connect();

    TickType_t slice = pdMS_TO_TICKS(_config.connectionTimeout);
    if (slice == 0) slice = 1;

    EventBits_t bits = 0;
    uint8_t attempts = 0;
    while (!(bits & (WIFI_CONNECTED_BIT | WIFI_FAIL_BIT)) && attempts < _config.maxConnectionAttempts) {
        if (_config.statusCallback) {
            _config.statusCallback->onConnectionProgress();
        }

        bits = xEventGroupWaitBits(
            _wifiEvents,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdTRUE,
            pdFALSE,
            slice
        );
        attempts++;
    }

    if (bits & WIFI_CONNECTED_BIT) {
        Logger.info("Connected to WiFi");
        _apMode = false;
        // Arms update()'s reconnect supervision.
        _staWantsConnection = true;
        _reconnectFailures = 0;
        startMdns();
        return true;
    } else {
        Logger.warning("Failed to connect to WiFi");
        esp_wifi_stop();
        return false;
    }
}

void WiFiSetupManager::startAPMode() {
    Logger.info("Starting Access Point Mode");

    _apMode = true;

    _staWantsConnection = false;

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.ap.ssid, _config.defaultAPName.c_str(), sizeof(wifi_config.ap.ssid) - 1);
    strncpy((char*)wifi_config.ap.password, _config.defaultAPPassword.c_str(), sizeof(wifi_config.ap.password) - 1);
    // ssid_len must describe the truncated copy, or the driver reads past the field.
    size_t apNameLen = _config.defaultAPName.length();
    if (apNameLen > sizeof(wifi_config.ap.ssid)) apNameLen = sizeof(wifi_config.ap.ssid);
    wifi_config.ap.ssid_len = (uint8_t)apNameLen;
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = _config.defaultAPPassword.length() >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        Logger.error("Could not start AP mode: %s", esp_err_to_name(err));
        _apMode = false;
        return;
    }

    startDnsServer();

    startWebServer();

    if (_config.statusCallback && _apNetif) {
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(_apNetif, &ip_info);
        _config.statusCallback->onAPMode(_config.defaultAPName, ip_info.ip);
    }

    Logger.info("AP Name: %s", _config.defaultAPName.c_str());
    Logger.info("Password: %s", _config.defaultAPPassword.c_str());
    Logger.info("IP: %s", getIPAddress().c_str());
}

void WiFiSetupManager::startWebServer() {
    if (_server) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = _config.webServerPort;
    // Shared with the consumer's whole route table, one slot per method+path. Overflow
    // is silent (the last registrations just fail), so the cap is generous.
    config.max_uri_handlers = 160;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    // The default 7 lets browser keep-alives squeeze out a consumer's long-lived
    // WebSocket. CONFIG_LWIP_MAX_SOCKETS=16 minus httpd's ~3 reserved caps this at 13;
    // the LRU purge reclaims the oldest idle socket at the cap.
    config.max_open_sockets = 12;
    config.lru_purge_enable = true;

    if (httpd_start(&_server, &config) != ESP_OK) {
        Logger.error("Failed to start web server");
        return;
    }

    httpd_uri_t setup_uri = wifisetup::route("/setup", HTTP_GET, handleSetupPage, this);
    httpd_uri_t style_uri = wifisetup::route("/wifi-setup-style.css", HTTP_GET, handleStyleCss, this);
    httpd_uri_t theme_uri = wifisetup::route("/wifi-setup-theme.js", HTTP_GET, handleThemeJs, this);
    httpd_uri_t networks_uri = wifisetup::route("/get-networks", HTTP_GET, handleGetNetworks, this);
    httpd_uri_t settings_uri = wifisetup::route("/get-current-settings", HTTP_GET, handleGetCurrentSettings, this);
    httpd_uri_t save_uri = wifisetup::route("/save-wifi", HTTP_POST, handleSaveWifi, this);
    httpd_uri_t reset_get_uri = wifisetup::route("/factory-reset", HTTP_GET, handleFactoryReset, this);
    httpd_uri_t reset_post_uri = wifisetup::route("/factory-reset", HTTP_POST, handleFactoryReset, this);

    httpd_register_uri_handler(_server, &setup_uri);
    httpd_register_uri_handler(_server, &style_uri);
    httpd_register_uri_handler(_server, &theme_uri);
    httpd_register_uri_handler(_server, &networks_uri);
    httpd_register_uri_handler(_server, &settings_uri);
    httpd_register_uri_handler(_server, &save_uri);
    httpd_register_uri_handler(_server, &reset_get_uri);
    httpd_register_uri_handler(_server, &reset_post_uri);

    // "/" and the OS captive-portal probe URLs.
    if (_apMode) {
        httpd_uri_t root_uri = wifisetup::route("/", HTTP_GET, handleRoot, this);
        httpd_uri_t gen204_uri = wifisetup::route("/generate_204", HTTP_GET, handleCaptiveRedirect, this);
        httpd_uri_t fwlink_uri = wifisetup::route("/fwlink", HTTP_GET, handleCaptiveRedirect, this);
        httpd_uri_t connect_uri = wifisetup::route("/connecttest.txt", HTTP_GET, handleCaptiveRedirect, this);
        httpd_uri_t hotspot_uri = wifisetup::route("/hotspot-detect.html", HTTP_GET, handleCaptiveRedirect, this);
        httpd_uri_t redirect_uri = wifisetup::route("/redirect", HTTP_GET, handleCaptiveRedirect, this);

        httpd_register_uri_handler(_server, &root_uri);
        httpd_register_uri_handler(_server, &gen204_uri);
        httpd_register_uri_handler(_server, &fwlink_uri);
        httpd_register_uri_handler(_server, &connect_uri);
        httpd_register_uri_handler(_server, &hotspot_uri);
        httpd_register_uri_handler(_server, &redirect_uri);
    }

    Logger.info("Web server started on port %d", _config.webServerPort);
}

void WiFiSetupManager::stopWebServer() {
    if (_server) {
        httpd_stop(_server);
        _server = nullptr;
    }
}

esp_err_t WiFiSetupManager::handleRoot(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/setup");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t WiFiSetupManager::handleCaptiveRedirect(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/setup");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t WiFiSetupManager::handleSetupPage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SETUP_HTML, strlen(SETUP_HTML));
    return ESP_OK;
}

esp_err_t WiFiSetupManager::handleStyleCss(httpd_req_t* req) {
    auto* self = static_cast<WiFiSetupManager*>(req->user_ctx);

    httpd_resp_set_type(req, "text/css");
    httpd_resp_send_chunk(req, STYLE_CSS, HTTPD_RESP_USE_STRLEN);

    // Appended after the static sheet so the overrides win on equal specificity.
    const WiFiSetupTheme* theme = self ? self->_config.theme : nullptr;
    if (theme) {
        std::string overrides = "\n:root {\n";
        auto addVar = [&overrides](const char* name, const std::string& value) {
            if (!value.empty()) {
                overrides += "  ";
                overrides += name;
                overrides += ": ";
                overrides += value;
                overrides += ";\n";
            }
        };
        addVar("--accent", theme->webPrimaryColor);
        addVar("--accent-dark", theme->webPrimaryDark);
        addVar("--bg", theme->webBackgroundColor);
        addVar("--card", theme->webSurfaceColor);
        addVar("--text", theme->webTextColor);
        addVar("--text-dim", theme->webTextSecondary);
        addVar("--border", theme->webBorderColor);
        for (const auto& var : theme->cssVariables) {
            overrides += "  --" + var.first + ": " + var.second + ";\n";
        }
        overrides += "}\n";

        // The select arrow is an SVG data URL: its fill needs its own override, with
        // the '#' percent-encoded or it terminates the URL.
        if (!theme->webPrimaryColor.empty()) {
            std::string color;
            for (char c : theme->webPrimaryColor) {
                if (c == '#') color += "%23";
                else color += c;
            }
            overrides += "select { background-image: url(\"data:image/svg+xml,%3csvg xmlns='http://www.w3.org/2000/svg' fill='";
            overrides += color;
            overrides += "' viewBox='0 0 16 16'%3e%3cpath d='M8 11L3 6h10z'/%3e%3c/svg%3e\"); }\n";
        }

        if (!theme->customCSS.empty()) {
            overrides += theme->customCSS;
            overrides += "\n";
        }

        httpd_resp_send_chunk(req, overrides.data(), overrides.size());
    }

    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t WiFiSetupManager::handleThemeJs(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/javascript");
    httpd_resp_send(req, THEME_JS, strlen(THEME_JS));
    return ESP_OK;
}

esp_err_t WiFiSetupManager::handleGetNetworks(httpd_req_t* req) {
    auto* self = static_cast<WiFiSetupManager*>(req->user_ctx);

    // Cold on a normal STA boot (the boot scan is lazy). This rescan blocks the shared
    // httpd for ~1-3 s, accepted for a rare user-initiated request.
    std::string networks;
    if (!self->getFreshScan(networks)) {
        self->scanNetworks(false);
        networks = self->getScannedNetworks();
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, networks.c_str(), networks.length());
    return ESP_OK;
}

esp_err_t WiFiSetupManager::handleGetCurrentSettings(httpd_req_t* req) {
    auto* self = static_cast<WiFiSetupManager*>(req->user_ctx);

    bool hasSettings = self->nvsHasKey(self->_config.deviceNameKey.c_str());
    std::string networkName = self->nvsGetString(self->_config.deviceNameKey.c_str(), "");
    std::string wifiSSID = self->nvsGetString(self->_config.ssidKey.c_str(), "");
    bool hasPassword = !self->nvsGetString(self->_config.passwordKey.c_str(), "").empty();

    // Whether a password is stored, never the password itself.
    std::string json = "{\"hasSettings\":";
    json += hasSettings ? "true" : "false";
    json += ",\"networkName\":\"" + jsonEscape(networkName) + "\"";
    json += ",\"wifiSSID\":\"" + jsonEscape(wifiSSID) + "\"";
    json += ",\"hasPassword\":";
    json += hasPassword ? "true}" : "false}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}

esp_err_t WiFiSetupManager::handleSaveWifi(httpd_req_t* req) {
    auto* self = static_cast<WiFiSetupManager*>(req->user_ctx);

    if (!wifisetup::isSameOrigin(req)) {
        wifisetup::sendCrossOriginRefused(req);
        return ESP_FAIL;
    }

    std::string body;
    if (!readFormBody(req, body)) {
        return ESP_FAIL;
    }

    std::string networkName = formValue(body, "network_name");
    std::string wifiNetwork = formValue(body, "wifi_network");
    std::string wifiPassword = formValue(body, "wifi_password");

    // mDNS labels are lowercase [a-z0-9-] and a space breaks resolution:
    // "My Device" becomes "my-device".
    {
        auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        size_t start = 0;
        while (start < networkName.size() && isSpace((unsigned char)networkName[start])) ++start;
        size_t end = networkName.size();
        while (end > start && isSpace((unsigned char)networkName[end - 1])) --end;
        networkName = networkName.substr(start, end - start);

        std::string clean;
        clean.reserve(networkName.size());
        for (char c : networkName) {
            if (c >= 'A' && c <= 'Z')                                 clean += (char)(c - 'A' + 'a');
            else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) clean += c;
            else                                                      clean += '-';
        }
        networkName = clean;
    }

    // Never store an unusable value: begin() treats any stored device name as
    // configured, so the device would fall back to AP mode on every boot, unexplained.
    const char* reason = nullptr;
    if (networkName.empty())                         reason = "Device name is required";
    else if (networkName.size() > HOSTNAME_MAX_BYTES)  reason = "Device name is too long";
    else if (wifiNetwork.empty())                    reason = "WiFi network is required";
    else if (wifiNetwork.size() > SSID_MAX_BYTES)      reason = "WiFi network name is too long (max 32 bytes)";
    else if (wifiPassword.size() > PASSWORD_MAX_BYTES) reason = "WiFi password is too long (max 63 bytes)";
    if (reason) {
        Logger.warning("Rejected WiFi settings: %s", reason);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, reason);
        return ESP_FAIL;
    }

    // The page leaves the field blank to keep the saved password of the saved network.
    const bool keepPassword = wifiPassword.empty() &&
                              wifiNetwork == self->nvsGetString(self->_config.ssidKey.c_str(), "");

    if (!self->nvsSetString(self->_config.deviceNameKey.c_str(), networkName) ||
        !self->nvsSetString(self->_config.ssidKey.c_str(), wifiNetwork) ||
        (!keepPassword && !self->nvsSetString(self->_config.passwordKey.c_str(), wifiPassword))) {
        Logger.error("Failed to store WiFi settings in NVS");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Could not save settings to flash");
        return ESP_FAIL;
    }

    Logger.info("WiFi settings saved: Host=%s, SSID=%s",
             networkName.c_str(), wifiNetwork.c_str());

    // Escaped before templating: both values are user-supplied and land in HTML.
    std::string html = WIFI_SAVED_HTML;
    replaceAll(html, "%NETWORK_NAME%", htmlEscape(networkName));
    replaceAll(html, "%WIFI_NETWORK%", htmlEscape(wifiNetwork));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html.c_str(), html.length());

    self->requestRestart();

    return ESP_OK;
}

esp_err_t WiFiSetupManager::handleFactoryReset(httpd_req_t* req) {
    auto* self = static_cast<WiFiSetupManager*>(req->user_ctx);

    if (req->method == HTTP_POST) {
        if (!wifisetup::isSameOrigin(req)) {
            wifisetup::sendCrossOriginRefused(req);
            return ESP_FAIL;
        }

        std::string body;
        if (!readFormBody(req, body)) {
            return ESP_FAIL;
        }

        // Cross-site forms are refused above; the fixed token the confirmation page posts
        // only stops an accidental reset, as any client on the network can send it.
        if (formValue(body, "confirm") != "RESET") {
            Logger.warning("Factory reset rejected - missing confirmation");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "Factory reset requires confirmation");
            return ESP_FAIL;
        }

        Logger.warning("Factory reset requested via web UI");

        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, FACTORY_RESET_HTML, strlen(FACTORY_RESET_HTML));

        self->factoryReset();
    } else {
        // GET is the confirmation page, never the action.
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, FACTORY_RESET_CONFIRM_HTML, strlen(FACTORY_RESET_CONFIRM_HTML));
    }

    return ESP_OK;
}

void WiFiSetupManager::startDnsServer() {
    if (_dnsTask) {
        return;
    }

    _dnsRunning = true;
    _dnsTaskAlive = true;
    if (xTaskCreate(dnsServerTask, "dns_server", 4096, this, 5, &_dnsTask) != pdPASS) {
        _dnsRunning = false;
        _dnsTaskAlive = false;
        _dnsTask = nullptr;
        Logger.error("Failed to create DNS task");
        return;
    }
    Logger.info("DNS server started");
}

// Ask the task to exit and wait: vTaskDelete() on a task blocked in lwIP recvfrom()
// leaks the netconn it waits on, and closing the socket under it races the same call.
void WiFiSetupManager::stopDnsServer() {
    _dnsRunning = false;

    if (_dnsTask) {
        for (int i = 0; i < 50 && _dnsTaskAlive; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (_dnsTaskAlive) {
            Logger.warning("DNS task did not exit; forcing it down");
            vTaskDelete(_dnsTask);
            _dnsTaskAlive = false;
        }
        _dnsTask = nullptr;
    }

    if (_dnsSocket >= 0) {
        close(_dnsSocket);
        _dnsSocket = -1;
    }
}

void WiFiSetupManager::dnsServerTask(void* pvParameters) {
    auto* self = static_cast<WiFiSetupManager*>(pvParameters);

    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(self->_config.dnsPort);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Error exits clear the handle, or startDnsServer() would refuse to restart.
    self->_dnsSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (self->_dnsSocket < 0) {
        Logger.error("Failed to create DNS socket");
        self->_dnsTask = nullptr;
        self->_dnsRunning = false;
        self->_dnsTaskAlive = false;
        vTaskDelete(nullptr);
        return;
    }

    // Bounds the recvfrom() block so the task sees _dnsRunning go false.
    struct timeval tv = { .tv_sec = 0, .tv_usec = DNS_RECV_TIMEOUT_MS * 1000 };
    setsockopt(self->_dnsSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (bind(self->_dnsSocket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        Logger.error("Failed to bind DNS socket");
        close(self->_dnsSocket);
        self->_dnsSocket = -1;
        self->_dnsTask = nullptr;
        self->_dnsRunning = false;
        self->_dnsTaskAlive = false;
        vTaskDelete(nullptr);
        return;
    }

    uint8_t buffer[DNS_BUFFER_SIZE];

    while (self->_dnsRunning) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int len = recvfrom(self->_dnsSocket, buffer, sizeof(buffer), 0,
                          (struct sockaddr*)&client_addr, &addr_len);

        if (len > 0) {
            self->processDnsRequest(self->_dnsSocket, buffer, len, &client_addr, addr_len);
        }
    }

    self->_dnsTaskAlive = false;
    vTaskDelete(nullptr);
}

// Answers A (and ANY) with the AP address, which is what makes the captive portal pop;
// other types get an empty NOERROR so the client falls back to A. Malformed queries are
// dropped: a client on the setup AP is untrusted input.
void WiFiSetupManager::processDnsRequest(int socket, uint8_t* buffer, int len,
                                         struct sockaddr_in* clientAddr, socklen_t addrLen) {
    if (len < DNS_HEADER_SIZE) return;

    // Only a standard query (QR=0, opcode=0) with one question can be answered by
    // appending a single record pointed at offset 12.
    const bool isQuery = (buffer[2] & 0x80) == 0;
    const uint8_t opcode = (buffer[2] >> 3) & 0x0F;
    const uint16_t qdcount = (uint16_t)((buffer[4] << 8) | buffer[5]);
    if (!isQuery || opcode != 0 || qdcount != 1) return;

    // QNAME: uncompressed labels up to a zero byte, then QTYPE and QCLASS.
    int pos = DNS_HEADER_SIZE;
    while (pos < len && buffer[pos] != 0) {
        if (buffer[pos] & 0xC0) return;
        pos += buffer[pos] + 1;
    }
    if (pos >= len) return;
    const int questionEnd = pos + 5;
    if (questionEnd > len) return;

    // The response is the header and question plus at most DNS_ANSWER_SIZE bytes; without
    // this bound any AP client can overflow `response` with a long question.
    if (questionEnd > DNS_BUFFER_SIZE - DNS_ANSWER_SIZE) return;

    const uint16_t qtype = (uint16_t)((buffer[pos + 1] << 8) | buffer[pos + 2]);
    const bool answer = qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY;

    esp_netif_ip_info_t ip_info = {};
    if (answer && (!_apNetif || esp_netif_get_ip_info(_apNetif, &ip_info) != ESP_OK)) {
        return;
    }

    uint8_t response[DNS_BUFFER_SIZE];
    memcpy(response, buffer, questionEnd);

    response[2] = 0x81;  // QR=1 (response), Opcode=0, AA=0, TC=0, RD=1
    response[3] = 0x80;  // RA=1, Z=0, RCODE=0 (no error)

    // Anything the query carried after the question (an EDNS0 OPT record, say) is not
    // echoed, so NSCOUNT/ARCOUNT are zero.
    response[6] = 0x00; response[7] = answer ? 0x01 : 0x00; // ANCOUNT
    response[8] = 0x00; response[9] = 0x00;   // NSCOUNT
    response[10] = 0x00; response[11] = 0x00; // ARCOUNT

    int answer_start = questionEnd;
    if (!answer) {
        sendto(socket, response, answer_start, 0,
               (struct sockaddr*)clientAddr, addrLen);
        return;
    }

    // NAME: pointer to the question at offset 12
    response[answer_start++] = 0xC0;
    response[answer_start++] = 0x0C;

    // TYPE A
    response[answer_start++] = 0x00;
    response[answer_start++] = 0x01;

    // CLASS IN
    response[answer_start++] = 0x00;
    response[answer_start++] = 0x01;

    // TTL 60 s
    response[answer_start++] = 0x00;
    response[answer_start++] = 0x00;
    response[answer_start++] = 0x00;
    response[answer_start++] = 0x3C;

    // RDLENGTH 4
    response[answer_start++] = 0x00;
    response[answer_start++] = 0x04;

    // RDATA: AP IPv4 address
    response[answer_start++] = (ip_info.ip.addr >> 0) & 0xFF;
    response[answer_start++] = (ip_info.ip.addr >> 8) & 0xFF;
    response[answer_start++] = (ip_info.ip.addr >> 16) & 0xFF;
    response[answer_start++] = (ip_info.ip.addr >> 24) & 0xFF;

    sendto(socket, response, answer_start, 0,
           (struct sockaddr*)clientAddr, addrLen);
}

// Reads all of content_len: httpd_req_recv() can return short, and anything unread is
// left on the socket. On false the 400 is sent; the caller returns ESP_FAIL to close it.
bool WiFiSetupManager::readFormBody(httpd_req_t* req, std::string& body) {
    if (req->content_len == 0 || req->content_len > MAX_FORM_BODY) {
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

std::string WiFiSetupManager::formValue(const std::string& body, const char* key) {
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

// application/x-www-form-urlencoded: '+' is a space and every reserved byte is %XX.
std::string WiFiSetupManager::urlDecode(const std::string& in) {
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

std::string WiFiSetupManager::jsonEscape(const std::string& in) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string WiFiSetupManager::htmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default:   out += c;
        }
    }
    return out;
}

// ',' and '|' delimit the network list but are legal in an SSID; the page unescapes.
std::string WiFiSetupManager::listEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\\' || c == ',' || c == '|') out += '\\';
        out += c;
    }
    return out;
}

std::string WiFiSetupManager::nvsGetString(const char* key, const std::string& defaultVal) {
    nvs_handle_t handle;
    if (nvs_open(_config.preferencesNamespace.c_str(), NVS_READONLY, &handle) != ESP_OK) {
        return defaultVal;
    }

    size_t required_size = 0;
    esp_err_t err = nvs_get_str(handle, key, nullptr, &required_size);
    if (err != ESP_OK || required_size == 0) {
        nvs_close(handle);
        return defaultVal;
    }

    std::string value;
    value.resize(required_size);
    err = nvs_get_str(handle, key, &value[0], &required_size);
    nvs_close(handle);

    if (err != ESP_OK) {
        return defaultVal;
    }

    // nvs_get_str's length includes the NUL.
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }

    return value;
}

bool WiFiSetupManager::nvsSetString(const char* key, const std::string& value) {
    nvs_handle_t handle;
    if (nvs_open(_config.preferencesNamespace.c_str(), NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_set_str(handle, key, value.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err == ESP_OK;
}

bool WiFiSetupManager::nvsClear() {
    nvs_handle_t handle;
    if (nvs_open(_config.preferencesNamespace.c_str(), NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err == ESP_OK;
}

bool WiFiSetupManager::nvsHasKey(const char* key) {
    nvs_handle_t handle;
    if (nvs_open(_config.preferencesNamespace.c_str(), NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t required_size = 0;
    esp_err_t err = nvs_get_str(handle, key, nullptr, &required_size);
    nvs_close(handle);

    return err == ESP_OK && required_size > 0;
}

void WiFiSetupManager::startMdns() {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        Logger.warning("mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    err = mdns_hostname_set(_hostName.c_str());
    if (err != ESP_OK) {
        Logger.warning("mDNS hostname set failed: %s", esp_err_to_name(err));
        return;
    }

    err = mdns_instance_name_set(_hostName.c_str());
    if (err != ESP_OK) {
        Logger.warning("mDNS instance name set failed: %s", esp_err_to_name(err));
    }

    err = mdns_service_add(nullptr, "_http", "_tcp", _config.webServerPort, nullptr, 0);
    if (err != ESP_OK) {
        Logger.warning("mDNS service add failed: %s", esp_err_to_name(err));
    }

    Logger.info("mDNS started: %s.local", _hostName.c_str());
}
