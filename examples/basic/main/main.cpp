// WiFi setup plus the log viewer (/logs) and OTA page (/update), headless.
// With no saved network (or when it can't be reached) the device opens the
// "ESP32-Setup" access point, password "setup123"; the captive portal serves /setup.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "WiFiSetupManager.h"
#include "OTAManager.h"
#include "Logger.h"

static WiFiSetupManager wifi;
static OTAManager ota;

extern "C" void app_main(void) {
    Logger.begin(200, /*enableSerial=*/true, /*enableWebSocket=*/false);   // no /ws here
    Logger.reportLastCrash();

    wifi.begin();

    httpd_handle_t server = wifi.getWebServer();
    Logger.registerEndpoints(server);
    ota.begin(server, "admin", "change-me", "");   // "" label = firmware only

    if (wifi.isConnected()) {
        Logger.info("Connected to %s - http://%s/setup",
                    wifi.getSSID().c_str(), wifi.getIPAddress().c_str());
    }

    for (;;) {
        wifi.update();
        ota.loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
