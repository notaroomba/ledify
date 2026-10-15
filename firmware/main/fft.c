// Fixed-point FFT for the sound styles. Twiddles come from one Q15 sine table built on first use.
#include <math.h>
#include <stdlib.h>
#include "fft.h"

static int16_t sine[FFT_N];                 // 32767 * sin(2 pi n / N)

static void tables(void)
{
    if (sine[FFT_N / 4]) {
        return;
    }
    for (int i = 0; i < FFT_N; i++) {
        sine[i] = (int16_t)lrintf(32767 * sinf(6.2831853f * i / FFT_N));
    }
}

void fft256(int32_t *re, int32_t *im)
{
    tables();
    for (int i = 1, j = 0; i < FFT_N; i++) {            // bit-reversed order
        int bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            int32_t t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= FFT_N; len <<= 1) {
        int half = len / 2, step = FFT_N / len;
        for (int i = 0; i < FFT_N; i += len) {
            for (int k = 0; k < half; k++) {
                int32_t c = sine[(k * step + FFT_N / 4) & (FFT_N - 1)], s = sine[k * step];
                int32_t *xr = &re[i + k + half], *xi = &im[i + k + half];
                int32_t vr = ((int64_t)*xr * c + (int64_t)*xi * s) >> 15;    // x * e^(-j 2 pi k / len)
                int32_t vi = ((int64_t)*xi * c - (int64_t)*xr * s) >> 15;
                *xr = re[i + k] - vr;
                *xi = im[i + k] - vi;
                re[i + k] += vr;
                im[i + k] += vi;
            }
        }
    }
}

void fft_bands(const int16_t x[FFT_N], uint32_t out[3])
{
    static int32_t re[FFT_N], im[FFT_N];
    static const uint8_t edge[4] = {1, 4, 32, 128};     // 62.5 Hz bins; bin 0 is DC. ponytail: three fixed bands, go to 512 points if the bass needs splitting
    tables();
    for (int i = 0; i < FFT_N; i++) {
        int32_t hann = (32767 - sine[(i + FFT_N / 4) & (FFT_N - 1)]) >> 1;   // (1 - cos) / 2 in Q15
        re[i] = x[i] * hann >> 15;
        im[i] = 0;
    }
    fft256(re, im);
    for (int b = 0; b < 3; b++) {
        uint32_t sum = 0;
        for (int k = edge[b]; k < edge[b + 1]; k++) {
            sum += abs(re[k]) + abs(im[k]);
        }
        out[b] = sum / (edge[b + 1] - edge[b]);
    }
}
