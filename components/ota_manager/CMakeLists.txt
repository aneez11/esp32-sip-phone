# OTA update manager (network pull + web upload) built on esp_https_ota and
# app_update. The web handlers live in main/ui_controller.c; this component only
# owns the flashing state machine and the firmware version.
idf_component_register(
    SRCS
        "ota_manager.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        config_store
        app_update
        esp_https_ota
        esp_http_client
        esp_event
        esp_app_format
        esp_http_server
        esp_timer
        freertos
        log
)
