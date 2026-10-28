#ifndef MATRIX_STATUS_H
#define MATRIX_STATUS_H

// Live SIP-speaker status on the LED matrix (MAX7219 8x8 chain / HUB75).
//
// Shows the same states as the web UI and the TFT:
//   SETUP (captive portal) - "AP 192.168.4.1"
//   OFFLINE                - "NO NETWORK"
//   REGISTERING            - "REG" + progress pixel
//   READY                  - "READY" / the SIP extension
//   INCOMING               - "RING" / "CALL <caller>"
//   CALLING                - "CALL <target>"
//   IN CALL                - "TALK" / receive level meter
// and a short "VOL n%" overlay whenever the volume changes.
//
// The task polls every 100 ms; it never blocks the SIP or audio tasks.

#include "config_manager.h"
#include "sip_client.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "matrix_display.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the renderer task. `sip_ref` may point at a NULL handle while the SIP
// client is not created yet (setup/AP mode). Returns ESP_ERR_INVALID_ARG when
esp_err_t matrix_status_start(matrix_display_handle_t matrix,
                              EventGroupHandle_t events,
                              sip_client_handle_t *sip_ref,
                              app_settings_t *settings);

#ifdef __cplusplus
}
#endif

#endif // MATRIX_STATUS_H
