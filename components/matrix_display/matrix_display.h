#ifndef MATRIX_DISPLAY_H
#define MATRIX_DISPLAY_H

// matrix_display — backend-agnostic LED matrix display for caller ID, queue
// count and alerts. See docs/FEATURE-PLAN.md.
//
// The component owns a small logical framebuffer (RGB888). Callers draw text /
// numbers / scrolls into it; the backend flushes it to the panel:
//   MX_HUB75    - HUB75 64x32 (mono or RGB) via the esp-hub75 DMA driver
//   MX_MAX7219  - chain of 8x8 MAX7219 modules (single-line ticker)
//   MX_PREVIEW  - no hardware: renders ASCII to the log / web UI
//
// Threading: every public draw function takes an internal mutex, so it is safe
// to call from any task. Heavy backends (HUB75) should still be driven from the
// dedicated matrix task, not from the SIP task.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MX_NONE = 0,
    MX_HUB75,
    MX_MAX7219,
    MX_PREVIEW,
} matrix_type_t;

typedef enum {
    MX_MONO_R = 0,  // monochrome panel, red channel populated
    MX_MONO_G,      // monochrome panel, green channel populated
    MX_MONO_B,      // monochrome panel, blue channel populated
    MX_RGB,         // full colour panel
} matrix_color_t;

typedef enum {
    MX_LAYOUT_2D = 0,   // full width x height bitmap (HUB75 64x32)
    MX_LAYOUT_LINE,     // single 8-px-tall scrolling line (MAX7219 array)
} matrix_layout_t;

// Panel configuration. `pins` is backend-specific:
//   HUB75   : [0..13] = r1,g1,b1,r2,g2,b2,a,b,c,d,e,clk,lat,oe  (-1 = unused)
//   MAX7219 : [0..2]  = din,clk,cs                                 (-1 = unused)
typedef struct {
    matrix_type_t   type;
    matrix_color_t  color;
    matrix_layout_t layout;
    uint16_t        width;      // HUB75: 64  MAX7219: 8 * modules
    uint16_t        height;     // HUB75: 32  MAX7219: 8
    uint8_t         modules;    // MAX7219 chain length
    uint8_t         brightness; // 0-100
    int8_t          pins[14];
} matrix_cfg_t;

// ---- lifecycle ----
// Returns NULL on failure (e.g. panel did not init). Safe to call with a
// type of MX_NONE (returns NULL). The config is copied.
struct matrix_display_s;
typedef struct matrix_display_s *matrix_display_handle_t;

matrix_display_handle_t matrix_display_init(const matrix_cfg_t *cfg);
void                    matrix_display_delete(matrix_display_handle_t h);

// ---- drawing (mutex-protected, no backend I/O on callers' task) ----
void matrix_clear(matrix_display_handle_t h);
void matrix_set_brightness(matrix_display_handle_t h, uint8_t percent);

// Draw text at (x,y). scale 1..4. Returns the pixel width consumed.
int  matrix_draw_text(matrix_display_handle_t h, int x, int y,
                      const char *text, uint8_t scale, uint32_t rgb);
// Draw text horizontally centred on its row (ignored in MX_LAYOUT_LINE).
void matrix_draw_text_center(matrix_display_handle_t h, int y,
                             const char *text, uint8_t scale, uint32_t rgb);
int  matrix_draw_number(matrix_display_handle_t h, int x, int y,
                        int value, uint8_t scale, uint32_t rgb);

// Scrolling ticker: copies the text and lets the backend task animate it.
void matrix_scroll_begin(matrix_display_handle_t h, const char *text,
                         uint8_t scale, uint32_t rgb, uint16_t px_per_s);
void matrix_scroll_stop(matrix_display_handle_t h);

// Flush the logical framebuffer to the backend now (no-op for scroll owner).
void matrix_flush(matrix_display_handle_t h);

// ---- introspection (web preview / tests) ----
// Render the logical framebuffer as ASCII (# = on, space = off), one line per
// row, '\n' separated. Writes at most out_len bytes (always NUL-terminated).
void matrix_get_ascii(matrix_display_handle_t h, char *out, size_t out_len);
// True when the panel has a colour channel (drawing uses RGB). Mono panels
// use the same framebuffer but the backend maps any lit pixel to its channel.
bool matrix_has_color(matrix_display_handle_t h);

#ifdef __cplusplus
}
#endif

#endif // MATRIX_DISPLAY_H
