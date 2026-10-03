// matrix_display.c — logical framebuffer + font renderer + backend dispatch.
//
// The framebuffer is RGB888 (w*h*3 bytes). All public entry points take the
// internal mutex so drawing is safe from any task. Backends implement the
// `matrix_backend_t` vtable declared in matrix_backend.h.

#include "matrix_display.h"
#include "matrix_backend.h"
#include "font5x7.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "MATRIX";

#define MX_MAX_SCROLL_TEXT 128

struct matrix_display_s {
    matrix_cfg_t cfg;
    uint8_t     *fb;            // RGB888, cfg.width * cfg.height * 3
    SemaphoreHandle_t lock;
    const matrix_backend_t *be;
    void       *be_ctx;

    // scrolling state
    char     scroll_text[MX_MAX_SCROLL_TEXT];
    uint8_t  scroll_scale;
    uint32_t scroll_rgb;
    uint16_t scroll_speed;      // px per second
    int      scroll_x;          // current x offset (can go negative)
    bool     scrolling;
};

// ---- framebuffer helpers (caller holds lock) ----

static inline void fb_pixel(struct matrix_display_s *d, int x, int y, uint32_t rgb)
{
    if (x < 0 || y < 0 || x >= d->cfg.width || y >= d->cfg.height)
        return;
    uint8_t *p = d->fb + ((size_t)y * d->cfg.width + x) * 3;
    p[0] = (rgb >> 16) & 0xFF; // R
    p[1] = (rgb >> 8) & 0xFF;  // G
    p[2] = rgb & 0xFF;         // B
}

static void fb_clear(struct matrix_display_s *d)
{
    memset(d->fb, 0, (size_t)d->cfg.width * d->cfg.height * 3);
}

// Blit one glyph (scaled) at (x,y); returns advance width in pixels.
static int draw_glyph(struct matrix_display_s *d, int x, int y, char c,
                      uint8_t scale, uint32_t rgb)
{
    const uint8_t *g = font5x7_glyph(c);
    for (int col = 0; col < FONT5X7_W; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < FONT5X7_H; row++) {
            if (bits & (1 << row)) {
                for (int sx = 0; sx < scale; sx++)
                    for (int sy = 0; sy < scale; sy++)
                        fb_pixel(d, x + col * scale + sx, y + row * scale + sy, rgb);
            }
        }
    }
    return FONT5X7_W * scale + scale; // 1px (scaled) letter spacing
}

// ---- public API ----

matrix_display_handle_t matrix_display_init(const matrix_cfg_t *cfg)
{
    if (!cfg || cfg->type == MX_NONE || cfg->width == 0 || cfg->height == 0) {
        ESP_LOGI(TAG, "No matrix configured (type=%d)", cfg ? cfg->type : -1);
        return NULL;
    }

    struct matrix_display_s *d = calloc(1, sizeof(*d));
    if (!d) {
        ESP_LOGE(TAG, "alloc failed");
        return NULL;
    }
    d->cfg = *cfg;
    if (d->cfg.brightness > 100)
        d->cfg.brightness = 100;

    size_t fb_len = (size_t)d->cfg.width * d->cfg.height * 3;
    d->fb = heap_caps_calloc(1, fb_len, MALLOC_CAP_8BIT);
    if (!d->fb) {
        ESP_LOGE(TAG, "framebuffer alloc failed (%u bytes)", (unsigned)fb_len);
        free(d);
        return NULL;
    }

    d->lock = xSemaphoreCreateMutex();
    if (!d->lock) {
        free(d->fb);
        free(d);
        return NULL;
    }

    // Bind the backend. Falls back to the preview backend so the rest of the
    // firmware (and the web preview) still work without the panel.
    d->be = matrix_backend_select(d->cfg.type);
    if (!d->be || d->be->init(&d->cfg, &d->be_ctx, d->fb, fb_len) != ESP_OK) {
        ESP_LOGW(TAG, "Backend %d init failed; using preview backend", d->cfg.type);
        d->be = matrix_backend_select(MX_PREVIEW);
        if (!d->be || d->be->init(&d->cfg, &d->be_ctx, d->fb, fb_len) != ESP_OK) {
            ESP_LOGE(TAG, "Preview backend init failed");
            vSemaphoreDelete(d->lock);
            free(d->fb);
            free(d);
            return NULL;
        }
    }

    fb_clear(d);
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->be->flush(d->be_ctx);
    xSemaphoreGive(d->lock);

    ESP_LOGI(TAG, "Matrix ready: type=%d %ux%u layout=%d color=%d",
             d->cfg.type, d->cfg.width, d->cfg.height, d->cfg.layout, d->cfg.color);
    return d;
}

void matrix_display_delete(matrix_display_handle_t h)
{
    struct matrix_display_s *d = h;
    if (!d)
        return;
    if (d->be && d->be->deinit)
        d->be->deinit(d->be_ctx);
    if (d->lock)
        vSemaphoreDelete(d->lock);
    free(d->fb);
    free(d);
}

void matrix_clear(matrix_display_handle_t h)
{
    struct matrix_display_s *d = h;
    if (!d) return;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    fb_clear(d);
    d->scrolling = false;
    d->scroll_text[0] = '\0';
    d->be->flush(d->be_ctx);
    xSemaphoreGive(d->lock);
}

void matrix_set_brightness(matrix_display_handle_t h, uint8_t percent)
{
    struct matrix_display_s *d = h;
    if (!d) return;
    if (percent > 100) percent = 100;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->cfg.brightness = percent;
    if (d->be->set_brightness)
        d->be->set_brightness(d->be_ctx, percent);
    xSemaphoreGive(d->lock);
}

int matrix_draw_text(matrix_display_handle_t h, int x, int y,
                     const char *text, uint8_t scale, uint32_t rgb)
{
    struct matrix_display_s *d = h;
    if (!d || !text) return 0;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;

    xSemaphoreTake(d->lock, portMAX_DELAY);
    int start = x;
    for (const char *s = text; *s; s++)
        x += draw_glyph(d, x, y, *s, scale, rgb);
    xSemaphoreGive(d->lock);
    return x - start;
}

void matrix_draw_text_center(matrix_display_handle_t h, int y,
                             const char *text, uint8_t scale, uint32_t rgb)
{
    struct matrix_display_s *d = h;
    if (!d || !text) return;
    if (scale < 1) scale = 1;
    if (d->cfg.layout == MX_LAYOUT_LINE)
        return; // centred text is meaningless on a 1-line ticker

    int len = (int)strlen(text);
    int w = len * (FONT5X7_W * scale + scale) - scale;
    int x = ((int)d->cfg.width - w) / 2;
    matrix_draw_text(h, x, y, text, scale, rgb);
}

int matrix_draw_number(matrix_display_handle_t h, int x, int y,
                       int value, uint8_t scale, uint32_t rgb)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    return matrix_draw_text(h, x, y, buf, scale, rgb);
}

void matrix_scroll_begin(matrix_display_handle_t h, const char *text,
                         uint8_t scale, uint32_t rgb, uint16_t px_per_s)
{
    struct matrix_display_s *d = h;
    if (!d) return;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    if (!px_per_s) px_per_s = 30;

    xSemaphoreTake(d->lock, portMAX_DELAY);
    strncpy(d->scroll_text, text ? text : "", sizeof(d->scroll_text) - 1);
    d->scroll_text[sizeof(d->scroll_text) - 1] = '\0';
    d->scroll_scale = scale;
    d->scroll_rgb = rgb;
    d->scroll_speed = px_per_s;
    d->scroll_x = d->cfg.width; // start off the right edge
    d->scrolling = true;
    xSemaphoreGive(d->lock);
}

void matrix_scroll_stop(matrix_display_handle_t h)
{
    struct matrix_display_s *d = h;
    if (!d) return;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->scrolling = false;
    xSemaphoreGive(d->lock);
}

void matrix_flush(matrix_display_handle_t h)
{
    struct matrix_display_s *d = h;
    if (!d) return;
    xSemaphoreTake(d->lock, portMAX_DELAY);
    d->be->flush(d->be_ctx);
    xSemaphoreGive(d->lock);
}

// Advance the scroll by one tick (called by the owner task at a fixed rate).
// Declared in matrix_backend.h so the matrix task can drive it.
void matrix_tick(struct matrix_display_s *d, uint32_t dt_ms)
{
    if (!d || !d->scrolling)
        return;
    xSemaphoreTake(d->lock, portMAX_DELAY);

    d->scroll_x -= (int)((d->scroll_speed * dt_ms) / 1000);

    int text_w = 0;
    for (const char *s = d->scroll_text; *s; s++)
        text_w += FONT5X7_W * d->scroll_scale + d->scroll_scale;
    if (d->scroll_x < -text_w)
        d->scroll_x = d->cfg.width; // wrap

    fb_clear(d);
    // Vertical centre of the glyph row within the panel.
    int glyph_h = FONT5X7_H * d->scroll_scale;
    int y = ((int)d->cfg.height - glyph_h) / 2;
    int x = d->scroll_x;
    for (const char *s = d->scroll_text; *s; s++)
        x += draw_glyph(d, x, y, *s, d->scroll_scale, d->scroll_rgb);

    d->be->flush(d->be_ctx);
    xSemaphoreGive(d->lock);
}

bool matrix_has_color(matrix_display_handle_t h)
{
    struct matrix_display_s *d = h;
    return d && d->cfg.color == MX_RGB;
}

void matrix_get_ascii(matrix_display_handle_t h, char *out, size_t out_len)
{
    struct matrix_display_s *d = h;
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!d) return;

    xSemaphoreTake(d->lock, portMAX_DELAY);
    size_t o = 0;
    for (int y = 0; y < d->cfg.height; y++) {
        for (int x = 0; x < d->cfg.width; x++) {
            if (o + 2 >= out_len) { // +2 for '\n' and NUL
                goto done;
            }
            const uint8_t *p = d->fb + ((size_t)y * d->cfg.width + x) * 3;
            out[o++] = (p[0] | p[1] | p[2]) ? '#' : ' ';
        }
        out[o++] = '\n';
    }
done:
    out[o] = '\0';
    xSemaphoreGive(d->lock);
}
