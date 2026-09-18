# Optional LVGL boot screen

`WiFiSetupBootUI` puts WiFi setup progress on an attached display: scanning, connecting,
the assigned IP, or the AP name and address to join. It implements `WiFiStatusCallback`, so
wiring it up is one assignment.

It is entirely optional — see *Headless builds* below.

## Using it

Bring LVGL up yourself (display driver, buffers, tick source, `lv_timer_handler` task —
this library does none of that), then:

```cpp
#include "WiFiSetupManager.h"
#include "WiFiSetupBootUI.h"

static WiFiSetupBootUI bootUI;

extern "C" void app_main(void) {
    // ... lv_init(), display driver, lv_display_create(), tick + handler task ...

    bootUI.initialize("MY DEVICE");        // optional custom title and theme

    WiFiSetupConfig config;
    config.defaultAPName = "MyDevice-Setup";
    config.statusCallback = &bootUI;       // progress now renders on screen

    static WiFiSetupManager wifiSetup(config);
    wifiSetup.begin();

    bootUI.cleanup();                      // remove the widgets, hand the screen back

    // ... your own UI ...
}
```

`initialize()` optionally takes the screen dimensions; without them it reads the default
display and sizes the title, text area and fonts to fit. `setDeviceName()` adds a `Device:`
line: `<name>.local` on the connected screen, the bare name on the AP screen.

## 🚨 Threading

`WiFiStatusCallback` methods fire from **whichever task reached the event** —
`onConnected()` runs on the system event-loop task, and the scan callbacks can run on the
HTTP server task. LVGL is not thread-safe.

So `WiFiSetupBootUI` is safe **only** where LVGL has no other concurrent owner, which is
the case it is built for: the boot window, before your own LVGL task starts. If your
application already runs an LVGL task while `begin()` is executing, do not point
`statusCallback` at this class — implement `WiFiStatusCallback` yourself and marshal each
update onto whichever task owns LVGL.

None of the callbacks block. In particular `onConnected()` does **not** pause to let you
read the IP: doing that inside the event loop would queue every other WiFi/IP/mDNS event
behind it. Keep the boot screen up for as long as you want from your own code.

## Object lifetime

The widgets belong to LVGL, which can free them behind this class's back — a consumer that
calls `lv_obj_clean(lv_scr_act())` or switches screens does exactly that. Each widget
carries an `LV_EVENT_DELETE` callback that nulls the matching member, so `cleanup()` and the
destructor can never delete a dangling pointer, whichever runs first.

## Theming

```cpp
WiFiSetupTheme theme;
theme.primaryColor    = 0xFF6B35;   // title text
theme.backgroundColor = 0x0A0A0A;
theme.surfaceColor    = 0x1A1A1A;   // text area
theme.textColor       = 0xF0F0F0;
theme.borderColor     = 0x333333;

bootUI.initialize("MY DEVICE", &theme);
```

The same struct carries the web-UI colours; see the README.

## Headless builds

Nothing to do. The component links LVGL only when the project already contains it
(`idf_component_optional_requires`, as `lvgl` or the component manager's `lvgl__lvgl`), and
`WiFiSetupBootUI.cpp` is wrapped in `#if __has_include(<lvgl.h>)`, so without LVGL it
compiles to an empty translation unit and nothing else in the library references it.

## LVGL configuration

The component ships no `lv_conf.h`; configure LVGL through its Kconfig or your own. The
layout uses Montserrat 10, 12, 14, 18 and 22 — enable all five, or the link fails.
