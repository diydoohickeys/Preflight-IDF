#pragma once

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <deque>
#include <vector>
#include <string>
#include <new>
#include <atomic>
#include <cstdarg>
#include <functional>
#include <memory>

enum LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARNING = 2,
    LOG_ERROR = 3
};

// Prefers PSRAM, keeping the internal heap for the network stack.
template <typename T>
class LogPSRAMAllocator {
public:
    using value_type = T;
    LogPSRAMAllocator() noexcept = default;
    template <typename U> LogPSRAMAllocator(const LogPSRAMAllocator<U>&) noexcept {}

    // Must throw, never return null: std::string memcpy()s into the result with no null check.
    T* allocate(std::size_t n) {
        void* ptr = nullptr;
#ifdef CONFIG_SPIRAM
        ptr = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (!ptr) ptr = malloc(n * sizeof(T));
        if (!ptr) throw std::bad_alloc();
        return static_cast<T*>(ptr);
    }
    void deallocate(T* p, std::size_t) noexcept { free(p); }
};

template <typename T, typename U>
bool operator==(const LogPSRAMAllocator<T>&, const LogPSRAMAllocator<U>&) { return true; }
template <typename T, typename U>
bool operator!=(const LogPSRAMAllocator<T>&, const LogPSRAMAllocator<U>&) { return false; }

using LogString = std::basic_string<char, std::char_traits<char>, LogPSRAMAllocator<char>>;

struct LogEntry {
    uint32_t timestamp;  // ms since boot
    LogLevel level;
    LogString message;

    LogEntry(uint32_t ts, LogLevel lvl, const char* msg)
        : timestamp(ts), level(lvl), message(msg) {}
    LogEntry(uint32_t ts, LogLevel lvl, const std::string& msg)
        : timestamp(ts), level(lvl), message(msg.c_str()) {}
};

class LoggerClass {
public:
    LoggerClass();

    // maxEntries 0 disables buffering; serial and the output callback still get every line.
    // enableWebSocket gates only the /logs page's live-tail connect; the consumer must serve /ws.
    void begin(size_t maxEntries = 100, bool enableSerial = true, bool enableWebSocket = true);

    // Registers GET /logs, GET /api/logs and POST /api/logs/clear.
    void registerEndpoints(httpd_handle_t server);

    void log(LogLevel level, const char* format, ...);

    void debug(const char* format, ...);
    void info(const char* format, ...);
    void warning(const char* format, ...);
    void error(const char* format, ...);

    void log(LogLevel level, const std::string& message);
    void debug(const std::string& message);
    void info(const std::string& message);
    void warning(const std::string& message);
    void error(const std::string& message);

    // A copy taken under the lock: the newest maxEntries (0 = all).
    std::vector<LogEntry> getEntries(size_t maxEntries = 0) const;

    // Most recent maxEntries (0 = all) as one contiguous string; the web endpoints stream instead.
    // Out of memory yields "[]" (JSON) or "" (HTML) rather than an exception.
    std::string getLogsJSON(size_t maxEntries = 0) const;

    std::string getLogsHTML(size_t maxEntries = 0) const;

    void clear();

    // Logs the previous boot's panic from a flash core dump (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH,
    // ELF format, coredump partition), then erases it. No-op otherwise; call once after begin().
    void reportLastCrash();

    size_t count() const;

    // Entries lost to an allocation failure or logged before begin(); shown by /logs and /api/logs.
    uint32_t droppedCount() const { return _droppedEntries.load(std::memory_order_relaxed); }

    void setSerialEnabled(bool enabled) { _serialEnabled = enabled; }

    void setWebSocketEnabled(bool enabled) { _webSocketEnabled = enabled; }
    bool isWebSocketEnabled() const { return _webSocketEnabled; }

    bool isSerialEnabled() const { return _serialEnabled; }

    void setMinLevel(LogLevel level) { _minLevel = level; }

    // timestamp is ms since boot.
    using LogOutputCallback = std::function<void(LogLevel level, uint32_t timestamp, const char* message)>;

    // Extra sink (e.g. USB CDC) called for every line on the logging task; nullptr disables.
    // Lines logged from inside the callback are buffered but not fed back to it.
    void setOutputCallback(LogOutputCallback callback);

    LogLevel getMinLevel() const { return _minLevel; }

private:
    static const char* TAG;

    // deque, not vector: at capacity every line pops the front, which moves every vector entry.
    std::deque<LogEntry, LogPSRAMAllocator<LogEntry>> _entries;
    size_t _maxEntries;
    // Counted, not logged (logging on the failure path re-enters addEntry); any task may bump it.
    std::atomic<uint32_t> _droppedEntries{0};
    bool _serialEnabled;
    bool _webSocketEnabled = true;
    LogLevel _minLevel;
    SemaphoreHandle_t _mutex;
    // Shared so a caller can hold it across the call while another task replaces it.
    std::shared_ptr<const LogOutputCallback> _outputCallback;

    // Takes a C string: copying the message would allocate on the path that must survive OOM.
    void addEntry(LogLevel level, const char* message);
    const char* getLevelString(LogLevel level) const;
    const char* getLevelColor(LogLevel level) const;
    void printToSerial(LogLevel level, const char* message);

    uint32_t getMillis() const {
        return (uint32_t)(esp_timer_get_time() / 1000);
    }

    // Take components, not a LogEntry, so the dropped notice needs no LogString allocation.
    void appendEntryJson(std::string& out, uint32_t timestamp, LogLevel level, const char* message) const;
    void appendEntryHtml(std::string& out, uint32_t timestamp, LogLevel level, const char* message) const;
    void appendDroppedNotice(std::string& out, bool json) const;

    // Chunked because one contiguous ~60-90 KB response fails on a merely fragmented heap.
    void streamLogsJson(httpd_req_t* req, size_t maxEntries) const;
    void streamLogsHtml(httpd_req_t* req, size_t maxEntries) const;

    static esp_err_t handleLogsPage(httpd_req_t* req);
    static esp_err_t handleLogsJson(httpd_req_t* req);
    static esp_err_t handleLogsClear(httpd_req_t* req);
};

extern LoggerClass Logger;
