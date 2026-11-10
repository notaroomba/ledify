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

void fx_set_hstep(uint16_t cm)
{
    hstep = cm < 1 ? 1 : cm > 500 ? 500 : cm;
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
    case FX_BUBBLE: bubble(m, out, n, dt, base); break;
    case FX_TILT:   tilt(m, out, n, dt, base); break;
    case FX_SHAKE:  shake(m, out, n, dt, base); break;
    case FX_SPIN:   spin(m, out, n); break;
    case FX_SLOSH:  slosh(m, out, n, dt, base); break;
    case FX_HEIGHT: height(m, out, n, dt, base); break;
    default:        memset(out, 0, (size_t)n * 3);
    }
    last_beats = a->beats;
}
