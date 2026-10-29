// matrix_status.c — renders the SIP speaker's state machine on the LED matrix.
//
// Honours the Display-tab settings:
//   * screensaver: enabled / idle timeout / mode / effect mask / speed /
//     custom texts / dim brightness
//   * multi-line layout: per-slot data source, scale, row span, alignment,
//     scroll (used when any slot has a non-Blank source)
//   * pixel aspect: glyph columns are stretched for non-square pixels
//
// All drawing stays in this one task (single writer, no extra mutex).

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
#include <time.h>

static const char *TAG = "MX_STATUS";

#define RGB_STATUS  0x00FF00    // mono panel: any lit pixel lights the LEDs
#define TICK_MS     100
#define VOL_HOLD_MS 2500
#define TEXT_MAX    48
#define SS_HOLD_MS  6000        // how long one saver effect stays on screen

typedef enum {
    MS_SETUP = 0,
    MS_OFFLINE,
    MS_REGISTERING,
    MS_READY,
    MS_INCOMING,
    MS_CALLING,
    MS_ACTIVE,
} mx_state_t;

// Screensaver effects, indexed by mask bit (see the plan, section 5c.2).
enum {
    SS_BOUNCE = 0, SS_RAIN, SS_SNAKE, SS_SPARKLE, SS_SCROLL, SS_METER, SS_CLOCK,
    SS_EFFECT_COUNT
};

static matrix_display_handle_t s_h;
static EventGroupHandle_t      s_ev;
static sip_client_handle_t    *s_sip_ref;
static app_settings_t         *s_set;
static int64_t                 s_test_until;   // suspend status drawing during a test pattern

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

// ---- geometry / text helpers ----------------------------------------------
static uint8_t aspect_mul(void) {
    // 0 = 1:1, 1 = 2:1 wide pixels (stretch glyphs x2), 2 = 1:2 tall (no
    // horizontal stretch; the font already suits tall panels better).
    return (s_set && s_set->matrix_pixel_aspect == 1) ? 2 : 1;
}

static int text_w(const char *t, uint8_t sc) {
    return matrix_text_width_stretch(t, sc, aspect_mul());
}

static int draw_text_at(int x, int y, const char *t, uint8_t sc) {
    return matrix_draw_text_stretch(s_h, x, y, t, sc, RGB_STATUS, aspect_mul());
}

// Pick a text scale that fits the panel height (min 1, max 4).
static uint8_t auto_scale(void) {
    int h = matrix_height(s_h);
    if (h >= 28) return 3;
    if (h >= 16) return 2;
    return 1;
}

static void draw_static(const char *text) {
    matrix_clear(s_h);
    uint8_t sc = auto_scale();
    int gh = 7 * sc;
    int y = (matrix_height(s_h) - gh) / 2;
    if (y < 0) y = 0;
    int x = (matrix_width(s_h) - text_w(text, sc)) / 2;
    if (x < 0) x = 0;
    draw_text_at(x, y, text, sc);
    matrix_flush(s_h);
}

// Scrolls one line right-to-left. `x`, `shown` and `step` keep animation state.
// Returns true when the text has just scrolled fully off, which lets the caller
// advance to the next custom string only after the current one was shown.
static bool draw_marquee(const char *text, int *x, char *shown, size_t shown_len, int step) {
    if (!text) text = "";
    if (step < 1) step = 1;
    if (strcmp(text, shown) != 0) {
        snprintf(shown, shown_len, "%s", text);
        *x = matrix_width(s_h);
    }
    matrix_clear(s_h);
    int ty = (matrix_height(s_h) - 7) / 2; // centre vertically on tall panels
    if (ty < 0) ty = 0;
    draw_text_at(*x, ty, shown, 1);
    matrix_flush(s_h);
    *x -= step;
    if (*x < -text_w(shown, 1)) {
        *x = matrix_width(s_h);
        return true;
    }
    return false;
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

// ---- data sources (multi-line slots) --------------------------------------
static void device_uptime(char *out, size_t n) {
    int64_t s = esp_timer_get_time() / 1000000;
    snprintf(out, n, "%dd %02d:%02d", (int)(s / 86400), (int)((s / 3600) % 24),
             (int)((s / 60) % 60));
}

static void clock_text(char *out, size_t n, bool want_date) {
    out[0] = '\0';
    int64_t epoch = (int64_t)time(NULL);
    if (epoch < 1600000000) return; // SNTP not synced yet
    // Manual offset so the clock can be corrected without recompiling TZ.
    epoch += (int64_t)(s_set ? s_set->clock_offset_min : 0) * 60;
    time_t t = (time_t)epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    if (want_date) {
        strftime(out, n, "%d/%m", &tm);
    } else if (s_set && s_set->clock_12h) {
        strftime(out, n, "%I:%M", &tm);
    } else {
        strftime(out, n, "%H:%M", &tm);
    }
}

static const char *state_label(mx_state_t s) {
    switch (s) {
        case MS_SETUP:       return "SETUP";
        case MS_OFFLINE:     return "OFFLINE";
        case MS_REGISTERING: return "REG";
        case MS_READY:       return "READY";
        case MS_INCOMING:    return "RING";
        case MS_CALLING:     return "CALL";
        case MS_ACTIVE:      return "TALK";
        default:             return "SIP";
    }
}

// Fill `out` with the text for one line slot (0 = Blank).
static void line_source_text(uint8_t src, mx_state_t state, char *out, size_t n) {
    out[0] = '\0';
    switch (src) {
        case 0: /* Blank */ break;
        case 1: { // Caller
            char uri[96] = {0};
            if (sip()) sip_client_get_remote_uri(sip(), uri, sizeof(uri));
            if (!uri[0]) snprintf(uri, sizeof(uri), "%s", s_set->sip_target);
            short_uri(uri, out, n);
            break;
        }
        case 2: snprintf(out, n, "Q 0"); break;             // no queue service yet
        case 3: break;                                      // Alert text: none yet
        case 4: snprintf(out, n, "%s", s_set->sip_user); break;
        case 5: snprintf(out, n, "%s", s_set->sip_server); break;
        case 6: {
            esp_ip4_addr_t ip = {0};
            if (net_get_ip(&ip) == ESP_OK) snprintf(out, n, IPSTR, IP2STR(&ip));
            break;
        }
        case 7: device_uptime(out, n); break;
        case 8: snprintf(out, n, "VOL %u%%", (unsigned)s_set->volume); break;
        case 9: case 10: case 11: case 12:
            snprintf(out, n, "%s", s_set->ss_text[src - 9]);
            break;
        case 13: clock_text(out, n, false); break;
        case 14: clock_text(out, n, true); break;
        case 15: snprintf(out, n, "%s", state_label(state)); break;
        default: break;
    }
}

// Draw the configured multi-line layout. Returns false when no slot has a
// source, in which case the caller uses the built-in per-state rendering.
static bool render_multiline(mx_state_t state) {
    int n = s_set->line_count;
    if (n < 1) n = 1;
    if (n > 4) n = 4;

    bool any = false;
    int bands = 0;
    for (int i = 0; i < n; i++) {
        if (s_set->line_src[i] != 0) any = true;
        int sp = s_set->line_span[i];
        bands += (sp >= 1 && sp <= 4) ? sp : 1;
    }
    if (!any || bands == 0) return false;

    int h = matrix_height(s_h);
    int w = matrix_width(s_h);
    int band_h = h / bands;
    if (band_h < 8) band_h = 8;

    matrix_clear(s_h);
    int y = 0;
    for (int i = 0; i < n; i++) {
        int sp = s_set->line_span[i];
        if (sp < 1 || sp > 4) sp = 1;
        int slot_h = band_h * sp;

        uint8_t sc = s_set->line_scale[i];
        if (sc == 0) {
            sc = (slot_h >= 28) ? 3 : (slot_h >= 16) ? 2 : 1;
        }
        if (sc > 4) sc = 4;
        int gh = 7 * sc;
        if (y + gh > h) break;

        char text[TEXT_MAX];
        line_source_text(s_set->line_src[i], state, text, sizeof(text));
        if (text[0]) {
            int tw = text_w(text, sc);
            int x = 0;
            uint8_t align = s_set->line_align[i];
            if (align == 1) x = (w - tw) / 2;
            else if (align == 2) x = w - tw;
            if (x < 0) x = 0;
            int ty = y + (slot_h - gh) / 2;
            if (ty + gh > h) ty = h - gh;
            if (ty < 0) ty = 0;
            if (tw > w && s_set->line_scroll[i] && align == 0) {
                // Scroll this line inside its band; the marquee helper draws at
                // y=0, so drive the text manually here.
                int off = (int)((esp_timer_get_time() / 1000 / 30) % (tw + w));
                draw_text_at(w - off, ty, text, sc);
            } else {
                draw_text_at(x, ty, text, sc);
            }
        }
        y += slot_h;
    }
    matrix_flush(s_h);
    return true;
}

// ---- screen savers ---------------------------------------------------------
static void ss_bounce(int *bx, int *by, int *vx, int *vy, int w, int h) {
    *bx += *vx; *by += *vy;
    if (*bx < 0) { *bx = 0; *vx = 1; }
    if (*bx >= w) { *bx = w - 1; *vx = -1; }
    if (*by < 0) { *by = 0; *vy = 1; }
    if (*by >= h) { *by = h - 1; *vy = -1; }
    matrix_clear(s_h);
    matrix_set_pixel(s_h, *bx, *by, RGB_STATUS);
    matrix_flush(s_h);
}

static void ss_rain(int frame, int w, int h) {
    matrix_clear(s_h);
    for (int x = 0; x < w; x++) {
        int y = (frame + x * 3) % (h + 2);
        if (y < h) matrix_set_pixel(s_h, x, y, RGB_STATUS);
        if (y - 1 >= 0 && y - 1 < h) matrix_set_pixel(s_h, x, y - 1, RGB_STATUS);
    }
    matrix_flush(s_h);
}

static void ss_snake(int *sx, int dir, int w, int h) {
    matrix_clear(s_h);
    for (int i = 0; i < 4; i++) {
        int x = *sx - i;
        if (x >= 0 && x < w) matrix_set_pixel(s_h, x, (i % 2 == 0) ? (h - 1) : 0, RGB_STATUS);
    }
    *sx += dir;
    if (*sx >= w + 4) *sx = -4;
    matrix_flush(s_h);
}

static void ss_sparkle(int frame, int w, int h) {
    matrix_clear(s_h);
    uint32_t r = (uint32_t)frame * 2654435761u + 0x9e3779b9u;
    for (int i = 0; i < 6; i++) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        int x = (int)(r % (unsigned)w);
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        int y = (int)(r % (unsigned)h);
        matrix_set_pixel(s_h, x, y, RGB_STATUS);
    }
    matrix_flush(s_h);
}

// Clock saver: time on the first row, date below on tall panels.
static void ss_clock(void) {
    char tbuf[8], dbuf[8];
    clock_text(tbuf, sizeof(tbuf), false);
    clock_text(dbuf, sizeof(dbuf), true);
    matrix_clear(s_h);
    if (!tbuf[0]) {
        draw_text_at((matrix_width(s_h) - text_w("--:--", 1)) / 2, 0, "--:--", 1);
    } else if (matrix_height(s_h) >= 16 && (!s_set || s_set->clock_date)) {
        uint8_t sc = auto_scale();
        int tw = text_w(tbuf, sc);
        draw_text_at((matrix_width(s_h) - tw) / 2, 0, tbuf, sc);
        int y2 = 7 * sc + 2;
        if (y2 + 7 <= matrix_height(s_h)) {
            draw_text_at((matrix_width(s_h) - text_w(dbuf, 1)) / 2, y2, dbuf, 1);
        }
    } else {
        draw_text_at((matrix_width(s_h) - text_w(tbuf, 1)) / 2, 0, tbuf, 1);
    }
    matrix_flush(s_h);
}

// Pick the next non-empty custom text (falls back to display name / SIP PHONE).
static const char *next_ss_text(int *idx) {
    for (int k = 0; k < 4; k++) {
        int i = (*idx + k) & 3;
        if (s_set->ss_text[i][0]) {
            *idx = (i + 1) & 3;
            return s_set->ss_text[i];
        }
    }
    return s_set->sip_display_name[0] ? s_set->sip_display_name : "SIP PHONE";
}

// ---- task ------------------------------------------------------------------
static void matrix_status_task(void *arg) {
    (void)arg;
    mx_state_t state = (mx_state_t)-1;
    int frame = 0;
    int mq_x = 0;
    int mq_step = 1;
    char shown[TEXT_MAX] = {0};
    char msg[TEXT_MAX] = {0};
    uint8_t last_volume = s_set ? s_set->volume : 0;
    int64_t volume_until = 0;

    // Screensaver runtime state.
    int64_t idle_since = now_ms();
    bool saver = false;
    int ss_effect = 0;
    int64_t ss_switched = 0;
    int ss_text_idx = 0;
    const char *ss_cur_text = NULL;  // string currently being scrolled
    bool ss_text_done = false;       // current string finished a full pass
    int ss_bx = 0, ss_by = 0, ss_vx = 1, ss_vy = 1;
    int ss_snake_x = -4, ss_snake_dir = 1;

    ESP_LOGI(TAG, "Status task started on a %ux%u panel (aspect x%u)",
             matrix_width(s_h), matrix_height(s_h), (unsigned)aspect_mul());

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        // A test pattern from the web UI owns the panel for a few seconds.
        if (s_test_until) {
            if (now_ms() < s_test_until) {
                frame++;
                vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_MS));
                continue;
            }
            s_test_until = 0;
            idle_since = now_ms();
        }

        mx_state_t st = panel_state();
        if (st != state) {
            ESP_LOGI(TAG, "Matrix state -> %d", (int)st);
            state = st;
            frame = 0;
            shown[0] = '\0';
            mq_x = matrix_width(s_h);
            idle_since = now_ms();
            if (saver) {
                saver = false;
                matrix_set_brightness(s_h, s_set->matrix_brightness);
            }
        }

        // Volume overlay takes priority over everything (and exits the saver).
        uint8_t volume = s_set ? s_set->volume : 0;
        if (volume != last_volume) {
            last_volume = volume;
            volume_until = now_ms() + VOL_HOLD_MS;
            shown[0] = '\0';
            mq_x = matrix_width(s_h);
            idle_since = now_ms();
            if (saver) {
                saver = false;
                matrix_set_brightness(s_h, s_set->matrix_brightness);
            }
        }
        if (now_ms() < volume_until) {
            snprintf(msg, sizeof(msg), "VOL %u%%", (unsigned)volume);
            draw_marquee(msg, &mq_x, shown, sizeof(shown), 1);
            frame++;
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_MS));
            continue;
        }

        // Display mode Off blanks the panel entirely (the status task keeps
        // running so turning it back on is instant).
        if (s_set->display_mode == DISPLAY_MODE_OFF) {
            matrix_clear(s_h);
            matrix_flush(s_h);
            frame++;
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_MS));
            continue;
        }

        // ---- screensaver: idle detection (READY only) ----
        bool want_saver = (state == MS_READY) && s_set->ss_enabled &&
                          s_set->ss_idle_s > 0 &&
                          (now_ms() - idle_since) >= (int64_t)s_set->ss_idle_s * 1000;

        if (want_saver && !saver) {
            // Build the enabled effect list from the mask.
            int enabled[SS_EFFECT_COUNT];
            int n = 0;
            for (int b = 0; b < SS_EFFECT_COUNT; b++) {
                if (s_set->ss_mask & (1 << b)) enabled[n++] = b;
            }
            if (n == 0) {
                enabled[0] = SS_SCROLL; // never leave the panel blank
                n = 1;
            }
            // ss_mode: 0=off 1=single 2=rotate 3=random
            if (s_set->ss_mode == 0) {
                want_saver = false;
            } else {
                saver = true;
                int pick = 0;
                if (s_set->ss_mode == 3) {
                    pick = (int)(esp_timer_get_time() % (int64_t)n);
                }
                ss_effect = enabled[pick];
                ss_switched = now_ms();
                ss_text_idx = 0;
                ss_cur_text = NULL;
                ss_text_done = false;
                ss_bx = matrix_width(s_h) / 2;
                ss_by = matrix_height(s_h) / 2;
                ss_vx = 1; ss_vy = 1;
                ss_snake_x = -4; ss_snake_dir = 1;
                shown[0] = '\0';
                mq_x = matrix_width(s_h);
                if (s_set->ss_bright > 0) matrix_set_brightness(s_h, s_set->ss_bright);
                ESP_LOGI(TAG, "Screensaver on (effect %d, n=%d)", ss_effect, n);
            }
        } else if (!want_saver && saver) {
            saver = false;
            matrix_set_brightness(s_h, s_set->matrix_brightness);
            ESP_LOGI(TAG, "Screensaver off");
        }

        if (saver) {
            // Rotate/random: switch effect every SS_HOLD_MS, except for the
            // scrolling text which switches only after a complete pass (a long
            // string would otherwise be cut off mid-scroll).
            bool time_to_switch = (s_set->ss_mode == 2 || s_set->ss_mode == 3) &&
                                  (now_ms() - ss_switched >= SS_HOLD_MS);
            if (ss_effect == SS_SCROLL &&
                (s_set->ss_mode == 2 || s_set->ss_mode == 3)) {
                time_to_switch = ss_text_done;
            }
            if (time_to_switch) {
                int enabled[SS_EFFECT_COUNT];
                int n = 0;
                for (int b = 0; b < SS_EFFECT_COUNT; b++) {
                    if (s_set->ss_mask & (1 << b)) enabled[n++] = b;
                }
                if (n == 0) { enabled[0] = SS_SCROLL; n = 1; }
                int cur = 0;
                for (int i = 0; i < n; i++) if (enabled[i] == ss_effect) cur = i;
                int next = (s_set->ss_mode == 3)
                               ? (int)(esp_timer_get_time() % (int64_t)n)
                               : (cur + 1) % n;
                ss_effect = enabled[next];
                ss_switched = now_ms();
                ss_text_idx = 0;
                ss_cur_text = NULL;
                ss_text_done = false;
                ss_bx = matrix_width(s_h) / 2; ss_by = matrix_height(s_h) / 2;
                ss_snake_x = -4;
                shown[0] = '\0';
                mq_x = matrix_width(s_h);
            }

            int w = matrix_width(s_h);
            int h = matrix_height(s_h);
            switch (ss_effect) {
                case SS_BOUNCE: ss_bounce(&ss_bx, &ss_by, &ss_vx, &ss_vy, w, h); break;
                case SS_RAIN: ss_rain(frame, w, h); break;
                case SS_SNAKE: ss_snake(&ss_snake_x, ss_snake_dir, w, h); break;
                case SS_SPARKLE: ss_sparkle(frame, w, h); break;
                case SS_METER: draw_meter(frame); break;
                case SS_CLOCK: ss_clock(); break;
                case SS_SCROLL:
                default: {
                    // Pick the next string only after the previous one finished
                    // a full pass, otherwise the marquee restarts every tick.
                    if (!ss_cur_text) {
                        ss_cur_text = next_ss_text(&ss_text_idx);
                        ss_text_done = false;
                    }
                    int step = (s_set->ss_speed == 0) ? 1 : (s_set->ss_speed == 2) ? 3 : 2;
                    if (draw_marquee(ss_cur_text, &mq_x, shown, sizeof(shown), step)) {
                        ss_cur_text = NULL;   // next pass shows the next string
                        ss_text_done = true;  // rotate may switch now
                    }
                    break;
                }
            }
            if (s_set->ss_speed == 0 && ss_effect != SS_SCROLL && (frame & 1)) {
                // slow: skip half the animation frames (scroll has its own step)
                vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_MS));
                frame++;
                continue;
            }
            frame++;
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TICK_MS));
            continue;
        }

        // ---- normal status rendering ----
        mq_step = (s_set->ss_speed == 2) ? 2 : 1;

        // Multi-line layout wins when the user configured any slot.
        if (matrix_height(s_h) >= 16 && render_multiline(state)) {
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
                draw_marquee(msg, &mq_x, shown, sizeof(shown), 1);
                break;
            }
            case MS_OFFLINE:
                draw_marquee("NO NETWORK", &mq_x, shown, sizeof(shown), 1);
                break;
            case MS_REGISTERING:
                draw_spinner(frame, "REG");
                break;
            case MS_READY: {
                if (s_set->clock_mode) { // idle clock replaces READY/extension
                    ss_clock();
                    break;
                }
                if (s_set->display_mode == DISPLAY_MODE_QUEUE) {
                    draw_static("Q 0"); // no queue service yet
                    break;
                }
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
                if (who[0]) snprintf(msg, sizeof(msg), "CALL %s", who);
                else snprintf(msg, sizeof(msg), "INCOMING");
                if (matrix_height(s_h) >= 16) {
                    matrix_clear(s_h);
                    uint8_t sc = auto_scale();
                    int tw = text_w("RING", sc);
                    draw_text_at((matrix_width(s_h) - tw) / 2, 0, "RING", sc);
                    uint8_t sc2 = (sc > 1) ? (uint8_t)(sc - 1) : 1;
                    int y2 = 7 * sc + 2;
                    if (y2 + 7 * sc2 <= matrix_height(s_h)) {
                        draw_text_at((matrix_width(s_h) - text_w(who, sc2)) / 2, y2, who, sc2);
                    }
                    matrix_flush(s_h);
                } else if ((frame / 5) % 2 == 0) {
                    draw_static("RING");
                } else {
                    draw_marquee(msg, &mq_x, shown, sizeof(shown), mq_step);
                }
                break;
            }
            case MS_CALLING: {
                char who[24] = {0};
                short_uri(s_set->sip_target, who, sizeof(who));
                if (who[0]) snprintf(msg, sizeof(msg), "CALL %s", who);
                else snprintf(msg, sizeof(msg), "CALLING");
                draw_marquee(msg, &mq_x, shown, sizeof(shown), mq_step);
                break;
            }
            case MS_ACTIVE:
                if (matrix_height(s_h) >= 16) {
                    matrix_clear(s_h);
                    uint8_t sc = auto_scale();
                    draw_text_at((matrix_width(s_h) - text_w("TALK", sc)) / 2, 0, "TALK", sc);
                    int w = matrix_width(s_h);
                    int base = 7 * sc + 2;
                    int avail = matrix_height(s_h) - base;
                    if (avail > 0) {
                        for (int x = 0; x < w; x++) {
                            float phase = (frame * 0.35f) + (x * 0.42f);
                            int hh = 1 + (int)(3.5f * (1.0f + sinf(phase)));
                            if (hh > avail) hh = avail;
                            for (int i = 0; i < hh; i++)
                                matrix_set_pixel(s_h, x, matrix_height(s_h) - 1 - i, RGB_STATUS);
                        }
                    }
                    matrix_flush(s_h);
                } else if ((frame / 10) % 3 == 0) {
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

// Draw a wiring test pattern (border + checkerboard + "TEST") and keep the
// status renderer from overwriting it for a few seconds.
void matrix_status_show_test_pattern(int seconds) {
    if (!s_h) return;
    if (seconds < 1) seconds = 5;
    s_test_until = now_ms() + (int64_t)seconds * 1000;

    int w = matrix_width(s_h);
    int h = matrix_height(s_h);
    matrix_clear(s_h);
    for (int x = 0; x < w; x++) {
        matrix_set_pixel(s_h, x, 0, RGB_STATUS);
        matrix_set_pixel(s_h, x, h - 1, RGB_STATUS);
    }
    for (int y = 0; y < h; y++) {
        matrix_set_pixel(s_h, 0, y, RGB_STATUS);
        matrix_set_pixel(s_h, w - 1, y, RGB_STATUS);
    }
    for (int y = 2; y < h - 2; y += 2)
        for (int x = 2; x < w - 2; x += 2)
            matrix_set_pixel(s_h, x, y, RGB_STATUS);
    matrix_draw_text_center(s_h, 0, "TEST", 1, RGB_STATUS);
    matrix_flush(s_h);
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

    if (xTaskCreate(matrix_status_task, "mx_status", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
