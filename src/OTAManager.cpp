#include "OTAManager.h"
#include "Logger.h"
#include "SameOrigin.h"
#include "HttpdRoute.h"
#include "html/ota_html.h"  // generated from extras/html_source/ota.html by extras/convert_html.py
#include <cstring>
#include <cstdio>
#include <cstdlib>

OTAManager::OTAManager()
    : _initialized(false)
    , _server(nullptr)
    , _currentStage(Stage::IDLE)
    , _currentProgress(0)
    , _currentSize(0)
    , _totalSize(0)
    , _autoReboot(true)
    , _rebootRequested(false)
    , _rebootTime(0)
    , _otaHandle(0)
    , _updatePartition(nullptr)
    , _otaInProgress(false)
    , _ledProgressCallback(nullptr)
    , _screenProgressCallback(nullptr)
    , _startCallback(nullptr)
    , _endCallback(nullptr)
    , _refreshCallback(nullptr)
    , _pageRouteRegistered(false)
    , _uploadRouteRegistered(false)
{
}

// Each route's user_ctx is `this`, so the routes go with it; the server must not have been stopped yet.
OTAManager::~OTAManager() {
    if (_pageRouteRegistered) {
        httpd_unregister_uri_handler(_server, "/update", HTTP_GET);
    }
    if (_uploadRouteRegistered) {
        httpd_unregister_uri_handler(_server, "/ota/upload", HTTP_POST);
    }
}

// Not mbedtls: that costs an extra REQUIRES, and its base64.h path differs between IDF 5 and 6.
static std::string base64Encode(const std::string& in) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t v = ((uint32_t)(unsigned char)in[i] << 16) |
                     ((uint32_t)(unsigned char)in[i + 1] << 8) |
                     ((uint32_t)(unsigned char)in[i + 2]);
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += kAlphabet[(v >> 6) & 0x3F];
        out += kAlphabet[v & 0x3F];
        i += 3;
    }

    const size_t rem = in.size() - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)(unsigned char)in[i] << 16;
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)(unsigned char)in[i] << 16) |
                     ((uint32_t)(unsigned char)in[i + 1] << 8);
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += kAlphabet[(v >> 6) & 0x3F];
        out += '=';
    }

    return out;
}

bool OTAManager::begin(httpd_handle_t server, const char* username, const char* password, const char* partitionLabel) {
    if (_initialized) {
        return true;
    }

    if (!server) {
        Logger.error("OTA: no web server provided!");
        return false;
    }

    _server = server;
    _username = username ? username : "";
    _password = password ? password : "";
    _partitionLabel = partitionLabel ? partitionLabel : "spiffs";

    _expectedAuth.clear();
    if (!_username.empty() && !_password.empty()) {
        _expectedAuth = base64Encode(_username + ":" + _password);
    }

    setupOTAEndpoints();

    Logger.info("OTA Manager initialized (filesystem updates: %s, auth: %s)",
                _partitionLabel.empty() ? "DISABLED"
                                        : ("partition '" + _partitionLabel + "'").c_str(),
                _expectedAuth.empty() ? "none" : "basic");
    _initialized = true;
    return true;
}

void OTAManager::loop() {
    if (!_initialized) {
        return;
    }

    if (_rebootRequested && _autoReboot && (getMillis() - _rebootTime > 2000)) {
        Logger.info("OTA: rebooting...");
        esp_restart();
    }
}

void OTAManager::setLEDProgressCallback(LEDProgressCallback callback) {
    _ledProgressCallback = callback;
}

void OTAManager::setScreenProgressCallback(ScreenProgressCallback callback) {
    _screenProgressCallback = callback;
}

void OTAManager::setStartCallback(StartCallback callback) {
    _startCallback = callback;
}

void OTAManager::setEndCallback(EndCallback callback) {
    _endCallback = callback;
}

void OTAManager::setRefreshCallback(RefreshCallback callback) {
    _refreshCallback = callback;
}

void OTAManager::setAutoReboot(bool enable) {
    _autoReboot = enable;
}

void OTAManager::setupOTAEndpoints() {
    httpd_uri_t update_page = wifisetup::route("/update", HTTP_GET, handleUpdatePage, this);
    _pageRouteRegistered = httpd_register_uri_handler(_server, &update_page) == ESP_OK;

    httpd_uri_t ota_upload = wifisetup::route("/ota/upload", HTTP_POST, handleOTAUpload, this);
    _uploadRouteRegistered = httpd_register_uri_handler(_server, &ota_upload) == ESP_OK;

    if (_pageRouteRegistered && _uploadRouteRegistered) {
        Logger.info("OTA: endpoints registered (/update, /ota/upload)");
    } else {
        Logger.error("OTA: could not register %s%s (handler table full or route already taken)",
                     _pageRouteRegistered ? "" : "/update ",
                     _uploadRouteRegistered ? "" : "/ota/upload");
    }
}

static const char* findInBuffer(const char* haystack, size_t haystackLen, const char* needle, size_t needleLen) {
    if (needleLen == 0 || needleLen > haystackLen) return nullptr;
    const char* last = haystack + (haystackLen - needleLen);
    for (const char* p = haystack; p <= last; p++) {
        p = static_cast<const char*>(memchr(p, needle[0], (size_t)(last - p) + 1));
        if (!p) break;
        if (memcmp(p, needle, needleLen) == 0) {
            return p;
        }
    }
    return nullptr;
}

// Writes "--" + the Content-Type boundary into `out`, or leaves it empty when absent or unusable.
// Returns whether the body is multipart at all.
static bool parseBoundary(httpd_req_t* req, char* out, size_t outLen) {
    out[0] = '\0';

    char contentType[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", contentType, sizeof(contentType)) != ESP_OK) {
        return false;
    }
    const char* start = strstr(contentType, "boundary=");
    if (!start) {
        return false;
    }
    start += 9;  // strlen("boundary=")

    size_t len;
    if (*start == '"') {
        start++;
        const char* end = strchr(start, '"');
        if (!end) {
            return true;
        }
        len = (size_t)(end - start);
    } else {
        len = strcspn(start, " \r\n;");
    }

    if (len > 0 && len + 3 <= outLen) {
        out[0] = '-';
        out[1] = '-';
        memcpy(out + 2, start, len);
        out[len + 2] = '\0';
    }
    return true;
}

// The page hides its Filesystem card unless OTA_FS_ENABLED is injected at the marker (inline, so no
// reveal flash and no extra route); a missing marker leaves it hidden, the safe default.
esp_err_t OTAManager::handleUpdatePage(httpd_req_t* req) {
    auto* self = static_cast<OTAManager*>(req->user_ctx);

    if (self && !self->authenticate(req)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");

    static const char kMarker[] = "<!--OTA_CAPS-->";
    const size_t htmlLen = strlen(OTA_HTML);
    const char* marker = findInBuffer(OTA_HTML, htmlLen, kMarker, sizeof(kMarker) - 1);
    if (!marker) {
        httpd_resp_send(req, OTA_HTML, htmlLen);
        return ESP_OK;
    }

    const bool fsEnabled = self && !self->_partitionLabel.empty();
    const char* caps = fsEnabled ? "<script>window.OTA_FS_ENABLED=true;</script>"
                                 : "<script>window.OTA_FS_ENABLED=false;</script>";

    httpd_resp_send_chunk(req, OTA_HTML, marker - OTA_HTML);
    httpd_resp_send_chunk(req, caps, HTTPD_RESP_USE_STRLEN);
    const char* tail = marker + (sizeof(kMarker) - 1);
    httpd_resp_send_chunk(req, tail, htmlLen - (tail - OTA_HTML));
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// Compares against the pre-encoded credential rather than decoding attacker-controlled input.
bool OTAManager::authenticate(httpd_req_t* req) {
    if (_username.empty() || _password.empty()) {
        return true;
    }

    bool ok = false;
    char* header = nullptr;
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len > 0 && len < 512) {
        header = (char*)malloc(len + 1);
        if (header && httpd_req_get_hdr_value_str(req, "Authorization", header, len + 1) == ESP_OK) {
            const char* kPrefix = "Basic ";
            if (strncmp(header, kPrefix, strlen(kPrefix)) == 0) {
                const char* got = header + strlen(kPrefix);
                while (*got == ' ') got++;
                ok = (_expectedAuth == got);
            }
        }
    }
    free(header);

    if (!ok) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"OTA\"");
        httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    }
    return ok;
}

// Every refusal before the body is read returns ESP_FAIL, so httpd closes the socket instead of draining it.
esp_err_t OTAManager::handleOTAUpload(httpd_req_t* req) {
    auto* self = static_cast<OTAManager*>(req->user_ctx);

    // Before any body byte is read, so an unauthenticated request never reaches esp_ota_write().
    if (!self->authenticate(req)) {
        Logger.warning("OTA: upload rejected - authentication required");
        return ESP_FAIL;
    }

    // A no-cors cross-site POST needs no preflight, so without credentials any page could flash the device.
    if (!wifisetup::isSameOrigin(req)) {
        Logger.warning("OTA: upload rejected - cross-origin request");
        wifisetup::sendCrossOriginRefused(req);
        return ESP_FAIL;
    }

    // Overlapping POSTs would share _otaHandle and interleave their writes into one partition.
    if (self->_currentStage == Stage::STARTING || self->_currentStage == Stage::IN_PROGRESS) {
        Logger.warning("OTA: upload rejected - an update is already running");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "An update is already in progress", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // A staged image is the boot target: a second upload would erase it, and a restart
    // mid-write boots the previous firmware instead.
    if (self->_currentStage == Stage::COMPLETE) {
        Logger.warning("OTA: upload rejected - an update is waiting for a restart");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "An update is already installed and waiting for a restart", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    const size_t totalSize = req->content_len;
    if (totalSize == 0) {
        Logger.error("OTA: upload rejected - no content");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }

    char query[64] = {0};
    bool isFilesystem = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(query, "type", param, sizeof(param)) == ESP_OK) {
            isFilesystem = (strcmp(param, "filesystem") == 0);
        }
    }

    if (isFilesystem && self->_partitionLabel.empty()) {
        Logger.warning("OTA: filesystem update refused - disabled on this device");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Filesystem updates are disabled on this device");
        return ESP_FAIL;
    }

    char boundary[80];
    const bool isMultipart = parseBoundary(req, boundary, sizeof(boundary));
    if (isMultipart && boundary[0] == '\0') {
        Logger.warning("OTA: upload rejected - unusable multipart boundary");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed multipart boundary");
        return ESP_FAIL;
    }

    // Any data subtype: the table may declare the label as littlefs rather than spiffs.
    const esp_partition_t* target = isFilesystem
        ? esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, self->_partitionLabel.c_str())
        : esp_ota_get_next_update_partition(nullptr);
    if (!target) {
        const char* msg = isFilesystem ? "Filesystem partition not found" : "No OTA partition";
        Logger.error("OTA: upload rejected - %s", msg);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
        return ESP_FAIL;
    }

    Logger.info("OTA: target partition '%s' = %u bytes (%u KB); incoming content-length = %u bytes",
                target->label, (unsigned)target->size, (unsigned)(target->size / 1024), (unsigned)totalSize);

    // Before anything is written: an overflow found mid-write would leave the old filesystem destroyed.
    // The body wraps the image in multipart framing; the exact limit is enforced on the bytes written.
    if (totalSize > target->size + MULTIPART_FRAMING_MAX) {
        Logger.warning("OTA: upload rejected - %u-byte body exceeds the '%s' partition (%u bytes)",
                       (unsigned)totalSize, target->label, (unsigned)target->size);
        char msg[96];
        snprintf(msg, sizeof(msg), "Image is larger than the '%s' partition (%u bytes)",
                 target->label, (unsigned)target->size);
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, msg);
        return ESP_FAIL;
    }

    Logger.info("OTA: upload starting - type=%s, content-length=%u bytes, multipart=%s",
                isFilesystem ? "filesystem" : "firmware", (unsigned)totalSize,
                isMultipart ? "yes" : "no");

    self->_currentSize = 0;
    self->_currentProgress = 0;
    self->_totalSize = totalSize;
    self->_updatePartition = target;
    self->_otaInProgress = false;
    self->_currentStage = Stage::STARTING;

    if (self->_screenProgressCallback) {
        self->_screenProgressCallback(0, Stage::STARTING);
    }
    if (self->_ledProgressCallback) {
        self->_ledProgressCallback(0);
    }
    if (self->_startCallback) {
        self->_startCallback();
    }

    // The start callback has fired, so every exit from here must fire the end callback.
    bool success = true;
    httpd_err_code_t failCode = HTTPD_500_INTERNAL_SERVER_ERROR;
    char failMsg[160] = {0};

    size_t received = 0;
    size_t written = 0;
    bool targetOpen = false;

    // Nothing destructive happens before the first image byte, so a malformed or abandoned body leaves the target intact.
    auto writeOut = [&](const char* p, size_t n) -> bool {
        if (n == 0) return true;
        if (written + n > target->size) {
            failCode = HTTPD_413_CONTENT_TOO_LARGE;
            snprintf(failMsg, sizeof(failMsg), "Image is larger than the '%s' partition (%u bytes)",
                     target->label, (unsigned)target->size);
            return false;
        }
        if (!targetOpen) {
            esp_err_t err;
            if (isFilesystem) {
                Logger.info("OTA: erasing partition '%s' (%u bytes)...", target->label, (unsigned)target->size);
                err = esp_partition_erase_range(target, 0, target->size);
            } else {
                err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &self->_otaHandle);
                self->_otaInProgress = (err == ESP_OK);
            }
            if (err != ESP_OK) {
                snprintf(failMsg, sizeof(failMsg), "%s failed: %s",
                         isFilesystem ? "Partition erase" : "OTA begin", esp_err_to_name(err));
                return false;
            }
            targetOpen = true;
            self->_currentStage = Stage::IN_PROGRESS;
        }
        esp_err_t werr = isFilesystem
            ? esp_partition_write(target, written, p, n)
            : esp_ota_write(self->_otaHandle, p, n);
        if (werr != ESP_OK) {
            Logger.error("OTA: %s write failed at offset %u: %s",
                         isFilesystem ? "filesystem" : "firmware",
                         (unsigned)written, esp_err_to_name(werr));
            snprintf(failMsg, sizeof(failMsg), "Flash write failed: %s", esp_err_to_name(werr));
            return false;
        }
        written += n;
        return true;
    };

    // Receives into `in`; up to HOLD_MAX held-back bytes sit just before it, so a delimiter split across
    // two reads is still found in one contiguous window.
    char* buffer = (char*)malloc(HOLD_MAX + UPLOAD_CHUNK);
    char* const in = buffer ? buffer + HOLD_MAX : nullptr;
    if (!buffer) {
        snprintf(failMsg, sizeof(failMsg), "Memory allocation failed");
        success = false;
    }

    // The image ends at the first delimiter; anything after it is drained unwritten.
    char delimiter[84] = {0};
    const size_t delimLen = isMultipart ? (size_t)snprintf(delimiter, sizeof(delimiter), "\r\n%s", boundary) : 0;

    size_t headerLen = 0;
    size_t holdLen = 0;
    bool headersDone = !isMultipart;
    bool imageEnded = false;
    int timeouts = 0;
    uint32_t lastProgressUpdate = 0;

    while (success && received < totalSize) {
        char* dest = headersDone ? in : in + headerLen;
        const int ret = httpd_req_recv(req, dest, headersDone ? UPLOAD_CHUNK : UPLOAD_CHUNK - headerLen);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < MAX_RECV_TIMEOUTS) {
            continue;
        }
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                Logger.error("OTA: client stalled at %u/%u bytes", (unsigned)received, (unsigned)totalSize);
                failCode = HTTPD_408_REQ_TIMEOUT;
                snprintf(failMsg, sizeof(failMsg), "Upload stalled");
            } else {
                Logger.error("OTA: receive error %d at %u/%u bytes", ret, (unsigned)received, (unsigned)totalSize);
                snprintf(failMsg, sizeof(failMsg), "Upload interrupted (network error %d)", ret);
            }
            success = false;
            break;
        }
        timeouts = 0;

        received += (size_t)ret;
        self->_currentSize = received;

        char* data = dest;
        size_t dataLen = (size_t)ret;

        // The boundary line and part headers can span reads, so they are gathered before the image begins.
        if (!headersDone) {
            headerLen += (size_t)ret;
            const size_t searchable = headerLen < PART_HEADER_MAX ? headerLen : PART_HEADER_MAX;
            const char* headerEnd = findInBuffer(in, searchable, "\r\n\r\n", 4);
            if (!headerEnd) {
                if (headerLen < PART_HEADER_MAX && received < totalSize) {
                    continue;
                }
                failCode = HTTPD_400_BAD_REQUEST;
                snprintf(failMsg, sizeof(failMsg), "Malformed multipart body - part headers not found");
                success = false;
                break;
            }
            const size_t boundaryLen = delimLen - 2;
            if ((size_t)(headerEnd - in) < boundaryLen || memcmp(in, boundary, boundaryLen) != 0) {
                failCode = HTTPD_400_BAD_REQUEST;
                snprintf(failMsg, sizeof(failMsg), "Malformed multipart body - opening boundary not found");
                success = false;
                break;
            }
            headersDone = true;
            data = const_cast<char*>(headerEnd) + 4;
            dataLen = headerLen - (size_t)(data - in);
            Logger.debug("OTA: multipart headers parsed, image starts at body offset %u", (unsigned)(data - in));
        }

        if (!imageEnded) {
            char* window = data - holdLen;
            const size_t windowLen = holdLen + dataLen;
            const char* delim = delimLen ? findInBuffer(window, windowLen, delimiter, delimLen) : nullptr;
            size_t emit;
            if (delim) {
                emit = (size_t)(delim - window);
                imageEnded = true;
            } else {
                emit = windowLen - (delimLen < windowLen ? delimLen : windowLen);
            }
            if (!writeOut(window, emit)) {
                success = false;
                break;
            }
            holdLen = imageEnded ? 0 : windowLen - emit;
            memmove(in - holdLen, window + emit, holdLen);
        }

        self->_currentProgress = (received * 100) / totalSize;

        uint32_t now = self->getMillis();
        if (now - lastProgressUpdate > 100) {
            Logger.debug("OTA: progress %d%%", self->_currentProgress);

            if (self->_screenProgressCallback) {
                self->_screenProgressCallback(self->_currentProgress, Stage::IN_PROGRESS);
            }
            if (self->_ledProgressCallback) {
                self->_ledProgressCallback(self->_currentProgress);
            }
            lastProgressUpdate = now;
        }

        if (self->_refreshCallback) {
            self->_refreshCallback();
        }

        vTaskDelay(1);
    }

    free(buffer);

    if (success && isMultipart && !imageEnded) {
        failCode = HTTPD_400_BAD_REQUEST;
        snprintf(failMsg, sizeof(failMsg), "Upload incomplete - closing boundary not found");
        success = false;
    } else if (success && written == 0) {
        failCode = HTTPD_400_BAD_REQUEST;
        snprintf(failMsg, sizeof(failMsg), "No image data received");
        success = false;
    }

    Logger.info("OTA: transfer done - %u bytes received, %u bytes written to '%s'",
                (unsigned)received, (unsigned)written, target->label);

    if (success && !isFilesystem) {
        esp_err_t err = esp_ota_end(self->_otaHandle);
        if (err != ESP_OK) {
            Logger.error("OTA: esp_ota_end failed: %s (image invalid, truncated, or corrupt)", esp_err_to_name(err));
            snprintf(failMsg, sizeof(failMsg), "Firmware image rejected: %s", esp_err_to_name(err));
            success = false;
        } else {
            err = esp_ota_set_boot_partition(target);
            if (err != ESP_OK) {
                Logger.error("OTA: esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
                snprintf(failMsg, sizeof(failMsg), "Could not set boot partition: %s", esp_err_to_name(err));
                success = false;
            }
        }
    } else if (!success && self->_otaInProgress) {
        esp_ota_abort(self->_otaHandle);
    }

    self->_otaInProgress = false;

    if (self->_endCallback) {
        self->_endCallback(success);
    }

    if (success) {
        self->_currentStage = Stage::COMPLETE;
        self->_currentProgress = 100;

        if (self->_screenProgressCallback) {
            self->_screenProgressCallback(100, Stage::COMPLETE);
        }
        if (self->_ledProgressCallback) {
            self->_ledProgressCallback(100);
        }

        httpd_resp_sendstr(req, "Update successful");

        if (self->_autoReboot) {
            Logger.info("OTA: update successful - rebooting shortly");
            self->_rebootRequested = true;
            self->_rebootTime = self->getMillis();
        } else {
            Logger.info("OTA: update successful - installed on the next restart");
        }
    } else {
        self->_currentStage = Stage::FAILED;

        if (self->_screenProgressCallback) {
            self->_screenProgressCallback(self->_currentProgress, Stage::FAILED);
        }

        const char* msg = failMsg[0] ? failMsg : "Update failed";
        Logger.error("OTA: update FAILED - %s", msg);
        httpd_resp_send_err(req, failCode, msg);
    }

    return success ? ESP_OK : ESP_FAIL;
}
