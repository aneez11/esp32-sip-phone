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
#include <stdbool.h>
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
    bool chain_reversed;         // module nearest the ESP32 is the rightmost panel
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

// Write one register to the whole chain with its own LOAD pulse.
//
// A MAX7219 only latches on the rising edge of CS, and the chain is one
// 16*modules-bit shift register. Shifting several *different* registers before
// a single pulse (as the init used to do) means only the last 64 bits survive,
// so the earlier writes are silently dropped. That left the chips in their
// power-on state - display test on, i.e. every LED lit.
static void m_write_reg_chain(max7219_ctx_t *c, uint8_t reg, uint8_t data)
{
    gpio_set_level(c->cs, 0);
    m_write_all(c, reg, data);
    gpio_set_level(c->cs, 1);
    esp_rom_delay_us(2);
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
    c->chain_reversed = cfg->chain_reversed;
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

    // Each register needs its own LOAD pulse. Display test is on after power-up
    // and must be the first thing cleared, otherwise the panel stays fully lit.
    m_write_reg_chain(c, MAX7219_DISPLAYTEST, 0x00);
    m_write_reg_chain(c, MAX7219_SHUTDOWN, 0x01);     // normal operation
    m_write_reg_chain(c, MAX7219_DECODE, 0x00);       // raw (no BCD)
    m_write_reg_chain(c, MAX7219_SCANLIMIT, 0x07);    // 8 rows
    m_write_reg_chain(c, MAX7219_INTENSITY, c->brightness);

    // Clear all digit registers so no stale pattern is latched.
    gpio_set_level(c->cs, 0);
    for (int r = 0; r < 8; r++)
        m_write_all(c, MAX7219_DIGIT0 + r, 0x00);
    gpio_set_level(c->cs, 1);

    *ctx = c;
    ESP_LOGI(TAG, "MAX7219 %d module(s) ready (DIN=%d CLK=%d CS=%d, intensity %u, chain %s)",
             c->modules, c->din, c->clk, c->cs, c->brightness,
             c->chain_reversed ? "right-to-left" : "left-to-right");
    return ESP_OK;
}

static void max_deinit(void *ctx) { free(ctx); }

// Push the framebuffer to the chain. Runs with the display mutex held.
//
// The MAX7219 latches a frame on the rising edge of LOAD/CS, so every update
// must be wrapped in CS low ... CS high. Previously the flush never toggled CS,
// so nothing on the panel ever changed after init.
//
// Bit order: bit 7 of a DIGIT register is the LEFTMOST LED of that module
// (verified with tools/max7219_test), so pixel x maps to (0x80 >> (x % 8)).
static void max_flush(void *ctx)
{
    max7219_ctx_t *c = (max7219_ctx_t *)ctx;
    if (!c || !c->fb) return;

    uint8_t rows[8][8]; // [module][row]
    memset(rows, 0, sizeof(rows));
    for (int r = 0; r < 8; r++) {
        for (int x = 0; x < c->width; x++) {
            int m = x / 8;
            if (m >= c->modules) break;
            const uint8_t *p = c->fb + ((size_t)r * c->width + x) * 3;
            if (p[0] | p[1] | p[2])
                rows[m][r] |= (uint8_t)(0x80 >> (x % 8));
        }
    }

    for (int r = 0; r < 8; r++) {
        gpio_set_level(c->cs, 0);
        // First frame shifted out ends up in the module farthest from the
        // ESP32. With a right-to-left chain, logical module 0 (screen left) is
        // the farthest one, so it must be shifted first.
        for (int slot = 0; slot < c->modules; slot++) {
            int m = c->chain_reversed ? slot : (c->modules - 1 - slot);
            m_write(c, MAX7219_DIGIT0 + r, rows[m][r]);
        }
        gpio_set_level(c->cs, 1);
    }
}

static void max_set_brightness(void *ctx, uint8_t percent)
{
    max7219_ctx_t *c = (max7219_ctx_t *)ctx;
    if (!c) return;
    c->brightness = (uint8_t)((percent * 15) / 100);
    gpio_set_level(c->cs, 0); // latch the new intensity
    m_write_all(c, MAX7219_INTENSITY, c->brightness);
    gpio_set_level(c->cs, 1);
}

static const matrix_backend_t MAX7219 = {
    .name = "max7219",
    .init = max_init,
    .deinit = max_deinit,
    .flush = max_flush,
    .set_brightness = max_set_brightness,
};

const matrix_backend_t *matrix_max7219_backend(void) { return &MAX7219; }
