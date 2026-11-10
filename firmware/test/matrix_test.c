// Host check for main/matrix.c:  cc -O2 -I../main matrix_test.c ../main/matrix.c && ./a.out
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "matrix.h"

static uint8_t out[64][3];

// Every layout must put each grid pixel on its own strip pixel.
static void bijective(matrix_t m)
{
    int seen[64] = {0};
    for (int y = 0; y < m.h; y++) {
        for (int x = 0; x < m.w; x++) {
            int i = matrix_index(&m, x, y);
            assert(i >= 0 && i < m.w * m.h);
            seen[i]++;
        }
    }
    for (int i = 0; i < m.w * m.h; i++) {
        assert(seen[i] == 1);
    }
}

int main(void)
{
    // 4 x 3, rows, progressive, from the top left: plain reading order.
    matrix_t m = {4, 3, 0};
    assert(matrix_index(&m, 0, 0) == 0 && matrix_index(&m, 3, 0) == 3 && matrix_index(&m, 0, 1) == 4 && matrix_index(&m, 3, 2) == 11);
    assert(matrix_index(&m, 4, 0) == -1 && matrix_index(&m, 0, 3) == -1 && matrix_index(&m, -1, 0) == -1);

    // Serpentine: the second row runs back from the right.
    m.lay = MATRIX_SERP;
    assert(matrix_index(&m, 3, 1) == 4 && matrix_index(&m, 0, 1) == 7 && matrix_index(&m, 0, 2) == 8);

    // Serpentine from the bottom right: pixel 0 bottom right, running left, then back along the row above.
    m.lay = MATRIX_SERP | MATRIX_RIGHT | MATRIX_BOTTOM;
    assert(matrix_index(&m, 3, 2) == 0 && matrix_index(&m, 0, 2) == 3 && matrix_index(&m, 0, 1) == 4 && matrix_index(&m, 3, 0) == 8);

    // Columns, serpentine, from the top left: down the first column, up the second.
    m.lay = MATRIX_COLS | MATRIX_SERP;
    assert(matrix_index(&m, 0, 0) == 0 && matrix_index(&m, 0, 2) == 2 && matrix_index(&m, 1, 2) == 3 && matrix_index(&m, 1, 0) == 5 && matrix_index(&m, 2, 0) == 6);

    // Columns, progressive, from the bottom left: every column goes up.
    m.lay = MATRIX_COLS | MATRIX_BOTTOM;
    assert(matrix_index(&m, 0, 2) == 0 && matrix_index(&m, 0, 0) == 2 && matrix_index(&m, 1, 2) == 3);

    for (int lay = 0; lay < 16; lay++) {
        bijective((matrix_t){4, 3, lay});
        bijective((matrix_t){5, 5, lay});
        bijective((matrix_t){1, 7, lay});
    }

    // Frame parsing.
    uint8_t f[MATRIX_HDR + 4 * 3 * 3] = {'L', 'M', 4, 3};
    int w, h;
    const uint8_t *rgb;
    assert(matrix_parse(f, sizeof f, &w, &h, &rgb) && w == 4 && h == 3 && rgb == f + MATRIX_HDR);
    assert(!matrix_parse(f, sizeof f - 1, &w, &h, &rgb));          // short
    f[0] = 'X';
    assert(!matrix_parse(f, sizeof f, &w, &h, &rgb));              // not a frame
    f[0] = 'L';
    f[2] = 0;
    assert(!matrix_parse(f, sizeof f, &w, &h, &rgb));              // empty
    f[2] = 4;

    // Blit, same size, serpentine: the picture's top right lands on strip pixel 3, the second row's right end on 4.
    uint8_t *px = f + MATRIX_HDR;
    for (int i = 0; i < 12; i++) {
        px[i * 3] = i;                                             // red = row-major position in the picture
    }
    m = (matrix_t){4, 3, MATRIX_SERP};
    memset(out, 0xee, sizeof out);
    assert(matrix_blit(&m, px, 4, 3, out, 64) == 12);
    assert(out[3][0] == 3 && out[4][0] == 7 && out[7][0] == 4 && out[8][0] == 8);
    assert(out[12][0] == 0xee);                                     // past the matrix: untouched

    // Blit, scaled: a 2 x 1 picture (left red, right blue) on the 4 x 3 matrix fills each half.
    uint8_t two[6] = {255, 0, 0, 0, 0, 255};
    assert(matrix_blit(&m, two, 2, 1, out, 64) == 12);
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 4; x++) {
            int i = matrix_index(&m, x, y);
            assert(x < 2 ? out[i][0] == 255 && out[i][2] == 0 : out[i][0] == 0 && out[i][2] == 255);
        }
    }

    // A strip shorter than the matrix: nothing written past it.
    memset(out, 0xee, sizeof out);
    assert(matrix_blit(&m, px, 4, 3, out, 5) == 5);
    assert(out[5][0] == 0xee);

    // No matrix: the picture is laid along the strip in reading order.
    m = (matrix_t){0, 0, 0};
    assert(matrix_blit(&m, px, 4, 3, out, 64) == 12 && out[7][0] == 7);
    assert(matrix_blit(&m, px, 4, 3, out, 6) == 6);

    printf("matrix: all good\n");
    return 0;
}
