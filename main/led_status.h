#ifndef LED_STATUS_H
#define LED_STATUS_H

#include "app_config.h"
#include "config_manager.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

// Drives the onboard RGB LED (WS2812 on most ESP32-S3 boards) as a second
// status indicator mirroring the web/matrix state machine. No-op on targets
// without an onboard RGB LED.
esp_err_t led_status_start(EventGroupHandle_t events,
                           void *sip_client_ref,
                           app_settings_t *settings);

#ifdef __cplusplus
}
#endif

#endif // LED_STATUS_H
