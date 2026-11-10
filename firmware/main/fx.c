// Sound styles that draw the whole strip at once. Every input attacks instantly and falls back with one release
// time set by "react", so a beat lands on the very next frame and the styles don't flicker between beats.
// Integer only per pixel (the C6 has no FPU); one expf per frame for the release factor.
#include <math.h>
#include <string.h>
#include "fx.h"

static uint32_t keep = 200;                  // share of the remaining value kept per ms of release, Q16: see fx_set_react
static uint8_t react_ = 60;
static uint16_t sm_loud, sm_bands[3], sm_spec[MIC_SPEC];   // smoothed inputs, 8.8 fixed point
static uint32_t last_beats;
static bool primed;
static struct { int32_t pos; uint16_t amp; uint8_t hue; } rings[8];   // ripple: pos in 1/256 px from the centre
static uint8_t hist[FX_MAX_LEDS][3];         // flow: what scrolled along so far
static uint32_t flow_acc;                    // flow: sub-pixel travel, 1/256 px
static int32_t peak_q, peak_hold_ms;         // mirror: the peak dot in 1/256 px from the centre and its hold time

void fx_set_react(uint8_t r)
{
    react_ = r < 1 ? 1 : r > 100 ? 100 : r;
}

static void hsv(uint8_t h, uint8_t v, uint8_t *p)
{
    uint8_t region = h / 43, rem = (h - region * 43) * 6, q = 255 - rem, t = rem;
    uint8_t r, g, b;
    switch (region) {
    case 0:  r = 255; g = t;   b = 0;   break;
    case 1:  r = q;   g = 255; b = 0;   break;
    case 2:  r = 0;   g = 255; b = t;   break;
    case 3:  r = 0;   g = q;   b = 255; break;
    case 4:  r = t;   g = 0;   b = 255; break;
    default: r = 255; g = 0;   b = q;   break;
    }
    p[0] = r * v / 255;
    p[1] = g * v / 255;
    p[2] = b * v / 255;
}

// Square law: LEDs look much brighter than their duty cycle at the low end, so quiet passages would read as
// half-lit without it.
static uint8_t gam(uint32_t x)
{
    return x * x / 255;
}

// Instant attack, exponential release: v moves to x at once when x is higher, else keeps `k`/65536 of the gap.
static uint16_t follow(uint16_t v, uint8_t x, uint32_t k)
{
    uint32_t t = (uint32_t)x << 8;
    return t >= v ? t : t + (uint32_t)(((uint64_t)(v - t) * k) >> 16);
}

// Where the sound sits: red for bass, green for mid, blue for treble, as a hue 0..170.
static uint8_t balance_hue(void)
{
    uint32_t b = sm_bands[0] >> 8, m = sm_bands[1] >> 8, t = sm_bands[2] >> 8, s = b + m + t;
    return s ? (m * 85 + t * 170) / s : 0;
}

static void add(uint8_t *p, const uint8_t *c)
{
    for (int ch = 0; ch < 3; ch++) {
        int v = p[ch] + c[ch];
        p[ch] = v > 255 ? 255 : v;
    }
}

static void eq(uint8_t (*out)[3], int n)
{
    for (int i = 0; i < n; i++) {
        int k = i * MIC_SPEC / n;
        hsv(k * 240 / MIC_SPEC, gam(sm_spec[k] >> 8), out[i]);   // red at the bass end through to pink at the top
    }
}

static void ripple(const mic_snap_t *a, uint8_t (*out)[3], int n, uint32_t dt, const uint8_t base[3])
{
    int half = n / 2 > 1 ? n / 2 : 1;
    uint32_t newb = a->beats - last_beats;
    for (uint32_t b = 0; b < newb && b < 2; b++) {               // a new ring per onset, taking the oldest slot
        int o = 0;
        for (int r = 1; r < 8; r++) {
            if (rings[r].amp < rings[o].amp) {
                o = r;
            }
        }
        rings[o].pos = 0;
        rings[o].amp = 160 + (sm_loud >> 8) * 95 / 255;
        rings[o].hue = balance_hue() + b * 40;
    }
    int32_t speed = (half << 8) * 10 / 6;                         // a ring reaches the ends in 0.6 s
    int width = n / 24 + 2;
    uint8_t bg[3];
    uint32_t bl = (sm_loud >> 8) / 6;                             // a faint glow of the base colour with the loudness
    for (int ch = 0; ch < 3; ch++) {
        bg[ch] = base[ch] * bl / 255;
    }
    for (int i = 0; i < n; i++) {
        memcpy(out[i], bg, 3);
    }
    for (int r = 0; r < 8; r++) {
        if (!rings[r].amp) {
            continue;
        }
        rings[r].pos += speed * (int32_t)dt / 1000;
        int pos = rings[r].pos >> 8;
        if (pos > half + width) {
            rings[r].amp = 0;
            continue;
        }
        uint32_t amp = (uint32_t)rings[r].amp * (half + width - pos) / (half + width);   // fades on its way out
        for (int d = pos - width; d <= pos + width; d++) {
            if (d < 0 || d > half) {
                continue;
            }
            int off = d > pos ? d - pos : pos - d;
            uint8_t c[3];
            hsv(rings[r].hue, gam(amp * (width - off + 1) / (width + 1)), c);
            int i1 = n / 2 + d, i2 = (n - 1) / 2 - d;              // both directions from the centre
            if (i1 < n) {
                add(out[i1], c);
            }
            if (i2 >= 0 && i2 != i1) {
                add(out[i2], c);
            }
        }
    }
}

static void mirror(uint8_t (*out)[3], int n, uint32_t dt)
{
    int half = (n + 1) / 2;
    int32_t lit_q = (int32_t)((uint32_t)sm_loud * half / 255);   // 1/256 px
    if (lit_q >= peak_q) {
        peak_q = lit_q;
        peak_hold_ms = 400;
    } else if (peak_hold_ms > 0) {
        peak_hold_ms -= dt;
    } else {
        peak_q -= (half << 8) * (int32_t)dt / 1000;               // then falls the whole half in a second
        peak_q = peak_q < lit_q ? lit_q : peak_q;
    }
    int lit = lit_q >> 8, pk = peak_q >> 8;
    memset(out, 0, (size_t)n * 3);
    for (int d = 0; d < half; d++) {
        uint8_t c[3] = {0, 0, 0};
        if (d < lit) {
            hsv(85 - d * 85 / half, 255, c);                       // green at the centre to red at the ends
        } else if (d == pk && pk > 0) {
            c[0] = c[1] = c[2] = 200;
        }
        int i1 = n / 2 + d, i2 = (n - 1) / 2 - d;
        if (i1 < n) {
            memcpy(out[i1], c, 3);
        }
        if (i2 >= 0) {
            memcpy(out[i2], c, 3);
        }
    }
}

static void chroma(uint8_t (*out)[3], int n)
{
    uint8_t c[3] = {gam(sm_bands[0] >> 8), gam(sm_bands[1] >> 8), gam(sm_bands[2] >> 8)};
    for (int i = 0; i < n; i++) {
        memcpy(out[i], c, 3);
    }
}

static void flow(uint8_t (*out)[3], int n, uint32_t dt)
{
    int speed = n / 2 > 30 ? n / 2 : 30;                          // px/s: the whole strip in about 2 s
    flow_acc += (uint32_t)speed * dt * 256 / 1000;
    int step = flow_acc >> 8;
    flow_acc &= 255;
    if (step > n) {
        step = n;
    }
    if (step) {
        memmove(hist[step], hist[0], (size_t)(n - step) * 3);
        uint8_t c[3];
        hsv(balance_hue(), gam(sm_loud >> 8), c);
        for (int i = 0; i < step; i++) {
            memcpy(hist[i], c, 3);
        }
    } else {
        hsv(balance_hue(), gam(sm_loud >> 8), hist[0]);           // the head always shows the sound now
    }
    memcpy(out, hist, (size_t)n * 3);
}

void fx_render(fx_t k, const mic_snap_t *a, uint8_t (*out)[3], int n, uint32_t dt, const uint8_t base[3])
{
    if (n <= 0) {
        return;
    }
    n = n > FX_MAX_LEDS ? FX_MAX_LEDS : n;
    dt = dt > 200 ? 200 : dt;                                     // after a pause, don't jump
    if (!primed) {
        last_beats = a->beats;
        primed = true;
    }
    // Release: tau from ~1 s (react 1) to 40 ms (react 100), log-spaced; keep = exp(-dt / tau) in Q16.
    float tau = 1000.0f * powf(0.04f, (react_ - 1) / 99.0f);
    keep = (uint32_t)(65536.0f * expf(-(float)dt / tau));
    sm_loud = follow(sm_loud, a->loud, keep);
    for (int i = 0; i < 3; i++) {
        sm_bands[i] = follow(sm_bands[i], a->bands[i], keep);
    }
    for (int i = 0; i < MIC_SPEC; i++) {
        sm_spec[i] = follow(sm_spec[i], a->spec[i], keep);
    }
    switch (k) {
    case FX_EQ:     eq(out, n); break;
    case FX_RIPPLE: ripple(a, out, n, dt, base); break;
    case FX_MIRROR: mirror(out, n, dt); break;
    case FX_CHROMA: chroma(out, n); break;
    case FX_FLOW:   flow(out, n, dt); break;
    default:        memset(out, 0, (size_t)n * 3);
    }
    last_beats = a->beats;
}
