#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mic.h"
#include "fusion.h"

// Sound and motion styles that draw the whole strip at once and keep state between frames (rings travelling, a
// scrolling history, a falling peak dot, a bubble gliding). Plain C with no ESP-IDF calls, so test/fx_test.c runs it
// on the host. The motion styles (bubble..height) read the IMU through fusion.c.
typedef enum { FX_EQ, FX_RIPPLE, FX_MIRROR, FX_CHROMA, FX_FLOW,
               FX_BUBBLE, FX_TILT, FX_SHAKE, FX_SPIN, FX_SLOSH, FX_HEIGHT,
               FX_AURORA, FX_FIRE, FX_BOUNCE, FX_COMET, FX_N } fx_t;
#define FX_FIRST_MOTION FX_BUBBLE
#define FX_FIRST_FUSED FX_AURORA     // aurora..comet listen to the mic and feel the IMU at once
#define FX_PAL 16                    // gradient samples handed to the fused styles
#define FX_MAX_LEDS 1000      // = MAX_LEDS in board.h

// How fast the styles fall back after a sound, 1..100: the release time runs from about 1 s (1) to 40 ms (100).
// Attacks are always instant.
void fx_set_react(uint8_t react);
// Height style: the colour steps every this many centimetres of height (1..500).
void fx_set_hstep(uint16_t cm);
// Fused styles' colours: lut holds FX_PAL samples of the user's gradient, start to end; on = use it instead of the
// style's own colours (aurora's greens and violets, fire's embers, a rainbow for bounce and comet).
void fx_set_palette(const uint8_t lut[FX_PAL][3], bool on);
// One frame of style k into out[0..n): full-brightness colours, the caller applies the level and the power cap.
// dt_ms: time since the previous frame. base: the solid colour (the ripple's background, the tilt and liquid colour).
// m: the latest motion snapshot (all zero without an IMU: the motion styles then sit still).
void fx_render(fx_t k, const mic_snap_t *a, const motion_snap_t *m, uint8_t (*out)[3], int n, uint32_t dt_ms,
               const uint8_t base[3]);
