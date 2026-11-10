// Sound and motion styles that draw the whole strip at once. Every input attacks instantly and falls back with one release
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
// Motion: float per frame (a handful of operations), integer per pixel.
static float bub_x, bub_v;                   // bubble: position -1..1 along the strip and its speed
static float sm_tilt, sm_side;               // tilt: smoothed inputs
static float sm_energy, flash;               // shake: smoothed energy, the flash from the last jolt
static uint8_t heat[FX_MAX_LEDS], spark_hue[FX_MAX_LEDS];
static float spark_acc;
static uint32_t last_kicks, rng = 0x2545F491;
static float liq_x, liq_v;                   // slosh: the liquid's centre -1..1 and its speed
static uint16_t hstep = 10;                  // height: cm per colour step
static int h_band;                           // height: the step showing (with hysteresis)
static float h_rgb[3];                       // height: the colour on the strip, easing to the step's
// Fused styles (sound and motion together).
static uint8_t user_lut[FX_PAL][3];          // the user's gradient, sampled
static bool use_user;
static uint8_t sin_t[256];                   // 128 + 127 sin, one turn over 256
static fx_t last_k = FX_N;                   // the style drawn last frame: trails are cleared on a change
static uint8_t trail[FX_MAX_LEDS][3];        // bounce, comet: what was drawn, fading
static float amb;                            // loudness 0..1 on a slow (1.2 s) follower: the room's mood, not the beat
static float au_p[3], au_drift, surge;       // aurora: phases of its three layers (turns), smoothed tilt, the beat swell
static uint8_t fire_h[FX_MAX_LEDS];          // fire: heat along the flame, 0 at the base
static uint8_t fire_snap[FX_MAX_LEDS][3];    // fire: the last frame before the flame turned round, faded out
static int fire_dir = 2, fire_xf;            // fire: 1 base at pixel 0, -1 base at the far end, 0 both ends; crossfade /256
static uint32_t fire_acc;                    // fire: ms not yet simulated
static struct { float x, v; } balls[6];      // bounce: position 0..1 along the strip (0 = pixel 0), strips per second
static float com_ph, com_prev[2], com_flash; // comet: phase in laps, last frame's positions (px), the beat flash

void fx_set_hstep(uint16_t cm)
{
    hstep = cm < 1 ? 1 : cm > 500 ? 500 : cm;
}

void fx_set_palette(const uint8_t lut[FX_PAL][3], bool on)
{
    memcpy(user_lut, lut, sizeof user_lut);
    use_user = on;
}

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

static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static float absf(float x) { return x < 0 ? -x : x; }
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

// A damped spring towards target, stepped at no more than 4 ms so a slow frame doesn't blow it up.
// w: stiffness as an angular frequency (rad/s), z: damping ratio (1 = glides in without overshoot).
static void spring(float *x, float *v, float target, float force, float w, float z, uint32_t dt_ms)
{
    for (float left = dt_ms / 1000.0f; left > 0; left -= 0.004f) {
        float h = left < 0.004f ? left : 0.004f;
        *v += (w * w * (target - *x) - 2 * z * w * *v + force) * h;
        *x += *v * h;
        if (*x > 1 || *x < -1) {                                  // the ends of the strip are walls: bounce softly
            *x = *x > 1 ? 1 : -1;
            *v *= -0.3f;
        }
    }
}

// One-pole smoothing with time constant tau seconds.
static float ease_to(float cur, float to, float tau, uint32_t dt_ms)
{
    float h = dt_ms / 1000.0f;
    return cur + (to - cur) * (h / (tau + h));
}

static void put(uint8_t *p, uint8_t r, uint8_t g, uint8_t b)
{
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

// Bubble: a spirit level. A bubble glides to the raised end (full travel at about 30 degrees) on a critically damped
// spring, so it never jitters or overshoots; green when level, warming to red as it nears an end. Two faint marks
// show the middle.
static void bubble(const motion_snap_t *m, uint8_t (*out)[3], int n, uint32_t dt, const uint8_t base[3])
{
    spring(&bub_x, &bub_v, clampf(m->tilt / 0.5f, -1, 1), 0, 14, 1, dt);
    int w = n / 12 > 2 ? n / 12 : 2;                             // bubble half-width, px
    int32_t span = ((n - 1) << 7) - (w << 8);                    // half the travel, 1/256 px
    span = span < 0 ? 0 : span;
    int32_t c = ((n - 1) << 7) + (int32_t)(bub_x * span);        // centre, 1/256 px
    uint8_t col[3], bg[3];
    hsv((uint8_t)(85 - absf(bub_x) * 85), 255, col);
    for (int ch = 0; ch < 3; ch++) {
        bg[ch] = base[ch] / 12;
    }
    int mark = w + 1;
    for (int i = 0; i < n; i++) {
        memcpy(out[i], bg, 3);
        if (n >= 20 && (i == (n - 1) / 2 - mark || i == n / 2 + mark)) {
            put(out[i], 40, 40, 40);
        }
        int32_t d = (i << 8) - c;
        d = d < 0 ? -d : d;
        int32_t wq = w << 8;
        if (d < wq) {
            uint32_t f = 255 - (uint32_t)((int64_t)d * d * 255 / ((int64_t)wq * wq));   // round-topped
            uint8_t px[3];
            for (int ch = 0; ch < 3; ch++) {
                px[ch] = col[ch] * f / 255;
            }
            add(out[i], px);
        }
    }
}

// Tilt: the whole strip takes the hue of the way it leans (forward, back, left, right go round the colour wheel),
// more of it the further it leans (full at about 30 degrees), from your colour when level; the lower end glows brighter.
static void tilt(const motion_snap_t *m, uint8_t (*out)[3], int n, uint32_t dt, const uint8_t base[3])
{
    sm_tilt = ease_to(sm_tilt, m->tilt, 0.08f, dt);
    sm_side = ease_to(sm_side, m->side, 0.08f, dt);
    float mag = sqrtf(sm_tilt * sm_tilt + sm_side * sm_side) / 0.5f;
    mag = mag > 1 ? 1 : mag;
    float ang = atan2f(sm_side, sm_tilt);                        // -pi..pi
    uint8_t hc[3], col[3];
    hsv((uint8_t)((ang + 3.14159f) * 255 / 6.28318f), 255, hc);
    uint32_t mq = (uint32_t)(mag * 256);
    for (int ch = 0; ch < 3; ch++) {
        col[ch] = (base[ch] * (256 - mq) + hc[ch] * mq) >> 8;
    }
    int32_t slope = (int32_t)(clampf(sm_tilt / 0.5f, -1, 1) * 120);   // brightness change end to middle, /256
    for (int i = 0; i < n; i++) {
        int32_t p = n > 1 ? (2 * i - (n - 1)) * 256 / (n - 1) : 0;    // -256..256 along the strip
        int32_t f = 256 - slope * p / 256 - (slope < 0 ? -slope : slope) / 2;   // the raised end dims, the low end stays full
        f = f < 0 ? 0 : f > 256 ? 256 : f;
        for (int ch = 0; ch < 3; ch++) {
            out[i][ch] = col[ch] * f >> 8;
        }
    }
}

// Shake: sparks fly while it moves, more the harder it is shaken, in colours that turn with the heading; a sharp
// knock flashes the whole strip. Sparks fade with the same release as the sound styles ("react").
static void shake(const motion_snap_t *m, uint8_t (*out)[3], int n, uint32_t dt, const uint8_t base[3])
{
    float e = m->energy > 1.5f ? 1.5f : m->energy;
    sm_energy = e > sm_energy ? e : e + (sm_energy - e) * keep / 65536.0f;
    if (m->kicks != last_kicks) {
        flash = 1;
    } else {
        flash *= keep / 65536.0f;
    }
    last_kicks = m->kicks;
    uint32_t fade = keep;                                         // per frame, Q16
    for (int i = 0; i < n; i++) {
        heat[i] = (uint8_t)((heat[i] * fade) >> 16);
    }
    float busy = sm_energy - 0.03f;                               // a still board's last trickle makes no sparks
    spark_acc += busy > 0 ? busy * n * dt / 1000.0f * 2.5f : 0;   // a brisk shake lights the strip 2.5 times a second
    int hue0 = (int)(m->heading * 256 / 360);
    for (int k = 0; spark_acc >= 1 && k < n; k++, spark_acc -= 1) {
        int i = rnd() % n;
        heat[i] = 255;
        spark_hue[i] = (uint8_t)(hue0 + (int)(rnd() % 48) - 24);
    }
    if (spark_acc > 1) {
        spark_acc = 0;
    }
    uint32_t glow = (uint32_t)(clampf(sm_energy, 0, 1) * 40);    // your colour underneath, with the motion
    uint8_t fl = (uint8_t)(flash * 160);
    for (int i = 0; i < n; i++) {
        put(out[i], base[0] * glow / 255, base[1] * glow / 255, base[2] * glow / 255);
        if (heat[i]) {
            uint8_t c[3];
            hsv(spark_hue[i], gam(heat[i]), c);
            add(out[i], c);
        }
        if (fl) {
            uint8_t w[3] = {fl, fl, fl};
            add(out[i], w);
        }
    }
}

// Spin: a rainbow that stays put in the room: turn the board and the colours slide along the strip the other way,
// one full turn for one full rainbow. Brighter the faster it turns.
static void spin(const motion_snap_t *m, uint8_t (*out)[3], int n)
{
    uint32_t off = (uint32_t)(m->heading * 256 / 360);
    float sp = absf(m->yaw_rate) / 180;
    uint8_t v = (uint8_t)(150 + 105 * (sp > 1 ? 1 : sp));
    for (int i = 0; i < n; i++) {
        hsv((uint8_t)(i * 256 / n - off), v, out[i]);
    }
}

// Slosh: half the strip is a liquid in your colour. It runs to the lower end, sloshes when the board is pushed along
// the strip (a ~1 Hz wave, lightly damped, so it rocks a few times and settles), and froths white at its edges while
// it moves.
static void slosh(const motion_snap_t *m, uint8_t (*out)[3], int n, uint32_t dt, const uint8_t base[3])
{
    float target = -clampf(m->tilt / 0.35f, -1, 1);              // away from the raised end
    spring(&liq_x, &liq_v, target, -m->lin_along * 36, 6, 0.25f, dt);   // pushed along +: it lags towards -
    int len = n / 2 > 1 ? n / 2 : 1;
    int32_t c = ((n - 1) << 7) + (int32_t)(liq_x * ((n - len) << 7));   // centre, 1/256 px
    int32_t lo = c - (len << 7), hi = c + (len << 7);
    uint32_t froth = (uint32_t)(clampf(absf(liq_v) * 0.6f, 0, 1) * 200);
    int edge = n / 30 > 2 ? n / 30 : 2;
    for (int i = 0; i < n; i++) {
        int32_t p = i << 8;
        int32_t in = p - lo < hi - p ? p - lo : hi - p;         // depth inside the liquid, 1/256 px
        if (in <= -256) {
            put(out[i], 0, 0, 0);
            continue;
        }
        uint32_t a = in >= 0 ? 255 : (uint32_t)(255 + in);       // a soft one-pixel surface
        uint32_t f = in < (edge << 8) ? froth * ((edge << 8) - (in > 0 ? in : 0)) / (edge << 8) : 0;
        for (int ch = 0; ch < 3; ch++) {
            uint32_t v = (base[ch] * (255 - f) + 255 * f) / 255;
            out[i][ch] = v * a / 255;
        }
    }
}

// Hue of an rgb colour, 0..255 (0 for greys).
static uint8_t hue_of(const uint8_t *c)
{
    int r = c[0], g = c[1], b = c[2], mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int d = mx - mn;
    if (!d) {
        return 0;
    }
    int h = mx == r ? 43 * (g - b) / d : mx == g ? 85 + 43 * (b - r) / d : 171 + 43 * (r - g) / d;
    return (uint8_t)(h < 0 ? h + 256 : h);
}

// Height: the whole strip changes colour every hstep cm the board is raised or lowered from where height was
// zeroed: your colour at 0, then an eighth of the way round the colour wheel per step (up one way, down the other).
// A step only changes once the board is a fifth of a step (and 3 cm) past the line, so hovering on it doesn't flicker; the
// colour eases over about 120 ms. A faint white dot travels along the strip as the board climbs through the step.
static void height(const motion_snap_t *m, uint8_t (*out)[3], int n, uint32_t dt, const uint8_t base[3])
{
    float pos = m->height * 100.0f / hstep;                       // in steps
    float hy = 3.0f / hstep;                                      // hysteresis: 3 cm (the barometer's jitter), at least
    hy = hy < 0.2f ? 0.2f : hy > 0.45f ? 0.45f : hy;              // a fifth of a step, at most just under half
    if (pos >= h_band + 1 + hy || pos < h_band - hy) {
        h_band = (int)floorf(pos + (pos > h_band ? -hy : hy));
    }
    uint8_t want[3];
    if (h_band == 0) {
        memcpy(want, base, 3);
    } else {
        hsv((uint8_t)(hue_of(base) + h_band * 32), 255, want);
    }
    for (int ch = 0; ch < 3; ch++) {
        h_rgb[ch] = ease_to(h_rgb[ch], want[ch], 0.12f, dt);
    }
    float frac = pos - floorf(pos);
    int dot = n > 1 ? (int)(frac * (n - 1) + 0.5f) : -1;
    for (int i = 0; i < n; i++) {
        put(out[i], (uint8_t)h_rgb[0], (uint8_t)h_rgb[1], (uint8_t)h_rgb[2]);
        if (n >= 10 && (i == dot || i == dot + 1)) {
            for (int ch = 0; ch < 3; ch++) {
                out[i][ch] = (out[i][ch] + 255) / 2;
            }
        }
    }
}


// ---- Fused styles: the mic and the IMU at once ----

static uint8_t sin8(uint32_t x) { return sin_t[x & 255]; }

// Colour at pos 0..255 of a list of nst stops, evenly spaced.
static void lut_at(const uint8_t (*lut)[3], int nst, uint8_t pos, uint8_t *out)
{
    uint32_t x = (uint32_t)pos * (nst - 1);                       // in stops, 8.8
    int k = x >> 8, k1 = k + 1 < nst ? k + 1 : k;
    uint32_t f = x & 255;
    for (int ch = 0; ch < 3; ch++) {
        out[ch] = (lut[k][ch] * (256 - f) + lut[k1][ch] * f) >> 8;
    }
}

// The style's own colours (own, or a rainbow when NULL), or the user's gradient when they asked for it.
static void pick(const uint8_t (*own)[3], int nown, uint8_t pos, uint8_t *out)
{
    if (use_user) {
        lut_at(user_lut, FX_PAL, pos, out);
    } else if (own) {
        lut_at(own, nown, pos, out);
    } else {
        hsv(pos, 255, out);
    }
}

static void scale3(uint8_t *p, const uint8_t *c, uint32_t v)       // p = c x v/255
{
    for (int ch = 0; ch < 3; ch++) {
        p[ch] = c[ch] * v / 255;
    }
}

static bool have_imu(const motion_snap_t *m)
{
    return m->up[0] != 0 || m->up[1] != 0 || m->up[2] != 0;
}

static float wrapf(float x) { return x - floorf(x); }

// Aurora: slow curtains of light in greens, teals and violets. Two waves running opposite ways along the strip cross into
// moving folds, a third, slower one swells them; the room's loudness (slow) brightens and quickens them, each beat sends a
// soft swell through, and they drift towards whichever end is lower. Quiet room: a calm, dim sky.
static const uint8_t AURORA[5][3] = {{0, 255, 70}, {0, 220, 150}, {0, 130, 255}, {110, 40, 255}, {210, 0, 170}};
static void aurora(const motion_snap_t *m, uint8_t (*out)[3], int n, uint32_t dt, bool beat)
{
    float h = dt / 1000.0f;
    au_drift = ease_to(au_drift, clampf(m->tilt / 0.5f, -1, 1), 0.4f, dt);
    float sp = 0.04f + 0.18f * amb;                               // turns per second of the folds
    au_p[0] = wrapf(au_p[0] + (sp + 0.35f * au_drift) * h);       // + raised (drift > 0): the folds travel to pixel 0
    au_p[1] = wrapf(au_p[1] + (sp * 1.6f - 0.2f * au_drift) * h);
    au_p[2] = wrapf(au_p[2] + (0.015f + 0.05f * amb) * h);
    if (beat) {
        surge = 0.4f + 0.6f * (sm_loud >> 8) / 255.0f;
    } else {
        surge *= expf(-h / 0.35f);
    }
    uint32_t lev = (uint32_t)(130 + 95 * amb + 50 * surge);       // overall brightness 0..255
    lev = lev > 255 ? 255 : lev;
    uint32_t p0 = (uint32_t)(au_p[0] * 256), p1 = (uint32_t)(au_p[1] * 256), p2 = (uint32_t)(au_p[2] * 256);
    uint32_t tint = (sm_bands[2] >> 8) / 4;                       // treble leans the colours to the violet end
    uint32_t spark = (uint32_t)(surge * 90);
    uint32_t floor_v = 18 + (uint32_t)(30 * amb);                 // the sky between the curtains never goes quite dark
    for (int i = 0; i < n; i++) {
        uint32_t x = (uint32_t)i * 256 / (uint32_t)n;             // 0..255 along the strip
        uint32_t f = ((uint32_t)sin8(x * 3 + p0) + sin8(x * 5 - p1)) / 2;          // the folds, 0..255
        uint32_t env = 90 + (sin8(x + p2 * 2) * 165 >> 8);                         // and their swell
        uint32_t v = f > 70 ? (f - 70) * 255 / 185 : 0;                            // folds stand out from the sky
        v = gam(v) * env >> 8;
        v = floor_v + v * (255 - floor_v) / 255;
        uint32_t pos = (sin8(x * 2 - p2 * 3) * 3 >> 2) + tint;
        uint8_t c[3];
        pick(use_user ? NULL : AURORA, 5, pos > 255 ? 255 : pos, c);
        scale3(out[i], c, v * lev / 255);
        if (spark && f > 215) {                                   // a beat makes the crests shimmer
            uint8_t w = (uint8_t)(spark * (f - 215) / 40);
            uint8_t wc[3] = {w, w, w};
            add(out[i], wc);
        }
    }
}

// Heat 0..255 as a flame colour: black, red, orange, yellow, white; or along the user's gradient, dark when cool.
static void heat_rgb(uint8_t h, uint8_t *p)
{
    if (use_user) {
        uint8_t c[3];
        lut_at(user_lut, FX_PAL, h, c);
        scale3(p, c, h < 85 ? h * 3 : 255);
        return;
    }
    uint32_t t = (uint32_t)h * 191 / 255, ramp = (t & 63) << 2;
    if (t >= 128) {
        put(p, 255, 255, ramp);
    } else if (t >= 64) {
        put(p, 255, ramp, 0);
    } else {
        put(p, ramp, 0, 0);
    }
}

static void fire_map(uint8_t (*out)[3], int n, int dir)
{
    for (int i = 0; i < n; i++) {
        int y = dir > 0 ? i : dir < 0 ? n - 1 - i : (i < n - 1 - i ? i : n - 1 - i);
        heat_rgb(fire_h[y], out[i]);
    }
}

// Fire: flames that always rise. The IMU says which end is down and the fire burns from there (from both ends when
// the strip lies level), turning round with a quick crossfade when you flip it. Sound is its fuel: louder burns taller
// and wilder, a beat throws up a flare, and shaking the board stokes it.
static void fire(const motion_snap_t *m, uint8_t (*out)[3], int n, uint32_t dt, uint32_t newb)
{
    int dir = fire_dir;
    if (!have_imu(m)) {
        dir = 1;
    } else if (m->tilt > 0.3f) {
        dir = 1;                                                  // + end raised: the base is pixel 0
    } else if (m->tilt < -0.3f) {
        dir = -1;
    } else if (m->tilt > -0.15f && m->tilt < 0.15f) {
        dir = 0;
    }
    if (fire_dir == 2) {
        fire_dir = dir;
    } else if (dir != fire_dir) {
        fire_map(fire_snap, n, fire_dir);
        fire_xf = 256;
        fire_dir = dir;
    }
    int len = dir ? n : (n + 1) / 2;
    float fuel = clampf((sm_loud >> 8) / 255.0f * 0.8f + clampf(m->energy, 0, 1) * 0.6f, 0, 1);
    uint32_t cool = (uint32_t)(100 - 50 * fuel), sparking = (uint32_t)(50 + 130 * fuel);
    if (newb) {                                                   // a flare from the base
        for (int y = 0; y < len / 12 + 2 && y < len; y++) {
            fire_h[y] = 255;
        }
    }
    for (fire_acc += dt; fire_acc >= 16; fire_acc -= 16) {        // the flame runs at 62.5 steps a second whatever the frame rate
        for (int y = 0; y < len; y++) {
            uint32_t c = rnd() % (cool * 10 / (uint32_t)len + 2);
            fire_h[y] = fire_h[y] > c ? fire_h[y] - c : 0;
        }
        for (int y = len - 1; y >= 2; y--) {
            fire_h[y] = (fire_h[y - 1] + 2 * fire_h[y - 2]) / 3;
        }
        if (len >= 2) {
            fire_h[1] = (fire_h[0] + fire_h[1] * 2) / 3;
        }
        if (rnd() % 255 < sparking) {
            int y = rnd() % (len / 10 + 2 < len ? len / 10 + 2 : len);
            uint32_t v = fire_h[y] + 160 + rnd() % 96;
            fire_h[y] = v > 255 ? 255 : v;
        }
    }
    fire_map(out, n, dir);
    if (fire_xf > 0) {
        for (int i = 0; i < n; i++) {
            for (int ch = 0; ch < 3; ch++) {
                out[i][ch] = (out[i][ch] * (256 - fire_xf) + fire_snap[i][ch] * fire_xf) >> 8;
            }
        }
        fire_xf -= (int)(dt * 256 / 300);
    }
}

// A glowing streak from pixel a to b (fractional pixels) with soft ends of r pixels, drawn over the trail (brightest wins),
// so a fast mover leaves no gaps between frames.
static void streak(int n, float a, float b, float r, const uint8_t *c)
{
    float lo = a < b ? a : b, hi = a < b ? b : a;
    int i0 = (int)floorf(lo - r), i1 = (int)ceilf(hi + r);
    i0 = i0 < 0 ? 0 : i0;
    i1 = i1 > n - 1 ? n - 1 : i1;
    for (int i = i0; i <= i1; i++) {
        float d = i < lo ? lo - i : i > hi ? i - hi : 0;
        if (d >= r) {
            continue;
        }
        uint32_t f = (uint32_t)(255 * (1 - d / r));
        f = gam(f);
        for (int ch = 0; ch < 3; ch++) {
            uint8_t v = c[ch] * f / 255;
            trail[i][ch] = v > trail[i][ch] ? v : trail[i][ch];
        }
    }
}

static void fade_trail(int n, uint32_t dt, float tau_ms)
{
    uint32_t k = (uint32_t)(256 * expf(-(float)dt / tau_ms));
    for (int i = 0; i < n; i++) {
        for (int ch = 0; ch < 3; ch++) {
            trail[i][ch] = trail[i][ch] * k >> 8;
        }
    }
}

// Bounce: glowing balls under real gravity. They fall to whichever end is lower (and roll about when the strip lies
// flat), bounce off the ends, and every beat kicks them back up, higher the louder it is; a knock on the board throws
// them all up and a shove along the strip sloshes them. Turning the board turns their colours.
static void bounce(const motion_snap_t *m, uint8_t (*out)[3], int n, uint32_t dt, bool beat)
{
    int k = n < 30 ? 2 : n < 90 ? 3 : 5;
    float g = have_imu(m) ? -9.81f * (m->tilt + m->lin_along) / 2.0f : -3.4f;   // strips/s^2 on a 2 m strip; + raised: fall to 0
    bool knock = m->kicks != last_kicks;
    float loud = (sm_loud >> 8) / 255.0f;
    float from[6];
    for (int b = 0; b < k; b++) {
        from[b] = balls[b].x * (n - 1);
        if (beat || knock) {
            float hgt = knock ? 0.9f : (0.15f + 0.6f * loud) * (0.55f + 0.45f * (b + 1) / k);
            float up = g < 0 ? 1 : -1;                            // against gravity
            float v = fabsf(g) > 0.6f ? sqrtf(2 * fabsf(g) * hgt) : 0.5f + 1.2f * hgt;
            if (fabsf(g) <= 0.6f) {
                up = rnd() & 1 ? 1 : -1;                          // flat: a nudge either way
            }
            balls[b].v += up * v;
        }
        for (float left = dt / 1000.0f; left > 0; left -= 0.004f) {
            float h = left < 0.004f ? left : 0.004f;
            balls[b].v += g * h;
            balls[b].v *= 1 - 0.35f * h;                          // a little air
            balls[b].x += balls[b].v * h;
            if (balls[b].x < 0) {
                balls[b].x = -balls[b].x * 0.5f;
                balls[b].v = -balls[b].v * 0.72f;
            } else if (balls[b].x > 1) {
                balls[b].x = 1 - (balls[b].x - 1) * 0.5f;
                balls[b].v = -balls[b].v * 0.72f;
            }
        }
    }
    fade_trail(n, dt, 110);
    float r = n / 40.0f > 1.5f ? n / 40.0f : 1.5f;
    uint8_t rot = (uint8_t)(m->heading * 256 / 360);
    for (int b = 0; b < k; b++) {
        uint8_t c[3];
        pick(NULL, 0, (uint8_t)(use_user ? b * 255 / (k - 1) : rot + b * 256 / k), c);
        float sp = fabsf(balls[b].v) / 2;
        scale3(c, c, (uint32_t)(190 + 65 * (sp > 1 ? 1 : sp)));
        streak(n, from[b], balls[b].x * (n - 1), r, c);
    }
    memcpy(out, trail, (size_t)n * 3);
}

// Comet: two comets swing along the strip from opposite ends and fly through each other in the middle on every beat,
// in time with the tempo the board has locked on to (one swing for two beats), flashing as they cross. Before it has a
// tempo they glide at a pace set by the loudness, and a beat only swells their heads. Their colours sit opposite on the wheel and turn as you turn the board.
static void comet(const mic_snap_t *a, const motion_snap_t *m, uint8_t (*out)[3], int n, uint32_t dt, bool beat)
{
    float h = dt / 1000.0f;
    bool locked = a->bpm && a->lock >= 30;
    float rate = locked ? a->bpm / 120.0f : 0.12f + 0.4f * amb;  // laps per second
    com_ph = wrapf(com_ph + rate * h);
    if (beat) {
        if (locked) {                                             // pull the crossing (phase 1/4 or 3/4) onto the beat
            float e = (com_ph - 0.25f) * 2;
            e = (e - floorf(e + 0.5f)) / 2;
            com_ph = wrapf(com_ph - e * 0.6f);
        }
        com_flash = 0.5f + 0.5f * (sm_loud >> 8) / 255.0f;
    } else {
        com_flash *= keep / 65536.0f;
    }
    fade_trail(n, dt, 180);
    float s = 0.5f - 0.5f * cosf(6.2831853f * com_ph), pos[2] = {s * (n - 1), (1 - s) * (n - 1)};
    float r = (n / 50.0f > 1.2f ? n / 50.0f : 1.2f) * (1 + com_flash);
    uint8_t hue = (uint8_t)(m->heading * 256 / 360);
    for (int c = 0; c < 2; c++) {
        uint8_t col[3];
        pick(NULL, 0, (uint8_t)(use_user ? c * 255 : hue + c * 128), col);
        float from = com_prev[c] >= 0 && com_prev[c] <= n - 1 ? com_prev[c] : pos[c];
        streak(n, from, pos[c], r, col);
        com_prev[c] = pos[c];
    }
    memcpy(out, trail, (size_t)n * 3);
    uint32_t fl = (uint32_t)(com_flash * 200);                    // the crossing flash: white at the middle, a quarter wide
    if (locked && fl > 8) {
        int w = n / 8 > 1 ? n / 8 : 1;
        for (int i = n / 2 - w; i <= n / 2 + w; i++) {
            if (i < 0 || i >= n) {
                continue;
            }
            int d = i - n / 2;
            uint32_t v = fl * (uint32_t)(w - (d < 0 ? -d : d)) / (uint32_t)w;
            v = gam(v);
            uint8_t wc[3] = {(uint8_t)v, (uint8_t)v, (uint8_t)v};
            add(out[i], wc);
        }
    }
}

void fx_render(fx_t k, const mic_snap_t *a, const motion_snap_t *m, uint8_t (*out)[3], int n, uint32_t dt,
               const uint8_t base[3])
{
    if (n <= 0) {
        return;
    }
    n = n > FX_MAX_LEDS ? FX_MAX_LEDS : n;
    dt = dt > 200 ? 200 : dt;                                     // after a pause, don't jump
    if (!primed) {
        last_beats = a->beats;
        last_kicks = m->kicks;
        for (int i = 0; i < 256; i++) {
            sin_t[i] = (uint8_t)(128 + 127 * sinf(i * 6.2831853f / 256));
        }
        for (int b = 0; b < 6; b++) {
            balls[b].x = 0.5f;
        }
        com_prev[0] = com_prev[1] = -1;
        primed = true;
    }
    if (k != last_k) {                                            // a fresh start for the styles that keep a picture
        memset(trail, 0, sizeof trail);
        com_prev[0] = com_prev[1] = -1;
        fire_dir = 2;
        fire_xf = 0;
        last_k = k;
    }
    uint32_t newb = a->beats - last_beats;
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
    amb = ease_to(amb, a->loud / 255.0f, 1.2f, dt);
    switch (k) {
    case FX_EQ:     eq(out, n); break;
    case FX_RIPPLE: ripple(a, out, n, dt, base); break;
    case FX_MIRROR: mirror(out, n, dt); break;
    case FX_CHROMA: chroma(out, n); break;
    case FX_FLOW:   flow(out, n, dt); break;
    case FX_BUBBLE: bubble(m, out, n, dt, base); break;
    case FX_TILT:   tilt(m, out, n, dt, base); break;
    case FX_SHAKE:  shake(m, out, n, dt, base); break;
    case FX_SPIN:   spin(m, out, n); break;
    case FX_SLOSH:  slosh(m, out, n, dt, base); break;
    case FX_HEIGHT: height(m, out, n, dt, base); break;
    case FX_AURORA: aurora(m, out, n, dt, newb != 0); break;
    case FX_FIRE:   fire(m, out, n, dt, newb); break;
    case FX_BOUNCE: bounce(m, out, n, dt, newb != 0); break;
    case FX_COMET:  comet(a, m, out, n, dt, newb != 0); break;
    default:        memset(out, 0, (size_t)n * 3);
    }
    last_beats = a->beats;
    last_kicks = m->kicks;
}
