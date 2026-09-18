#pragma once

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string>
#include <functional>

class OTAManager {
public:
    enum class Stage {
        IDLE,
        STARTING,
        IN_PROGRESS,
        COMPLETE,
        FAILED
    };

    // progress is a percentage, 0-100.
    using LEDProgressCallback = std::function<void(uint8_t progress)>;
    using ScreenProgressCallback = std::function<void(uint8_t progress, Stage stage)>;
    using StartCallback = std::function<void()>;
    using EndCallback = std::function<void(bool success)>;
    // Called after every received chunk, to keep a UI responsive during the transfer.
    using RefreshCallback = std::function<void()>;

    OTAManager();
    ~OTAManager();

    OTAManager(const OTAManager&) = delete;
    OTAManager& operator=(const OTAManager&) = delete;

    // Registers GET /update and POST /ota/upload; both require basic auth unless username or password is empty.
    // partitionLabel "" refuses filesystem uploads (400). Use it whenever that partition holds user data:
    // the upload is a raw overwrite with no confirmation and no undo.
    bool begin(httpd_handle_t server, const char* username = "", const char* password = "", const char* partitionLabel = "spiffs");

    // Call regularly; performs the reboot after a successful update.
    void loop();

    void setLEDProgressCallback(LEDProgressCallback callback);
    void setScreenProgressCallback(ScreenProgressCallback callback);
    void setStartCallback(StartCallback callback);
    void setEndCallback(EndCallback callback);
    void setRefreshCallback(RefreshCallback callback);
    // Default true: reboot 2 s after a successful update.
    void setAutoReboot(bool enable);

    Stage getCurrentStage() const { return _currentStage; }
    uint8_t getCurrentProgress() const { return _currentProgress; }
    bool isUpdating() const { return _currentStage == Stage::IN_PROGRESS; }
    bool isInitialized() const { return _initialized; }

private:
    bool _initialized;
    httpd_handle_t _server;
    Stage _currentStage;
    uint8_t _currentProgress;
    size_t _currentSize;
    size_t _totalSize;
    bool _autoReboot;
    bool _rebootRequested;
    uint32_t _rebootTime;
    std::string _username;
    std::string _password;
    std::string _expectedAuth;   // base64("user:pass")
    std::string _partitionLabel;

    esp_ota_handle_t _otaHandle;
    const esp_partition_t* _updatePartition;
    bool _otaInProgress;

    LEDProgressCallback _ledProgressCallback;
    ScreenProgressCallback _screenProgressCallback;
    StartCallback _startCallback;
    EndCallback _endCallback;
    RefreshCallback _refreshCallback;

    bool _pageRouteRegistered;
    bool _uploadRouteRegistered;

    void setupOTAEndpoints();
    bool authenticate(httpd_req_t* req);
    uint32_t getMillis() const { return (uint32_t)(esp_timer_get_time() / 1000); }

    static constexpr size_t UPLOAD_CHUNK = 4096;
    // Holds a multipart delimiter (CRLF + "--" + <=70-char boundary) across a read split.
    static constexpr size_t HOLD_MAX = 128;
    // The boundary line + part headers must end within this many body bytes.
    static constexpr size_t PART_HEADER_MAX = 1024;
    // Content-Length may exceed the partition by at most this much: the multipart framing around the image.
    static constexpr size_t MULTIPART_FRAMING_MAX = PART_HEADER_MAX + HOLD_MAX;
    // Consecutive receive timeouts before a stalled upload is abandoned.
    static constexpr int MAX_RECV_TIMEOUTS = 3;

    static esp_err_t handleUpdatePage(httpd_req_t* req);
    static esp_err_t handleOTAUpload(httpd_req_t* req);
};
