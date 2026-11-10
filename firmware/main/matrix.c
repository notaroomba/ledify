#include <string.h>
#include "matrix.h"

int matrix_index(const matrix_t *m, int x, int y)
{
    int w = m->w, h = m->h;
    if (x < 0 || y < 0 || x >= w || y >= h) {
        return -1;
    }
    if (m->lay & MATRIX_RIGHT) {
        x = w - 1 - x;
    }
    if (m->lay & MATRIX_BOTTOM) {
        y = h - 1 - y;
    }
    if (m->lay & MATRIX_COLS) {
        if ((m->lay & MATRIX_SERP) && (x & 1)) {
            y = h - 1 - y;
        }
        return x * h + y;
    }
    if ((m->lay & MATRIX_SERP) && (y & 1)) {
        x = w - 1 - x;
    }
    return y * w + x;
}

bool matrix_parse(const uint8_t *b, int len, int *w, int *h, const uint8_t **rgb)
{
    if (len < MATRIX_HDR || b[0] != 'L' || b[1] != 'M' || !b[2] || !b[3]) {
        return false;
    }
    if (len < MATRIX_HDR + b[2] * b[3] * 3) {
        return false;
    }
    *w = b[2];
    *h = b[3];
    *rgb = b + MATRIX_HDR;
    return true;
}

int matrix_blit(const matrix_t *m, const uint8_t *rgb, int iw, int ih, uint8_t (*out)[3], int n)
{
    if (!m->w || !m->h) {
        int k = iw * ih < n ? iw * ih : n;
        memcpy(out, rgb, k * 3);
        return k;
    }
    int total = m->w * m->h;
    if (total > n) {
        total = n;
    }
    memset(out, 0, total * 3);
    for (int y = 0; y < m->h; y++) {
        const uint8_t *row = rgb + (y * ih / m->h) * iw * 3;
        for (int x = 0; x < m->w; x++) {
            int i = matrix_index(m, x, y);
            if (i >= 0 && i < n) {
                memcpy(out[i], row + (x * iw / m->w) * 3, 3);
            }
        }
    }
    return total;
}
