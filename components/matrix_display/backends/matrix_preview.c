// matrix_preview.c — no-hardware backend. Logs a compact ASCII rendering of the
// framebuffer so the matrix logic (caller ID, queue, alerts) can be exercised in
// the Wokwi simulator, the PC, and unit tests without a panel.

#include "matrix_backend.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "MX_PREVIEW";

typedef struct {
    const matrix_cfg_t *cfg;
    uint8_t *fb;
    size_t   fb_len;
    uint8_t  brightness;
} preview_ctx_t;

static esp_err_t preview_init(const matrix_cfg_t *cfg, void **ctx, uint8_t *fb, size_t fb_len)
{
    preview_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) return ESP_ERR_NO_MEM;
    c->cfg = cfg;
    c->fb = fb;
    c->fb_len = fb_len;
    c->brightness = cfg->brightness;
    *ctx = c;
    ESP_LOGI(TAG, "Preview backend %ux%u (no hardware)", cfg->width, cfg->height);
    return ESP_OK;
}

static void preview_deinit(void *ctx) { free(ctx); }

static void preview_flush(void *ctx)
{
    preview_ctx_t *c = (preview_ctx_t *)ctx;
    if (!c) return;
    // One log line per row, ASCII. Kept short so it is readable in a monitor.
    char row[129];
    int w = c->cfg->width;
    if (w > 128) w = 128;
    for (int y = 0; y < c->cfg->height; y++) {
        const uint8_t *p = c->fb + (size_t)y * c->cfg->width * 3;
        for (int x = 0; x < w; x++) {
            uint8_t v = p[x * 3] | p[x * 3 + 1] | p[x * 3 + 2];
            row[x] = v ? '#' : '.';
        }
        row[w] = '\0';
        ESP_LOGI(TAG, "|%s|", row);
    }
}

static void preview_set_brightness(void *ctx, uint8_t percent)
{
    preview_ctx_t *c = (preview_ctx_t *)ctx;
    if (c) c->brightness = percent;
}

static const matrix_backend_t PREVIEW = {
    .name = "preview",
    .init = preview_init,
    .deinit = preview_deinit,
    .flush = preview_flush,
    .set_brightness = preview_set_brightness,
};

const matrix_backend_t *matrix_preview_backend(void) { return &PREVIEW; }
