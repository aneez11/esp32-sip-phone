// matrix_max7219.c — chain of 8x8 MAX7219 modules driven over GPIO (bit-bang).
//
// Used as a single-line ticker (layout MX_LAYOUT_LINE): width = 8 * modules,
// height = 8. Only the pixel's luma is used; the displayed colour is the LEDs'
// own colour.
//
// Bit-banged (no SPI host) so it never contends with the TFT/LVGL SPI bus and
// works on any free GPIO. ~1 MHz is plenty for a few modules.

#include "matrix_backend.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us
#include "driver/gpio.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MX_MAX7219";

// MAX7219 registers
#define MAX7219_DIGIT0      0x01
#define MAX7219_DECODE      0x09
#define MAX7219_INTENSITY   0x0A
#define MAX7219_SCANLIMIT   0x0B
#define MAX7219_SHUTDOWN    0x0C
#define MAX7219_DISPLAYTEST 0x0F

typedef struct {
    int din, clk, cs;
    uint8_t modules;
    uint8_t brightness;          // register value 0..15
    const uint8_t *fb;           // RGB888, width = 8*modules, height = 8
    uint16_t width;
} max7219_ctx_t;

static void m_write(max7219_ctx_t *c, uint8_t reg, uint8_t data)
{
    uint16_t frame = ((uint16_t)reg << 8) | data;
    for (int b = 15; b >= 0; b--) {
        gpio_set_level(c->din, (frame >> b) & 1);
        gpio_set_level(c->clk, 0);
        esp_rom_delay_us(1);
        gpio_set_level(c->clk, 1);
        esp_rom_delay_us(1);
    }
}

static void m_write_all(max7219_ctx_t *c, uint8_t reg, uint8_t data)
{
    for (int m = 0; m < c->modules; m++)
        m_write(c, reg, data);
}

static esp_err_t max_init(const matrix_cfg_t *cfg, void **ctx, uint8_t *fb, size_t fb_len)
{
    (void)fb_len;
    if (cfg->pins[0] < 0 || cfg->pins[1] < 0 || cfg->pins[2] < 0) {
        ESP_LOGE(TAG, "DIN/CLK/CS pins not configured");
        return ESP_ERR_INVALID_ARG;
    }
    int modules = cfg->modules ? cfg->modules : (cfg->width / 8);
    if (modules < 1) modules = 1;
    if (modules > 8) modules = 8;

    max7219_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) return ESP_ERR_NO_MEM;
    c->din = cfg->pins[0];
    c->clk = cfg->pins[1];
    c->cs = cfg->pins[2];
    c->modules = (uint8_t)modules;
    c->brightness = (uint8_t)((cfg->brightness * 15) / 100);
    c->fb = fb;
    c->width = cfg->width;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << c->din) | (1ULL << c->clk) | (1ULL << c->cs),
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&io) != ESP_OK) {
        free(c);
        return ESP_FAIL;
    }
    gpio_set_level(c->cs, 1);

    gpio_set_level(c->cs, 0);
    m_write_all(c, MAX7219_SHUTDOWN, 0x01);     // normal operation
    m_write_all(c, MAX7219_DISPLAYTEST, 0x00);
    m_write_all(c, MAX7219_DECODE, 0x00);       // raw (no BCD)
    m_write_all(c, MAX7219_SCANLIMIT, 0x07);    // 8 rows
    m_write_all(c, MAX7219_INTENSITY, c->brightness);
    gpio_set_level(c->cs, 1);

    *ctx = c;
    ESP_LOGI(TAG, "MAX7219 %d module(s) ready (DIN=%d CLK=%d CS=%d, intensity %u)",
             c->modules, c->din, c->clk, c->cs, c->brightness);
    return ESP_OK;
}

static void max_deinit(void *ctx) { free(ctx); }

// Push the framebuffer to the chain. Runs with the display mutex held.
// Column-major: module m, row r -> a byte whose bit b is pixel (m*8 + r? ) ...
// MAX7219 stores a column per DIGIT register in row-major; we map the logical
// bitmap directly: DIGIT(row) holds 8 horizontal bits of that row for the
// module.
static void max_flush(void *ctx)
{
    max7219_ctx_t *c = (max7219_ctx_t *)ctx;
    if (!c || !c->fb) return;

    uint8_t rows[8][8]; // [module][row]
    memset(rows, 0, sizeof(rows));
    for (int r = 0; r < 8; r++) {
        for (int x = 0; x < c->width; x++) {
            int m = x / 8;
            int bit = x % 8;
            if (m >= c->modules) break;
            const uint8_t *p = c->fb + ((size_t)r * c->width + x) * 3;
            if (p[0] | p[1] | p[2])
                rows[m][r] |= (uint8_t)(1 << bit);
        }
    }

    for (int r = 0; r < 8; r++) {
        for (int m = c->modules - 1; m >= 0; m--) {
            m_write(c, MAX7219_DIGIT0 + r, rows[m][r]);
        }
    }
}

static void max_set_brightness(void *ctx, uint8_t percent)
{
    max7219_ctx_t *c = (max7219_ctx_t *)ctx;
    if (!c) return;
    c->brightness = (uint8_t)((percent * 15) / 100);
    m_write_all(c, MAX7219_INTENSITY, c->brightness);
}

static const matrix_backend_t MAX7219 = {
    .name = "max7219",
    .init = max_init,
    .deinit = max_deinit,
    .flush = max_flush,
    .set_brightness = max_set_brightness,
};

const matrix_backend_t *matrix_max7219_backend(void) { return &MAX7219; }
