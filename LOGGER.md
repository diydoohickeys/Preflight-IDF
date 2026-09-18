# Logger

A buffered logger for ESP-IDF: writes through `ESP_LOG*` to the console, keeps recent
entries in a PSRAM-backed ring buffer, and serves them over HTTP.

## Quick start

```cpp
#include "Logger.h"

Logger.begin(300);          // ring-buffer size
Logger.setMinLevel(LOG_INFO);
Logger.reportLastCrash();   // decode the previous boot's core dump, if any

Logger.info("Application started");
Logger.error("Failed to connect: %s", ssid.c_str());

Logger.registerEndpoints(server);   // an httpd_handle_t
```

`begin(maxEntries, enableSerial, enableWebSocket)`:

| Argument | |
|---|---|
| `maxEntries` | Entries to buffer. **0 disables buffering** — output still reaches the console and the output callback, and `/logs` serves an empty list. |
| `enableSerial` | Mirror to `ESP_LOG*`. Default true. |
| `enableWebSocket` | Whether the `/logs` page tries to live-tail (see below). Default true. |

## Levels

`LOG_DEBUG` · `LOG_INFO` · `LOG_WARNING` · `LOG_ERROR`, filtered by `setMinLevel()`
(default `LOG_INFO`). A filtered call returns before formatting, so a suppressed
`debug()` costs one comparison — it does not format a string and throw it away.

Both a printf form and a `std::string` form exist for every level:

```cpp
Logger.debug("value=%d", x);
Logger.warning(std::string("something odd"));
```

## Endpoints

`registerEndpoints(httpd_handle_t)` adds:

| Route | |
|---|---|
| `GET /logs` | HTML view, newest at the bottom, colour-coded by level. |
| `GET /api/logs` | A JSON array of `{"timestamp":…,"level":"INFO","message":"…"}` |
| `POST /api/logs/clear` | Empty the buffer. A cross-origin request is refused with 403. |

**Both views return the last 300 entries by default.** Rendering a full buffer builds one
large response that a RAM-tight server cannot serve; retention is unaffected. `?max=N`
widens the window and `?max=0` returns everything — which is what you need to see the start
of a boot, including anything `reportLastCrash()` printed.

Responses are streamed in small chunks rather than built whole, so peak allocation is one
batch however many entries are requested.

### Dropped entries

Entries lost to an allocation failure, or logged before `begin()` was called, are reported
as a synthetic WARNING entry at the head of the rendered range - so a gap in the history
reads as a gap rather than as a working log that happens to be missing lines.

It is an entry rather than a field wrapping the response deliberately: `/api/logs` is read
by external tooling that expects a bare array, and the notice has to be visible to whoever
reads the log, including in a downloaded copy. `droppedCount()` returns the raw number.

### Live tail

The `/logs` page can follow a WebSocket at `/ws`. **This library does not register `/ws`** —
a consumer provides it, and is expected to broadcast `{"type":"log", …}` frames. In a build
with no such endpoint, pass `enableWebSocket=false` so the page does not report a failed
connect against a socket that does not exist.

The Arduino sister adds `attachWebSocket()` and a WebSocket parameter on
`registerEndpoints()`, as they take ESPAsyncWebServer types; the rest of the API is identical.

## Reading entries in code

```cpp
std::vector<LogEntry> recent = Logger.getEntries(50);   // newest 50; 0 = all
```

A copy taken under the logger's lock, so it is safe while other tasks log. The copy
allocates, so under memory pressure it throws `std::bad_alloc`.

## Crash reporting

```cpp
Logger.reportLastCrash();   // once, early, after begin()
```

If the previous run ended in a panic and core-dump-to-flash is enabled, this decodes the
stored dump and logs the faulting task, the program counter and (on Xtensa) a backtrace at
ERROR level — through this Logger, so it reaches `/logs` and any output callback with no
serial console attached. Resolve the addresses with `addr2line` against the firmware `.elf`.

Requires `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`, a `coredump` partition, and the ELF format
(`CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF`). A no-op otherwise, so it is safe to call
unconditionally.

A decoded dump is **erased** so it reports exactly once. A dump that could **not** be
decoded is deliberately **kept**: erasing it would destroy the only copy of the evidence and
guarantee the next panic is guesswork too, and a later build may decode it. The cost is one
warning per boot until it is read.

## Forwarding elsewhere

```cpp
Logger.setOutputCallback([](LogLevel level, uint32_t ts, const char* msg) {
    // e.g. push over USB CDC to a companion app
});
```

Called for every accepted entry, on whichever task logged it. Keep it fast and
non-blocking; anything slow here is paid by every log call. An exception it throws is
caught and discarded. A line it logs itself is buffered but not passed back to it, so it
cannot recurse.

## Memory

Entries are allocated from PSRAM where available, falling back to internal heap, so the
buffer does not compete with the network stack for internal DRAM.

⚠ **The allocator throws `std::bad_alloc` and the logger catches it** — see the
Requirements section of the README. Under `-fno-exceptions` the component does not compile;
with a zero emergency exception pool the catch never runs.

Losing a line under memory pressure is intentional: the console output still happens, and
the entry is counted and reported (see Dropped entries). The logger must never crash the device it is diagnosing.
