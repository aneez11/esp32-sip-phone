// matrix_status.c — renders the SIP speaker's state machine on the LED matrix.
//
// Mirrors the sequence validated with tools/max7219_test: the panel shows what
// the device is doing (setup / offline / registering / ready / incoming /
// calling / in call) plus a volume overlay, so the matrix is a real status
// display rather than a boot-time test pattern.

#include "matrix_status.h"
#include "app_config.h"
#include "net_manager.h"
#include "sip_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/ip_addr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "MX_STATUS";

#define RGB_STATUS  0x00FF00    // mono panel: any lit pixel lights the LEDs
#define TICK_MS     100
#define VOL_HOLD_MS 2500
#define TEXT_MAX    40

typedef enum {
    MS_SETUP = 0,
    MS_OFFLINE,
    MS_REGISTERING,
    MS_READY,
    MS_INCOMING,
    MS_CALLING,
    MS_ACTIVE,
} mx_state_t;

static matrix_display_handle_t s_h;
static EventGroupHandle_t      s_ev;
static sip_client_handle_t    *s_sip_ref;
static app_settings_t         *s_set;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// The SIP client is created after this task starts, so always dereference.
static sip_client_handle_t sip(void) { return s_sip_ref ? *s_sip_ref : NULL; }

// "sip:1001@host" -> "1001" (first 22 chars).
static void short_uri(const char *in, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    const char *p = in ? in : "";
    const char *sip = strstr(p, "sip:");
    if (sip) p = sip + 4;
    size_t i = 0;
    while (p[i] && p[i] != '@' && p[i] != '>' && i + 1 < out_len) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

static mx_state_t panel_state(void) {
    if (net_is_setup_ap()) return MS_SETUP;

    EventBits_t bits = s_ev ? xEventGroupGetBits(s_ev) : 0;
    // Only the Wi-Fi bit matters here: the AP fallback also sets it, and that
    // case is handled above.
    if (!(bits & WIFI_CONNECTED_BIT)) return MS_OFFLINE;

    if (sip()) {
        switch (sip_client_get_call_state(sip())) {
            case SIP_CALL_STATE_INCOMING:
            case SIP_CALL_STATE_RINGING:    return MS_INCOMING;
            case SIP_CALL_STATE_INVITING:
            case SIP_CALL_STATE_CONNECTING: return MS_CALLING;
            case SIP_CALL_STATE_ACTIVE:     return MS_ACTIVE;
            default: break;
        }
    }
    return (bits & SIP_REGISTERED_BIT) ? MS_READY : MS_REGISTERING;
}

// ---- drawing helpers -------------------------------------------------------
static void draw_static(const char *text) {
    matrix_clear(s_h);
    matrix_draw_text_center(s_h, 0, text, 1, RGB_STATUS);
    matrix_flush(s_h);
}

// Scrolls one line right-to-left. `x` and `shown` keep the animation state.
static void draw_marquee(const char *text, int *x, char *shown, size_t shown_len) {
    if (!text) text = "";
    if (strcmp(text, shown) != 0) {
        snprintf(shown, shown_len, "%s", text);
        *x = matrix_width(s_h);
    }
    matrix_clear(s_h);
    matrix_draw_text(s_h, *x, 0, shown, 1, RGB_STATUS);
    matrix_flush(s_h);
    (*x)--;
    if (*x < -matrix_text_width(shown, 1)) *x = matrix_width(s_h);
}

// Receive-level style bars, animated (the pipeline exposes no level to poll).
static void draw_meter(int frame) {
    int w = matrix_width(s_h);
    matrix_clear(s_h);
    for (int x = 0; x < w; x++) {
        float phase = (frame * 0.35f) + (x * 0.42f);
        int h = 1 + (int)(3.5f * (1.0f + sinf(phase)));
        if (h > 8) h = 8;
        for (int i = 0; i < h; i++) matrix_set_pixel(s_h, x, 7 - i, RGB_STATUS);
    }
    matrix_flush(s_h);
}

static void draw_spinner(int frame, const char *label) {
    int w = matrix_width(s_h);
    matrix_clear(s_h);
    matrix_draw_text_center(s_h, 0, label, 1, RGB_STATUS);
    matrix_set_pixel(s_h, (frame * 2) % (w ? w : 1), 7, RGB_STATUS);
    matrix_flush(s_h);
}

// ---- task ------------------------------------------------------------------
static void matrix_status_task(void *arg) {
    (void)arg;
    mx_state_t state = (mx_state_t)-1;
    int frame = 0;
    int mq_x = 0;
    char shown[TEXT_MAX] = {0};
    char msg[TEXT_MAX] = {0};
    uint8_t last_volume = s_set ? s_set->volume : 0;
    int64_t volume_until = 0;

    ESP_LOGI(TAG, "Status task started on a %ux%u panel",
             matrix_width(s_h), matrix_height(s_h));

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        mx_state_t st = panel_state();
        if (st != state) {
            ESP_LOGI(TAG, "Matrix state -> %d", (int)st);
            state = st;
            frame = 0;
            shown[0] = '\0';   // force marquee restart
            mq_x = matrix_width(s_h);
        }

        // Volume overlay (web UI / buttons change s_set->volume directly).
        uint8_t volume = s_set ? s_set->volume : 0;
        if (volume != last_volume) {
            last_volume = volume;
            volume_until = now_ms() + VOL_HOLD_MS;
            shown[0] = '\0';
            mq_x = matrix_width(s_h);
        }
        if (now_ms() < volume_until) {
            snprintf(msg, sizeof(msg), "VOL %u%%", (unsigned)volume);
            draw_marquee(msg, &mq_x, shown, sizeof(shown));
            frame++;
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_MS));
            continue;
        }

        switch (state) {
            case MS_SETUP: {
                esp_ip4_addr_t ip = {0};
                if (net_get_ip(&ip) == ESP_OK) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip));
                    snprintf(msg, sizeof(msg), "AP SETUP %s", ip_str);
                } else {
                    snprintf(msg, sizeof(msg), "AP SETUP");
                }
                draw_marquee(msg, &mq_x, shown, sizeof(shown));
                break;
            }
            case MS_OFFLINE:
                draw_marquee("NO NETWORK", &mq_x, shown, sizeof(shown));
                break;
            case MS_REGISTERING:
                draw_spinner(frame, "REG");
                break;
            case MS_READY: {
                // Alternate "READY" and the extension every ~2.5 s.
                const char *label = ((frame / 25) % 2 == 0) ? "READY"
                                                             : (s_set->sip_user[0] ? s_set->sip_user : "READY");
                draw_static(label);
                break;
            }
            case MS_INCOMING: {
                char uri[96] = {0};
                char who[24] = {0};
                if (sip()) sip_client_get_remote_uri(sip(), uri, sizeof(uri));
                short_uri(uri, who, sizeof(who));
                if (who[0]) {
                    snprintf(msg, sizeof(msg), "CALL %s", who);
                } else {
                    snprintf(msg, sizeof(msg), "INCOMING");
                }
                // Blink "RING", then scroll the caller between blinks.
                if ((frame / 5) % 2 == 0) {
                    draw_static("RING");
                } else {
                    draw_marquee(msg, &mq_x, shown, sizeof(shown));
                }
                break;
            }
            case MS_CALLING: {
                char who[24] = {0};
                short_uri(s_set->sip_target, who, sizeof(who));
                snprintf(msg, sizeof(msg), who[0] ? "CALL %s" : "CALLING", who);
                draw_marquee(msg, &mq_x, shown, sizeof(shown));
                break;
            }
            case MS_ACTIVE:
                // "TALK" then a level meter, repeating.
                if ((frame / 10) % 3 == 0) {
                    draw_static("TALK");
                } else {
                    draw_meter(frame);
                }
                break;
            default:
                draw_static("SIP");
                break;
        }

        frame++;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t matrix_status_start(matrix_display_handle_t matrix,
                              EventGroupHandle_t events,
                              sip_client_handle_t *sip_ref,
                              app_settings_t *settings) {
    if (!matrix || !events || !settings) return ESP_ERR_INVALID_ARG;

    s_h = matrix;
    s_ev = events;
    s_sip_ref = sip_ref;
    s_set = settings;

    if (xTaskCreate(matrix_status_task, "mx_status", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
