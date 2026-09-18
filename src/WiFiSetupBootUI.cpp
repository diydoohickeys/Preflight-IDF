#if __has_include(<lvgl.h>)

#include "WiFiSetupBootUI.h"
#include "Logger.h"
#include <cstdio>

WiFiSetupBootUI::WiFiSetupBootUI()
    : _textArea(nullptr)
    , _titleLabel(nullptr)
    , _initialized(false)
    , _screenWidth(0)
    , _screenHeight(0)
    , _primaryColor(UI_COLOR_PRIMARY)
    , _backgroundColor(UI_COLOR_BACKGROUND)
    , _surfaceColor(UI_COLOR_SURFACE)
    , _surfaceLight(UI_COLOR_SURFACE_LIGHT)
    , _textColor(UI_COLOR_TEXT)
    , _borderColor(UI_COLOR_BORDER)
{
}

WiFiSetupBootUI::~WiFiSetupBootUI() {
    cleanup();
}

// The delete callback holds the member's ADDRESS, so a move must re-point it at the new owner.
void WiFiSetupBootUI::rebindDeleteCallback(lv_obj_t* obj, lv_obj_t** oldSlot, lv_obj_t** newSlot) {
    if (!obj) return;
    lv_obj_remove_event_cb_with_user_data(obj, onObjectDeleted, oldSlot);
    lv_obj_add_event_cb(obj, onObjectDeleted, LV_EVENT_DELETE, newSlot);
}

WiFiSetupBootUI::WiFiSetupBootUI(WiFiSetupBootUI&& other) noexcept
    : _textArea(other._textArea)
    , _titleLabel(other._titleLabel)
    , _initialized(other._initialized)
    , _deviceName(std::move(other._deviceName))
    , _screenWidth(other._screenWidth)
    , _screenHeight(other._screenHeight)
    , _primaryColor(other._primaryColor)
    , _backgroundColor(other._backgroundColor)
    , _surfaceColor(other._surfaceColor)
    , _surfaceLight(other._surfaceLight)
    , _textColor(other._textColor)
    , _borderColor(other._borderColor)
{
    rebindDeleteCallback(_textArea, &other._textArea, &_textArea);
    rebindDeleteCallback(_titleLabel, &other._titleLabel, &_titleLabel);

    other._textArea = nullptr;
    other._titleLabel = nullptr;
    other._initialized = false;
    other._screenWidth = 0;
    other._screenHeight = 0;
}

WiFiSetupBootUI& WiFiSetupBootUI::operator=(WiFiSetupBootUI&& other) noexcept {
    if (this != &other) {
        cleanup();

        _textArea = other._textArea;
        _titleLabel = other._titleLabel;
        _initialized = other._initialized;
        _deviceName = std::move(other._deviceName);
        _screenWidth = other._screenWidth;
        _screenHeight = other._screenHeight;
        _primaryColor = other._primaryColor;
        _backgroundColor = other._backgroundColor;
        _surfaceColor = other._surfaceColor;
        _surfaceLight = other._surfaceLight;
        _textColor = other._textColor;
        _borderColor = other._borderColor;

        rebindDeleteCallback(_textArea, &other._textArea, &_textArea);
        rebindDeleteCallback(_titleLabel, &other._titleLabel, &_titleLabel);

        other._textArea = nullptr;
        other._titleLabel = nullptr;
        other._initialized = false;
        other._screenWidth = 0;
        other._screenHeight = 0;
    }
    return *this;
}

WiFiSetupBootUI::ResponsiveConfig WiFiSetupBootUI::calculateResponsiveConfig() const {
    ResponsiveConfig config;

    float aspectRatio = (float)_screenWidth / (float)_screenHeight;
    bool isWideScreen = aspectRatio > 2.0f;
    bool isTallScreen = aspectRatio < 1.0f;

    if (_screenHeight >= 400) {
        config.titleFont = &lv_font_montserrat_22;
        config.textFont = &lv_font_montserrat_14;
        config.titleTopMargin = 30;
        config.padding = 20;
    } else if (_screenHeight >= 300) {
        config.titleFont = &lv_font_montserrat_18;
        config.textFont = &lv_font_montserrat_12;
        config.titleTopMargin = 20;
        config.padding = 15;
    } else {
        config.titleFont = &lv_font_montserrat_14;
        config.textFont = &lv_font_montserrat_10;
        config.titleTopMargin = 10;
        config.padding = 10;
    }

    if (isWideScreen) {
        config.textAreaYOffset = 15;
        config.textAreaWidthPercent = 92;
        config.textAreaHeightPercent = 65;
        config.borderWidth = 2;
        config.radius = 8;
    } else if (isTallScreen) {
        config.textAreaYOffset = 50;
        config.textAreaWidthPercent = 90;
        config.textAreaHeightPercent = 55;
        config.borderWidth = 3;
        config.radius = 12;
    } else {
        config.textAreaYOffset = 40;
        config.textAreaWidthPercent = 90;
        config.textAreaHeightPercent = 60;
        config.borderWidth = 3;
        config.radius = 12;
    }

    return config;
}

void WiFiSetupBootUI::applyScreenTheme() {
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(_backgroundColor), 0);
    lv_obj_set_style_bg_grad_color(lv_scr_act(), lv_color_hex(_surfaceColor), 0);
    lv_obj_set_style_bg_grad_dir(lv_scr_act(), LV_GRAD_DIR_VER, 0);
}

void WiFiSetupBootUI::onObjectDeleted(lv_event_t* e) {
    auto** slot = static_cast<lv_obj_t**>(lv_event_get_user_data(e));
    if (slot) {
        *slot = nullptr;
    }
}

void WiFiSetupBootUI::createTitle(const char* title, const ResponsiveConfig& config) {
    _titleLabel = lv_label_create(lv_scr_act());
    if (_titleLabel) {
        lv_obj_add_event_cb(_titleLabel, onObjectDeleted, LV_EVENT_DELETE, &_titleLabel);
        lv_label_set_text(_titleLabel, title);
        lv_obj_set_style_text_color(_titleLabel, lv_color_hex(_primaryColor), 0);
        lv_obj_set_style_text_font(_titleLabel, config.titleFont, 0);
        lv_obj_align(_titleLabel, LV_ALIGN_TOP_MID, 0, config.titleTopMargin);
    }
}

void WiFiSetupBootUI::createTextArea(const ResponsiveConfig& config) {
    _textArea = lv_textarea_create(lv_scr_act());
    if (_textArea) {
        lv_obj_add_event_cb(_textArea, onObjectDeleted, LV_EVENT_DELETE, &_textArea);
        lv_textarea_set_one_line(_textArea, false);
        lv_obj_align(_textArea, LV_ALIGN_CENTER, 0, config.textAreaYOffset);
        lv_obj_set_size(_textArea, LV_PCT(config.textAreaWidthPercent), LV_PCT(config.textAreaHeightPercent));

        lv_obj_set_style_bg_color(_textArea, lv_color_hex(_surfaceColor), 0);
        lv_obj_set_style_bg_grad_color(_textArea, lv_color_hex(_surfaceLight), 0);
        lv_obj_set_style_bg_grad_dir(_textArea, LV_GRAD_DIR_VER, 0);

        lv_obj_set_style_border_width(_textArea, config.borderWidth, 0);
        lv_obj_set_style_border_color(_textArea, lv_color_hex(_borderColor), 0);
        lv_obj_set_style_border_opa(_textArea, LV_OPA_COVER, 0);

        lv_obj_set_style_radius(_textArea, config.radius, 0);
        lv_obj_set_style_text_color(_textArea, lv_color_hex(_textColor), 0);
        lv_obj_set_style_text_font(_textArea, config.textFont, 0);
        lv_obj_set_style_pad_all(_textArea, config.padding, 0);
    }
}

bool WiFiSetupBootUI::initialize(const char* title, const WiFiSetupTheme* theme,
                                 uint16_t screenWidth, uint16_t screenHeight) {
    if (_initialized) {
        return true;
    }

    if (screenWidth == 0 || screenHeight == 0) {
        _screenWidth = LV_HOR_RES;
        _screenHeight = LV_VER_RES;
    } else {
        _screenWidth = screenWidth;
        _screenHeight = screenHeight;
    }

    if (_screenWidth == 0 || _screenHeight == 0) {
        Logger.error("Boot UI: invalid screen dimensions");
        return false;
    }

    Logger.info("Boot UI: initializing for %dx%d display", _screenWidth, _screenHeight);

    if (theme) {
        _primaryColor = theme->primaryColor;
        _backgroundColor = theme->backgroundColor;
        _surfaceColor = theme->surfaceColor;
        _surfaceLight = theme->surfaceLight;
        _textColor = theme->textColor;
        _borderColor = theme->borderColor;
    }

    ResponsiveConfig config = calculateResponsiveConfig();

    applyScreenTheme();
    createTitle(title, config);
    createTextArea(config);

    if (_textArea && _titleLabel) {
        _initialized = true;
        Logger.info("Boot UI: ready");
        return true;
    } else {
        cleanup();
        return false;
    }
}

void WiFiSetupBootUI::addText(const char* text) {
    if (!_initialized || !_textArea || !text) {
        return;
    }

    lv_textarea_add_text(_textArea, text);
    lv_refr_now(lv_display_get_default());
}

void WiFiSetupBootUI::clearText() {
    if (!_initialized || !_textArea) {
        return;
    }

    lv_textarea_set_text(_textArea, "");
}

void WiFiSetupBootUI::cleanup() {
    if (_textArea) {
        lv_obj_delete(_textArea);
        _textArea = nullptr;
    }

    if (_titleLabel) {
        lv_obj_delete(_titleLabel);
        _titleLabel = nullptr;
    }

    _initialized = false;
}

void WiFiSetupBootUI::onScanStart() {
    addText("Scanning WiFi networks...\r\n");
}

void WiFiSetupBootUI::onScanComplete(int networks) {
    char text[64];
    snprintf(text, sizeof(text), "Found %d network%s\r\n",
             networks, networks != 1 ? "s" : "");
    addText(text);
}

void WiFiSetupBootUI::onConnecting(const std::string& ssid) {
    char text[128];
    snprintf(text, sizeof(text), "Connecting to '%s'\r\n", ssid.c_str());
    addText(text);
}

void WiFiSetupBootUI::onConnectionProgress() {
    addText(".");
}

void WiFiSetupBootUI::onConnected(esp_ip4_addr_t ip) {
    char device[96] = "";
    if (!_deviceName.empty()) {
        snprintf(device, sizeof(device), "Device: %s.local\r\n", _deviceName.c_str());
    }
    char text[160];
    snprintf(text, sizeof(text), "\r\nConnected!\r\n%sIP: " IPSTR "\r\n", device, IP2STR(&ip));
    addText(text);
    // No delay to show the IP: a status callback must not block; the caller decides how long this screen stays up.
}

void WiFiSetupBootUI::onAPMode(const std::string& apName, esp_ip4_addr_t ip) {
    char device[96] = "";
    if (!_deviceName.empty()) {
        snprintf(device, sizeof(device), "Device: %s\r\n", _deviceName.c_str());
    }
    char text[256];
    snprintf(text, sizeof(text),
             "AP Mode Started\r\n%sNetwork: %s\r\nIP: " IPSTR "\r\nConnect and open browser\r\n",
             device, apName.c_str(), IP2STR(&ip));
    addText(text);
}

#endif // __has_include(<lvgl.h>)
