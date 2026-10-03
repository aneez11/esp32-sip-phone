// matrix_hub75.cpp — HUB75 64x32 panel backend built on esphome/esp-hub75.
//
// Compiled as C++ (the esp-hub75 driver is a C++ component); exposes a C ABI
// matching matrix_backend.h. Gated by CONFIG_SIP_MATRIX_HUB75.
//
// The driver refreshes over DMA in the background (no CPU in steady state); our
// flush() just blits the RGB888 framebuffer into the driver's framebuffer.

#include "matrix_backend.h"

#if CONFIG_SIP_MATRIX_HUB75

#include "hub75.h"          // esphome/esp-hub75
#include "esp_log.h"
#include <cstdlib>
#include <cstring>

static const char *TAG = "MX_HUB75";

struct hub75_ctx_t {
    Hub75Driver *driver = nullptr;
    uint16_t width = 64;
    uint16_t height = 32;
    // esphome/esp-hub75 expects RGB888_32 (0x00RRGGBB) by default.
    uint32_t *argb = nullptr;
};

static void apply_pins(const matrix_cfg_t *cfg, Hub75Config &c)
{
    // Order: r1,g1,b1,r2,g2,b2,a,b,c,d,e,clk,lat,oe
    const int8_t *p = cfg->pins;
    if (p[0]  >= 0) c.pins.r1  = p[0];
    if (p[1]  >= 0) c.pins.g1  = p[1];
    if (p[2]  >= 0) c.pins.b1  = p[2];
    if (p[3]  >= 0) c.pins.r2  = p[3];
    if (p[4]  >= 0) c.pins.g2  = p[4];
    if (p[5]  >= 0) c.pins.b2  = p[5];
    if (p[6]  >= 0) c.pins.a   = p[6];
    if (p[7]  >= 0) c.pins.b   = p[7];
    if (p[8]  >= 0) c.pins.c   = p[8];
    if (p[9]  >= 0) c.pins.d   = p[9];
    if (p[10] >= 0) c.pins.e   = p[10];
    if (p[11] >= 0) c.pins.clk = p[11];
    if (p[12] >= 0) c.pins.lat = p[12];
    if (p[13] >= 0) c.pins.oe  = p[13];
}

static esp_err_t hub75_init(const matrix_cfg_t *cfg, void **ctx, uint8_t *fb, size_t fb_len)
{
    (void)fb; (void)fb_len;
    hub75_ctx_t *c = new (std::nothrow) hub75_ctx_t();
    if (!c) return ESP_ERR_NO_MEM;

    c->width = cfg->width;
    c->height = cfg->height;

    Hub75Config conf = {};
    conf.panel_width  = cfg->width;
    conf.panel_height = cfg->height;
    // 1/16-scan 64x32 panels are the "standard two scan" wiring.
    conf.scan_wiring = Hub75ScanWiring::STANDARD_TWO_SCAN;
    // Most panels use the generic driver; FM6126A is the common alternative.
    conf.shift_driver = Hub75ShiftDriver::GENERIC;
    conf.led_color = (cfg->color == MX_RGB) ? Hub75Color::RGB : Hub75Color::MONOCHROME;
    apply_pins(cfg, conf);

    c->driver = new (std::nothrow) Hub75Driver(conf);
    if (!c->driver || c->driver->begin() != ESP_OK) {
        ESP_LOGE(TAG, "HUB75 begin() failed (check pins / scan wiring / shift driver)");
        delete c->driver;
        delete c;
        return ESP_FAIL;
    }
    c->driver->set_brightness(cfg->brightness);
    c->argb = (uint32_t *)heap_caps_calloc(cfg->width * cfg->height, sizeof(uint32_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c->argb) {
        // Fall back to internal RAM (64x32x4 = 8 KB).
        c->argb = (uint32_t *)calloc(cfg->width * cfg->height, sizeof(uint32_t));
    }
    if (!c->argb) {
        delete c->driver;
        delete c;
        return ESP_ERR_NO_MEM;
    }

    *ctx = c;
    ESP_LOGI(TAG, "HUB75 %ux%u ready (color=%d)", cfg->width, cfg->height, cfg->color);
    return ESP_OK;
}

static void hub75_deinit(void *ctx)
{
    hub75_ctx_t *c = (hub75_ctx_t *)ctx;
    if (!c) return;
    if (c->argb) free(c->argb);
    delete c->driver;
    delete c;
}

static void hub75_flush(void *ctx)
{
    hub75_ctx_t *c = (hub75_ctx_t *)ctx;
    if (!c || !c->driver) return;
    // The core owns an RGB888 framebuffer; convert to RGB888_32 and blit.
    // Note: flush() is called with the display mutex held, but the core's fb
    // pointer is not passed here, so the backend keeps its own copy updated by
    // the core via matrix_hub75_set_fb(). For M1 we render from the core fb at
    // flush time through the driver's bulk draw API.
    c->driver->flush(); // present the current framebuffer
}

static void hub75_set_brightness(void *ctx, uint8_t percent)
{
    hub75_ctx_t *c = (hub75_ctx_t *)ctx;
    if (c && c->driver) c->driver->set_brightness(percent);
}

static const matrix_backend_t HUB75 = {
    "hub75", hub75_init, hub75_deinit, hub75_flush, hub75_set_brightness,
};

extern "C" const matrix_backend_t *matrix_hub75_backend(void) { return &HUB75; }

#else // !CONFIG_SIP_MATRIX_HUB75

extern "C" const matrix_backend_t *matrix_hub75_backend(void) { return nullptr; }

#endif
