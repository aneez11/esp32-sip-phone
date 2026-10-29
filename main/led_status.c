// led_status.c — drives the onboard RGB LED (WS2812 on most ESP32-S3 boards)
// as a second status indicator, mirroring the matrix/web state machine.
//
// Colours:
//   setup (AP)     : blue breathing
//   offline        : red
//   registering    : yellow (blinking)
//   ready          : green (dim, breathing)
//   incoming       : cyan (fast blink)
//   calling        : blue (blink)
//   in call        : green (solid)
//   volume change  : white flash

#include "led_status.h"
#include "app_config.h"
#include "net_manager.h"
#include "sip_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "LED_STATUS";

#ifndef RGB_LED_PIN
#define RGB_LED_PIN 48
#endif

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static EventGroupHandle_t   s_ev;
static sip_client_handle_t *s_sip_ref;
static app_settings_t      *s_set;

static sip_client_handle_t sip(void) { return (s_sip_ref && *s_sip_ref) ? *s_sip_ref : NULL; }

static void set_color(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_chan || !s_enc) return;
    uint8_t buf[3] = { g, r, b }; // WS2812 GRB order
    rmt_transmit_config_t cfg = { .loop_count = 0 };
    rmt_transmit(s_chan, s_enc, buf, sizeof(buf), &cfg);
    rmt_tx_wait_all_done(s_chan, pdMS_TO_TICKS(10));
}

static void led_status_task(void *arg) {
    (void)arg;
    uint8_t last_volume = s_set ? s_set->volume : 0;
    int64_t volume_until = 0;
    int frame = 0;

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        uint8_t vol = s_set ? s_set->volume : 0;
        if (vol != last_volume) {
            last_volume = vol;
            volume_until = esp_timer_get_time() / 1000 + 600;
        }

        uint8_t r = 0, g = 0, b = 0;

        if (esp_timer_get_time() / 1000 < volume_until) {
            r = g = b = 200;
        } else {
            int st;
            if (net_is_setup_ap()) st = 0;
            else {
                EventBits_t bits = s_ev ? xEventGroupGetBits(s_ev) : 0;
                if (!(bits & WIFI_CONNECTED_BIT)) st = 1;
                else if (sip()) {
                    switch (sip_client_get_call_state(sip())) {
                        case SIP_CALL_STATE_INCOMING:
                        case SIP_CALL_STATE_RINGING:    st = 5; break;
                        case SIP_CALL_STATE_INVITING:
                        case SIP_CALL_STATE_CONNECTING: st = 6; break;
                        case SIP_CALL_STATE_ACTIVE:     st = 7; break;
                        default: st = (bits & SIP_REGISTERED_BIT) ? 3 : 2; break;
                    }
                } else {
                    st = 2;
                }
            }
            switch (st) {
                case 0: { int v = (int)(60 + 60 * sinf(frame * 0.05f)); r = 0; g = 0; b = (uint8_t)v; } break;
                case 1: r = 180; g = 0; b = 0; break;
                case 2: if ((frame / 5) % 2) { r = 180; g = 180; b = 0; } break;
                case 3: { int v = (int)(20 + 30 * sinf(frame * 0.03f)); r = 0; g = (uint8_t)v; b = 0; } break;
                case 5: if ((frame / 2) % 2) { r = 0; g = 180; b = 180; } break;
                case 6: if ((frame / 4) % 2) { r = 0; g = 0; b = 180; } break;
                case 7: r = 0; g = 160; b = 0; break;
                default: break;
            }
        }

        set_color(r, g, b);
        frame++;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(100));
    }
}

esp_err_t led_status_start(EventGroupHandle_t events, void *sip_client_ref,
                           app_settings_t *settings) {
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
    (void)events; (void)sip_client_ref; (void)settings;
    return ESP_OK;
#else
    rmt_tx_channel_config_t cfg = {
        .gpio_num = RGB_LED_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&cfg, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt channel create failed: %s", esp_err_to_name(err));
        return err;
    }
    rmt_bytes_encoder_config_t enc = {
        .bit0 = { .level0 = 1, .duration0 = 4, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 8, .level1 = 0, .duration1 = 5 },
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&enc, &s_enc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "encoder create failed: %s", esp_err_to_name(err));
        return err;
    }
    rmt_enable(s_chan);

    s_ev = events;
    s_sip_ref = (sip_client_handle_t *)sip_client_ref;
    s_set = settings;

    if (xTaskCreate(led_status_task, "led_status", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "RGB LED status on GPIO %d", RGB_LED_PIN);
    return ESP_OK;
#endif
}
