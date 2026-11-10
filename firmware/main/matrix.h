#pragma once
#include <stdbool.h>
#include <stdint.h>

// LED matrices: how a W x H grid of pixels is wired as one strip, and drawing pictures onto it. Plain C so
// test/matrix_test.c can check it on the host.
//
// Layout bits (the "mlay" setting):
//   MATRIX_SERP    serpentine (zig-zag): every other row (or column) runs back the other way; off = every row starts
//                  at the same side ("progressive")
//   MATRIX_COLS    the strip runs down columns instead of along rows
//   MATRIX_RIGHT   the first pixel is on the right
//   MATRIX_BOTTOM  the first pixel is at the bottom
// Start corner and direction together cover every way a panel can be turned or flipped.
#define MATRIX_SERP   1
#define MATRIX_COLS   2
#define MATRIX_RIGHT  4
#define MATRIX_BOTTOM 8

typedef struct {
    uint16_t w, h;          // 0 x 0: no matrix, pictures are laid along the strip row by row
    uint8_t lay;
} matrix_t;

// Strip index of pixel (x, y), x from the left and y from the top as the viewer sees it; -1 outside the grid.
int matrix_index(const matrix_t *m, int x, int y);

// A pixel frame as the panel sends it: 'L' 'M' width height, then width x height RGB bytes, row by row from the
// top left. Returns false on anything malformed.
#define MATRIX_HDR 4
bool matrix_parse(const uint8_t *b, int len, int *w, int *h, const uint8_t **rgb);

// Draw a row-major RGB picture of iw x ih onto the strip buffer out (n pixels). With a matrix set, the picture is
// scaled (nearest pixel) to the matrix and each pixel goes where the wiring puts it; pixels past the matrix are
// cleared. Without one it is laid along the strip row by row. Returns how many strip pixels were written.
int matrix_blit(const matrix_t *m, const uint8_t *rgb, int iw, int ih, uint8_t (*out)[3], int n);
