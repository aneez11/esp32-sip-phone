#ifndef MATRIX_FONT5X7_H
#define MATRIX_FONT5X7_H

#include <stdint.h>
#include <stddef.h>

// Classic 5x7 bitmap font for ASCII 0x20-0x7E. Each glyph is 5 column bytes
// (LSB = top row, 7 rows used). Unknown characters render as a blank glyph.
#define FONT5X7_W 5
#define FONT5X7_H 7
#define FONT5X7_FIRST 0x20
#define FONT5X7_LAST  0x7E

// Returns a pointer to the 5-byte glyph for `c`, or the space glyph when out of
// range. Never returns NULL.
const uint8_t *font5x7_glyph(char c);

#endif // MATRIX_FONT5X7_H
