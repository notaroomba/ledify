#pragma once
#include <stdint.h>
#include "mic.h"

// Sound styles that draw the whole strip at once and keep state between frames (rings travelling, a scrolling
// history, a falling peak dot). Plain C with no ESP-IDF calls, so test/fx_test.c runs it on the host.
typedef enum { FX_EQ, FX_RIPPLE, FX_MIRROR, FX_CHROMA, FX_FLOW, FX_N } fx_t;
#define FX_MAX_LEDS 1000      // = MAX_LEDS in board.h

// How fast the styles fall back after a sound, 1..100: the release time runs from about 1 s (1) to 40 ms (100).
// Attacks are always instant.
void fx_set_react(uint8_t react);
// One frame of style k into out[0..n): full-brightness colours, the caller applies the level and the power cap.
// dt_ms: time since the previous frame. base: the solid colour, used as the ripple's background tint.
void fx_render(fx_t k, const mic_snap_t *a, uint8_t (*out)[3], int n, uint32_t dt_ms, const uint8_t base[3]);
