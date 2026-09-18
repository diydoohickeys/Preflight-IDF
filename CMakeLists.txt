idf_component_register(
    SRCS
        "src/WiFiSetupManager.cpp"
        "src/WiFiSetupBootUI.cpp"
        "src/OTAManager.cpp"
        "src/Logger.cpp"
    INCLUDE_DIRS "src"
    REQUIRES
        mdns
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        nvs_flash
        esp_partition   # OTAManager raw FS-partition writes; Logger coredump mmap
        esp_app_format
        app_update
        esp_timer
        espcoredump     # Logger::reportLastCrash
)

# LVGL only when the project has it (by either name the component manager gives it); without
# it, WiFiSetupBootUI.cpp's __has_include(<lvgl.h>) guard compiles the boot UI out.
idf_component_optional_requires(PUBLIC lvgl lvgl__lvgl)

target_compile_definitions(${COMPONENT_LIB} PRIVATE PROGMEM=)

# Logger's PSRAM allocator throws std::bad_alloc; fail here rather than with -fno-exceptions compile errors.
if(NOT CONFIG_COMPILER_CXX_EXCEPTIONS)
    message(FATAL_ERROR
        "Preflight requires C++ exceptions. Add "
        "CONFIG_COMPILER_CXX_EXCEPTIONS=y to your sdkconfig.defaults "
        "(and keep CONFIG_COMPILER_CXX_EXCEPTIONS_EMG_POOL_SIZE non-zero, or a "
        "throw under memory pressure calls std::terminate instead of unwinding).")
endif()
