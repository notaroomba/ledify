#pragma once
#include <stdint.h>

// 256-point radix-2 FFT in int32 with Q15 twiddles: no FPU, no esp-dsp. In place, natural order in and out.
// Input up to +/-32767 never overflows (growth is bounded by 256 * sqrt 2).
#define FFT_N 256
void fft256(int32_t *re, int32_t *im);
// Mean |re|+|im| per band of a Hann-windowed block sampled at 16 kHz: bass 60-250 Hz, mid 250-2000 Hz,
// treble 2-8 kHz. A full-scale sine inside a band gives roughly 32 * 32767 / bins-in-band.
void fft_bands(const int16_t x[FFT_N], uint32_t out[3]);
