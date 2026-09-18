#include "Logger.h"
#include "SameOrigin.h"
#include "HttpdRoute.h"
#include "html/logs_html.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#include "esp_partition.h"
#endif

namespace {
    const char* const ENTRIES_MARKER = "%LOG_ENTRIES%";

    // Set while this task runs the output callback, so a line it logs is not fed back to it.
    thread_local bool inOutputCallback = false;

    // Releases on every exit, including a bad_alloc thrown while held: a leaked lock blocks every later log call.
    class MutexLock {
    public:
        explicit MutexLock(SemaphoreHandle_t mutex)
            : _mutex(mutex)
            , _held(mutex != nullptr && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {}
        ~MutexLock() {
            if (_held) xSemaphoreGive(_mutex);
        }
        MutexLock(const MutexLock&) = delete;
        MutexLock& operator=(const MutexLock&) = delete;

        bool held() const { return _held; }

    private:
        SemaphoreHandle_t _mutex;
        bool _held;
    };

    void replaceAll(std::string& s, const std::string& from, const std::string& to) {
        if (from.empty()) return;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.length(), to);
            pos += to.length();
        }
    }

    // Only template text is substituted, never the entries: a logged message may contain a marker.
    std::string logsPageSlice(const char* text, size_t length, size_t total, bool webSocket) {
        std::string slice(text, length);
        replaceAll(slice, "%LOG_COUNT%", std::to_string(total));
        replaceAll(slice, "%LOG_WS%", webSocket ? "true" : "false");
        return slice;
    }
}

LoggerClass Logger;

const char* LoggerClass::TAG = "Logger";

LoggerClass::LoggerClass()
    : _maxEntries(100)
    , _serialEnabled(true)
    , _minLevel(LOG_INFO)
    , _mutex(nullptr)
{
}

void LoggerClass::begin(size_t maxEntries, bool enableSerial, bool enableWebSocket) {
    _maxEntries = maxEntries;
    _serialEnabled = enableSerial;
    _webSocketEnabled = enableWebSocket;

    if (_mutex == nullptr) {
        _mutex = xSemaphoreCreateMutex();
    }

    info("Logger started, buffering %u entries", (unsigned)maxEntries);
}

// Filter before formatting: a filtered debug() costs one compare, not a vsnprintf.
void LoggerClass::log(LogLevel level, const char* format, ...) {
    if (level < _minLevel) return;
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    addEntry(level, buffer);
}

void LoggerClass::debug(const char* format, ...) {
    if (LOG_DEBUG < _minLevel) return;
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    addEntry(LOG_DEBUG, buffer);
}

void LoggerClass::info(const char* format, ...) {
    if (LOG_INFO < _minLevel) return;
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    addEntry(LOG_INFO, buffer);
}

void LoggerClass::warning(const char* format, ...) {
    if (LOG_WARNING < _minLevel) return;
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    addEntry(LOG_WARNING, buffer);
}

void LoggerClass::error(const char* format, ...) {
    if (LOG_ERROR < _minLevel) return;
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    addEntry(LOG_ERROR, buffer);
}

void LoggerClass::log(LogLevel level, const std::string& message) {
    addEntry(level, message.c_str());
}

void LoggerClass::debug(const std::string& message) {
    addEntry(LOG_DEBUG, message.c_str());
}

void LoggerClass::info(const std::string& message) {
    addEntry(LOG_INFO, message.c_str());
}

void LoggerClass::warning(const std::string& message) {
    addEntry(LOG_WARNING, message.c_str());
}

void LoggerClass::error(const std::string& message) {
    addEntry(LOG_ERROR, message.c_str());
}

void LoggerClass::addEntry(LogLevel level, const char* message) {
    if (level < _minLevel) {
        return;
    }

    const uint32_t timestamp = getMillis();

    // Runs from any context, including C callbacks where an escaping exception terminate()s:
    // an allocation failure drops the line (counted). Never log from the catch - it re-enters.
    bool buffered = false;
    if (_mutex && _maxEntries > 0) {
        try {
            LogEntry newEntry(timestamp, level, message);
            MutexLock lock(_mutex);
            if (lock.held()) {
                while (_entries.size() >= _maxEntries) {
                    _entries.pop_front();
                }
                _entries.push_back(std::move(newEntry));
                buffered = true;
            }
        } catch (...) {
        }
    }
    // begin(0) turns buffering off by choice; that is not a loss.
    if (!buffered && _maxEntries > 0) {
        _droppedEntries.fetch_add(1, std::memory_order_relaxed);
    }

    if (_serialEnabled) {
        printToSerial(level, message);
    }

    if (inOutputCallback) {
        return;
    }
    try {
        std::shared_ptr<const LogOutputCallback> callback;
        {
            MutexLock lock(_mutex);
            callback = _outputCallback;
        }
        if (callback && *callback) {
            inOutputCallback = true;
            (*callback)(level, timestamp, message);
            inOutputCallback = false;
        }
    } catch (...) {
        inOutputCallback = false;
        // Only this sink misses the line; the buffered copy survives, so it is not counted as dropped.
    }
}

void LoggerClass::setOutputCallback(LogOutputCallback callback) {
    std::shared_ptr<const LogOutputCallback> next;
    if (callback) {
        next = std::make_shared<LogOutputCallback>(std::move(callback));
    }
    // Swapped, not assigned: the old callback is destroyed after the lock is released.
    MutexLock lock(_mutex);
    _outputCallback.swap(next);
}

std::vector<LogEntry> LoggerClass::getEntries(size_t maxEntries) const {
    std::vector<LogEntry> snapshot;
    MutexLock lock(_mutex);
    if (!lock.held()) return snapshot;
    const size_t total = _entries.size();
    const size_t start = (maxEntries > 0 && total > maxEntries) ? total - maxEntries : 0;
    snapshot.assign(_entries.begin() + static_cast<std::ptrdiff_t>(start), _entries.end());
    return snapshot;
}

void LoggerClass::printToSerial(LogLevel level, const char* message) {
    switch (level) {
        case LOG_DEBUG:
            ESP_LOGD(TAG, "%s", message);
            break;
        case LOG_INFO:
            ESP_LOGI(TAG, "%s", message);
            break;
        case LOG_WARNING:
            ESP_LOGW(TAG, "%s", message);
            break;
        case LOG_ERROR:
            ESP_LOGE(TAG, "%s", message);
            break;
        default:
            ESP_LOGI(TAG, "%s", message);
            break;
    }
}

const char* LoggerClass::getLevelString(LogLevel level) const {
    switch (level) {
        case LOG_DEBUG:   return "DEBUG";
        case LOG_INFO:    return "INFO";
        case LOG_WARNING: return "WARN";
        case LOG_ERROR:   return "ERROR";
        default:          return "UNKNOWN";
    }
}

const char* LoggerClass::getLevelColor(LogLevel level) const {
    switch (level) {
        case LOG_DEBUG:   return "#888888";
        case LOG_INFO:    return "#4A90E2";
        case LOG_WARNING: return "#FFA500";
        case LOG_ERROR:   return "#E74C3C";
        default:          return "#FFFFFF";
    }
}

void LoggerClass::appendEntryJson(std::string& out, uint32_t timestamp, LogLevel level,
                                  const char* message) const {
    static const char* kHex = "0123456789abcdef";

    out += "{";
    out += "\"timestamp\":" + std::to_string(timestamp) + ",";
    out += "\"level\":\"";
    out += getLevelString(level);
    out += "\",";

    // Control bytes must be \uXXXX-escaped: one raw byte invalidates the whole /api/logs payload.
    out += "\"message\":\"";
    for (const char* p = message; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
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
    out += "\"";

    out += "}";
}

void LoggerClass::appendEntryHtml(std::string& out, uint32_t timestamp, LogLevel level,
                                  const char* message) const {
    out += "<div class='log-entry'>";
    out += "<span class='timestamp'>" + std::to_string(timestamp) + "ms</span>";
    out += "<span class='level' style='color:";
    out += getLevelColor(level);
    out += "'>";
    out += getLevelString(level);
    out += "</span>";

    out += "<span class='message'>";
    for (const char* p = message; *p; ++p) {
        if (*p == '&') out += "&amp;";
        else if (*p == '<') out += "&lt;";
        else if (*p == '>') out += "&gt;";
        else out += *p;
    }
    out += "</span>";
    out += "</div>";
}

std::string LoggerClass::getLogsJSON(size_t maxEntries) const {
    try {
        std::string json = "[";
        appendDroppedNotice(json, /*json=*/true);
        bool wroteAny = json.size() > 1;

        {
            MutexLock lock(_mutex);
            if (lock.held()) {
                size_t count = _entries.size();
                size_t start = 0;

                if (maxEntries > 0 && count > maxEntries) {
                    start = count - maxEntries;
                }

                for (size_t i = start; i < count; i++) {
                    if (wroteAny) json += ",";
                    wroteAny = true;
                    const LogEntry& entry = _entries[i];
                    appendEntryJson(json, entry.timestamp, entry.level, entry.message.c_str());
                }
            }
        }

        json += "]";
        return json;
    } catch (...) {
        return "[]";
    }
}

std::string LoggerClass::getLogsHTML(size_t maxEntries) const {
    try {
        std::string entriesHtml;
        appendDroppedNotice(entriesHtml, /*json=*/false);
        size_t totalEntries = 0;

        {
            MutexLock lock(_mutex);
            if (lock.held()) {
                totalEntries = _entries.size();
                size_t start = 0;
                if (maxEntries > 0 && totalEntries > maxEntries) {
                    start = totalEntries - maxEntries;
                }

                for (size_t i = start; i < totalEntries; i++) {
                    const LogEntry& entry = _entries[i];
                    appendEntryHtml(entriesHtml, entry.timestamp, entry.level, entry.message.c_str());
                }
            }
        }

        // logs_html.h is generated from logs.html by extras/convert_html.py; edit the .html.
        const char* tpl = LOGS_HTML;
        const char* mark = strstr(tpl, ENTRIES_MARKER);
        if (!mark) {
            return logsPageSlice(tpl, strlen(tpl), totalEntries, _webSocketEnabled);
        }
        const char* after = mark + strlen(ENTRIES_MARKER);
        std::string html = logsPageSlice(tpl, (size_t)(mark - tpl), totalEntries, _webSocketEnabled);
        html += entriesHtml;
        html += logsPageSlice(after, strlen(after), totalEntries, _webSocketEnabled);
        return html;
    } catch (...) {
        return std::string();
    }
}

size_t LoggerClass::count() const {
    MutexLock lock(_mutex);
    return lock.held() ? _entries.size() : 0;
}

void LoggerClass::clear() {
    {
        MutexLock lock(_mutex);
        if (lock.held()) {
            _entries.clear();
        }
    }

    info("Logs cleared");
}

void LoggerClass::reportLastCrash() {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (esp_core_dump_image_check() != ESP_OK) {
        return;
    }

    // A few hundred bytes: heap it rather than grow the caller's stack.
    esp_core_dump_summary_t* summary =
        (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
    if (!summary) {
        error("Crash report: no heap for coredump summary; leaving dump on flash");
        return;
    }

    bool decoded = false;
    if (esp_core_dump_get_summary(summary) == ESP_OK) {
        error("CRASH (prev boot): task '%s' faulted at PC=0x%08x - addr2line against the .elf",
              summary->exc_task, (unsigned)summary->exc_pc);
#if defined(__XTENSA__)
        // Only Xtensa fills the backtrace; on RISC-V the PC above is the anchor.
        uint32_t depth = summary->exc_bt_info.depth;
        if (depth > 16) depth = 16;
        for (uint32_t i = 0; i < depth; i++) {
            error("CRASH:   bt[%u] 0x%08x", (unsigned)i,
                  (unsigned)summary->exc_bt_info.bt[i]);
        }
        if (summary->exc_bt_info.corrupted) {
            warning("CRASH: backtrace flagged corrupted (partial/unreliable)");
        }
#endif
        decoded = true;
    } else {
        // get_summary() returns a bare ESP_FAIL for both a corrupt ELF and a failed mmap (which
        // needs a free 64 KB MMU window); probe the mmap to say which, as the fixes differ.
        size_t dumpAddr = 0, dumpSize = 0;
        esp_err_t ierr = esp_core_dump_image_get(&dumpAddr, &dumpSize);
        error("Crash report: coredump present but summary decode failed "
              "(image_get=%s addr=0x%08x size=%u)",
              esp_err_to_name(ierr), (unsigned)dumpAddr, (unsigned)dumpSize);

        const esp_partition_t* part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
        if (!part) {
            error("Crash report:   no coredump partition in the table");
        } else {
            const void* mapAddr = nullptr;
            esp_partition_mmap_handle_t mapHandle = 0;
            esp_err_t merr = esp_partition_mmap(part, 0, part->size,
                                                ESP_PARTITION_MMAP_DATA, &mapAddr, &mapHandle);
            if (merr == ESP_OK) {
                esp_partition_munmap(mapHandle);
                error("Crash report:   partition mmap OK - so it is the ELF that did not parse");
            } else {
                error("Crash report:   partition mmap FAILED (%s) - the dump is INTACT but "
                      "this firmware has no free MMU window to read it through",
                      esp_err_to_name(merr));
            }
        }
    }

    free(summary);

    if (decoded) {
        if (esp_core_dump_image_erase() != ESP_OK) {
            warning("Crash report: failed to erase coredump image");
        }
    } else {
        // Never erase a dump that could not be read: a later build may still decode it.
        warning("Crash report: KEEPING the coredump image (it could not be read, so it is "
                "not spent). It will report once per boot until a build decodes it.");
    }
#endif  // CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
}

// Default window is the most recent 300, which can hide the start of a boot (where the
// crash report prints). ?max=N widens it, ?max=0 returns everything.
static constexpr size_t DEFAULT_WEB_LOG_ENTRIES = 300;

// Keeps peak allocation at a few KB however many entries are requested.
static constexpr size_t LOG_STREAM_BATCH = 24;

// Reported as a synthetic entry, not a response field: /api/logs readers expect a bare
// array, and the gap must survive in a downloaded copy.
void LoggerClass::appendDroppedNotice(std::string& out, bool json) const {
    uint32_t dropped = droppedCount();
    if (dropped == 0) return;

    char notice[128];
    snprintf(notice, sizeof(notice),
             "[logger] %u earlier entries were dropped (out of memory, or logged before "
             "Logger.begin())", (unsigned)dropped);
    if (json) appendEntryJson(out, getMillis(), LOG_WARNING, notice);
    else      appendEntryHtml(out, getMillis(), LOG_WARNING, notice);
}

void LoggerClass::streamLogsJson(httpd_req_t* req, size_t maxEntries) const {
    httpd_resp_send_chunk(req, "[", 1);

    std::string batch;
    batch.reserve(4096);

    size_t index = 0;
    bool positioned = false;
    bool first = true;

    appendDroppedNotice(batch, /*json=*/true);
    if (!batch.empty()) {
        first = false;
        if (httpd_resp_send_chunk(req, batch.data(), batch.size()) != ESP_OK) return;
    }

    while (true) {
        batch.clear();
        size_t produced = 0;

        {
            MutexLock lock(_mutex);
            if (!lock.held()) break;
            size_t total = _entries.size();
            if (!positioned) {
                index = (maxEntries > 0 && total > maxEntries) ? total - maxEntries : 0;
                positioned = true;
            }
            // The mutex is released between batches, so trimming can shift indices (a repeated or
            // skipped line); holding it across a network send would stall every task that logs.
            for (; index < total && produced < LOG_STREAM_BATCH; index++, produced++) {
                if (!first) batch += ',';
                first = false;
                const LogEntry& entry = _entries[index];
                appendEntryJson(batch, entry.timestamp, entry.level, entry.message.c_str());
            }
        }

        if (!batch.empty() &&
            httpd_resp_send_chunk(req, batch.data(), batch.size()) != ESP_OK) {
            return;  // client gone; httpd has already aborted the response
        }
        if (produced < LOG_STREAM_BATCH) break;
    }

    httpd_resp_send_chunk(req, "]", 1);
    httpd_resp_send_chunk(req, nullptr, 0);
}

void LoggerClass::streamLogsHtml(httpd_req_t* req, size_t maxEntries) const {
    const char* tpl = LOGS_HTML;
    const char* mark = strstr(tpl, ENTRIES_MARKER);
    const size_t total = count();

    std::string head = logsPageSlice(tpl, mark ? (size_t)(mark - tpl) : strlen(tpl), total,
                                     _webSocketEnabled);
    if (httpd_resp_send_chunk(req, head.data(), head.size()) != ESP_OK) return;
    head.clear();
    head.shrink_to_fit();

    if (mark) {
        std::string batch;
        batch.reserve(4096);

        size_t index = 0;
        bool positioned = false;

        appendDroppedNotice(batch, /*json=*/false);
        if (!batch.empty() &&
            httpd_resp_send_chunk(req, batch.data(), batch.size()) != ESP_OK) {
            return;
        }

        while (true) {
            batch.clear();
            size_t produced = 0;

            {
                MutexLock lock(_mutex);
                if (!lock.held()) break;
                size_t entryCount = _entries.size();
                if (!positioned) {
                    index = (maxEntries > 0 && entryCount > maxEntries) ? entryCount - maxEntries : 0;
                    positioned = true;
                }
                for (; index < entryCount && produced < LOG_STREAM_BATCH; index++, produced++) {
                    const LogEntry& entry = _entries[index];
                    appendEntryHtml(batch, entry.timestamp, entry.level, entry.message.c_str());
                }
            }

            if (!batch.empty() &&
                httpd_resp_send_chunk(req, batch.data(), batch.size()) != ESP_OK) {
                return;
            }
            if (produced < LOG_STREAM_BATCH) break;
        }

        const char* after = mark + strlen(ENTRIES_MARKER);
        std::string tail = logsPageSlice(after, strlen(after), total, _webSocketEnabled);
        if (httpd_resp_send_chunk(req, tail.data(), tail.size()) != ESP_OK) return;
    }

    httpd_resp_send_chunk(req, nullptr, 0);
}

static size_t logWindowFromQuery(httpd_req_t* req) {
    size_t maxEntries = DEFAULT_WEB_LOG_ENTRIES;
    // Sized for the whole query string: a truncated read errors and ?max= is silently ignored.
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(query, "max", param, sizeof(param)) == ESP_OK) {
            int v = atoi(param);
            if (v > 0) maxEntries = (size_t)v;
            else if (v == 0) maxEntries = 0;
        }
    }
    return maxEntries;
}

// Out of memory mid-stream: ESP_FAIL closes the socket, so the client sees a truncated response
// instead of a complete-looking one, and no exception reaches httpd's C code.
esp_err_t LoggerClass::handleLogsPage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    try {
        Logger.streamLogsHtml(req, logWindowFromQuery(req));
    } catch (...) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t LoggerClass::handleLogsJson(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    try {
        Logger.streamLogsJson(req, logWindowFromQuery(req));
    } catch (...) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t LoggerClass::handleLogsClear(httpd_req_t* req) {
    if (!wifisetup::isSameOrigin(req)) {
        return wifisetup::sendCrossOriginRefused(req);
    }
    Logger.clear();
    const char* response = "{\"status\":\"success\",\"message\":\"Logs cleared\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

void LoggerClass::registerEndpoints(httpd_handle_t server) {
    if (!server) {
        return;
    }

    httpd_uri_t logs_page = wifisetup::route("/logs", HTTP_GET, handleLogsPage, nullptr);
    httpd_register_uri_handler(server, &logs_page);

    httpd_uri_t logs_json = wifisetup::route("/api/logs", HTTP_GET, handleLogsJson, nullptr);
    httpd_register_uri_handler(server, &logs_json);

    httpd_uri_t logs_clear = wifisetup::route("/api/logs/clear", HTTP_POST, handleLogsClear, nullptr);
    httpd_register_uri_handler(server, &logs_clear);

    info("Log endpoints registered (/logs, /api/logs)");
}
