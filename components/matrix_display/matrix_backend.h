#ifndef MATRIX_BACKEND_H
#define MATRIX_BACKEND_H

// Internal backend interface for matrix_display. Not part of the public API.

#include "matrix_display.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    // Bind to the panel. `fb` is the RGB888 framebuffer the core owns.
    esp_err_t (*init)(const matrix_cfg_t *cfg, void **ctx, uint8_t *fb, size_t fb_len);
    void      (*deinit)(void *ctx);
    void      (*flush)(void *ctx);
    void      (*set_brightness)(void *ctx, uint8_t percent);
} matrix_backend_t;

// Returns the backend for a type (never NULL; MX_NONE -> preview).
const matrix_backend_t *matrix_backend_select(matrix_type_t type);

// Scroll tick, driven by the matrix task. Declared for the task to call.
struct matrix_display_s;
void matrix_tick(struct matrix_display_s *d, uint32_t dt_ms);

#ifdef __cplusplus
}
#endif

#endif // MATRIX_BACKEND_H
