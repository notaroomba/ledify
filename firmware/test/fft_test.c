// Host check for main/fft.c:  cc -O2 -I../main fft_test.c ../main/fft.c -lm && ./a.out
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fft.h"

static void tone(int16_t x[FFT_N], float hz, float amp)
{
    for (int i = 0; i < FFT_N; i++) {
        x[i] = (int16_t)(amp * 32767 * sinf(6.2831853f * hz * i / 16000));
    }
}

int main(void)
{
    int32_t re[FFT_N], im[FFT_N];
    for (int i = 0; i < FFT_N; i++) {               // full-scale cosine in bin 10: N/2 * A at bins 10 and 246
        re[i] = (int32_t)(32767 * cosf(6.2831853f * 10 * i / FFT_N));
        im[i] = 0;
    }
    fft256(re, im);
    for (int k = 0; k < FFT_N; k++) {
        long mag = labs(re[k]) + labs(im[k]);
        if (k == 10 || k == FFT_N - 10) {
            assert(mag > 32767L * 120 && mag < 32767L * 136);
        } else {
            assert(mag < 32767L * 2);
        }
    }

    int16_t x[FFT_N];
    uint32_t b[3];
    struct { float hz; int band; } cases[] = {{100, 0}, {1000, 1}, {4000, 2}};
    for (int c = 0; c < 3; c++) {
        tone(x, cases[c].hz, 0.5f);
        fft_bands(x, b);
        for (int k = 0; k < 3; k++) {
            if (k != cases[c].band) {
                assert(b[cases[c].band] > 4 * b[k]);
            }
        }
    }
    tone(x, 100, 0);
    fft_bands(x, b);
    assert(b[0] == 0 && b[1] == 0 && b[2] == 0);
    // 16-band spectrum: a tone lands in the band whose bin edges hold it, and the three-band sums are unchanged
    static const float tones[] = {100, 300, 1000, 3000, 7000};
    for (int t = 0; t < 5; t++) {
        int16_t x[FFT_N];
        uint32_t three[3], three2[3], spec[FFT_SPEC];
        tone(x, tones[t], 0.5f);
        fft_spectrum(x, three, spec);
        fft_bands(x, three2);
        assert(!memcmp(three, three2, sizeof three));
        int best = 0;
        for (int k = 1; k < FFT_SPEC; k++) {
            best = spec[k] > spec[best] ? k : best;
        }
        float bin = tones[t] / 62.5f;
        assert(bin >= fft_spec_edge[best] - 1 && bin < fft_spec_edge[best + 1] + 1);
    }
    puts("fft ok");
    return 0;
}
