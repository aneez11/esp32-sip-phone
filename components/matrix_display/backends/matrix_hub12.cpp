// matrix_hub12.c — minimal bit-bang driver for HUB12 32x16 panels (1/4 scan).
//
// HUB12 is the 12-pin cousin of HUB75: R1,G1,B1,R2,G2,B2,A,B,CLK,LAT,OE.
// 1/4 scan = 4 address banks of 4 rows each (A,B select the bank). The driver
// refreshes in a background task because a single flush() at 10 Hz would
// flicker badly.
//
// This is a bring-up driver: timing is conservative (slow clock) and it is not
// optimised. It is selected when matrix_type == MX_HUB75 and matrix_height
// <= 16 with scan == 1 (1/4) — i.e. a HUB12 panel. For 64x32 panels the
// (still unavailable) esphome/esp-hub75 DMA driver remains the target.

#include "matrix_backend.h"
#include "matrix_display.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MX_HUB12";

#define HUB12_REFRESH_HZ  120
#define HUB12_OE_US       1000   // output-enable time per bank (~1 ms)

typedef struct {
    int r1, g1, b1, r2, g2, b2;
    int a, b, clk, lat, oe;
    uint16_t width, height;
    const uint8_t *fb;
    size_t fb_len;
    volatile bool running;
    TaskHandle_t task;
} hub12_ctx_t;

static void hub12_refresh_task(void *arg) {
    hub12_ctx_t *c = static_cast<hub12_ctx_t *>(arg);
    uint8_t bank = 0;
    while (c->running) {
        // address lines
        gpio_set_level((gpio_num_t)c->a, bank & 1);
        gpio_set_level((gpio_num_t)c->b, (bank >> 1) & 1);

        // shift 32 columns x 2 rows (upper + lower)
        gpio_set_level((gpio_num_t)c->clk, 0);
        gpio_set_level((gpio_num_t)c->lat, 0);
        for (int col = c->width - 1; col >= 0; col--) {
            int row_up = bank;          // 0..3 -> rows 0,4,8,12
            int row_lo = bank + 4;      // rows 4,8,12,16 (clamped below)
            if (row_lo >= c->height) row_lo = c->height - 1;
            // upper row
            const uint8_t *pu = c->fb + ((size_t)row_up * c->width + col) * 3;
            uint8_t on_up = (pu[0] | pu[1] | pu[2]) ? 1 : 0;
            // lower row
            const uint8_t *pl = c->fb + ((size_t)row_lo * c->width + col) * 3;
            uint8_t on_lo = (pl[0] | pl[1] | pl[2]) ? 1 : 0;
            // shift one bit per channel (mono: on/off)
            gpio_set_level((gpio_num_t)c->clk, 0);
            gpio_set_level((gpio_num_t)c->r1, on_up); gpio_set_level((gpio_num_t)c->g1, on_up); gpio_set_level((gpio_num_t)c->b1, on_up);
            gpio_set_level((gpio_num_t)c->r2, on_lo); gpio_set_level((gpio_num_t)c->g2, on_lo); gpio_set_level((gpio_num_t)c->b2, on_lo);
            gpio_set_level((gpio_num_t)c->clk, 1);
        }
        gpio_set_level((gpio_num_t)c->lat, 1);
        gpio_set_level((gpio_num_t)c->oe, 0);  // enable output (active low)
        esp_rom_delay_us(HUB12_OE_US);
        gpio_set_level((gpio_num_t)c->oe, 1);  // disable
        bank = (bank + 1) & 3;
        vTaskDelay(pdMS_TO_TICKS(1000 / (HUB12_REFRESH_HZ * 4)));
    }
    vTaskDelete(NULL);
}

static esp_err_t hub12_init(const matrix_cfg_t *cfg, void **ctx, uint8_t *fb, size_t fb_len) {
    hub12_ctx_t *c = (hub12_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return ESP_ERR_NO_MEM;
    c->r1 = cfg->pins[0]; c->g1 = cfg->pins[1]; c->b1 = cfg->pins[2];
    c->r2 = cfg->pins[3]; c->g2 = cfg->pins[4]; c->b2 = cfg->pins[5];
    c->a = cfg->pins[6]; c->b = cfg->pins[7];
    c->clk = cfg->pins[11]; c->lat = cfg->pins[12]; c->oe = cfg->pins[13];
    c->width = cfg->width ? cfg->width : 32;
    c->height = cfg->height ? cfg->height : 16;
    c->fb = fb;
    c->fb_len = fb_len;

    // configure pins
    int pins[] = { c->r1, c->g1, c->b1, c->r2, c->g2, c->b2, c->a, c->b, c->clk, c->lat, c->oe };
    for (int i = 0; i < 11; i++) {
        if (pins[i] < 0) continue;
        gpio_config_t io = { .pin_bit_mask = 1ULL << (unsigned)pins[i], .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
        gpio_config(&io);
        gpio_set_level((gpio_num_t)pins[i], 0);
    }
    c->oe = c->oe; // ensure oe high (disabled) initially
    if (c->oe >= 0) gpio_set_level((gpio_num_t)c->oe, 1);

    c->running = true;
    if (xTaskCreate(hub12_refresh_task, "hub12", 4096, c, 4, &c->task) != pdPASS) {
        free(c);
        return ESP_ERR_NO_MEM;
    }
    *ctx = c;
    ESP_LOGI(TAG, "HUB12 %ux%u ready (R1=%d G1=%d B1=%d R2=%d G2=%d B2=%d A=%d B=%d CLK=%d LAT=%d OE=%d)",
             c->width, c->height, c->r1, c->g1, c->b1, c->r2, c->g2, c->b2, c->a, c->b, c->clk, c->lat, c->oe);
    return ESP_OK;
}

static void hub12_deinit(void *ctx) {
    hub12_ctx_t *c = (hub12_ctx_t *)ctx;
    if (!c) return;
    c->running = false;
    if (c->task) vTaskDelay(pdMS_TO_TICKS(50));
    free(c);
}

static void hub12_flush(void *ctx) {
    // Refresh task reads the framebuffer directly; nothing to do here.
    (void)ctx;
}

static void hub12_set_brightness(void *ctx, uint8_t percent) {
    (void)ctx; (void)percent;
    // HUB12 brightness is controlled by OE duty cycle; not implemented in v1.
}

static const matrix_backend_t HUB12 = {
    "hub12", hub12_init, hub12_deinit, hub12_flush, hub12_set_brightness,
};

extern "C" const matrix_backend_t *matrix_hub12_backend(void) { return &HUB12; }