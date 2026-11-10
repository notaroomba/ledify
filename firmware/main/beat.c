// Tempo tracking and beat prediction, see beat.h. Integer only except for the prior table built once.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "beat.h"

#define H_LEN 256                    // onset-strength history: 4.1 s at 62.5 Hz
#define L_MIN 20                     // shortest beat period in frames: 187.5 BPM
#define L_MAX 64                     // longest: 58.6 BPM
#define EST_EVERY 32                 // re-estimate tempo and phase every 0.5 s
#define LOCK_ON 25                   // periodicity (0..100) needed to start predicting...
#define LOCK_OFF 12                  // ...and below which it stops
#define HITS_ON 6                    // of the last 8 predicted beats, this many must have had an onset to start...
#define HITS_OFF 4                   // ...and below this many it stops (also after 3 empty beats in a row)
#define WIN_US 8000                  // an FFT frame stands for the middle of its 16 ms window

static uint16_t h[H_LEN];
static int hpos;
static uint32_t nfr;
static int64_t last_t;
static uint16_t prior[L_MAX + 2];    // Q8 log-Gaussian around 120 BPM, 0.7 octave wide
static int32_t period;               // us, 0 = no tempo yet
static int32_t cand;
static int agree;
static int64_t next_us, fired_us;    // the predicted beat to come and the last one fired
static bool fired_hit;               // an onset landed near the last fired beat
static uint8_t hitbits;              // the last 8 fired beats, 1 = an onset was there
static bool locked;
static uint8_t lock_;
static uint32_t mean_q4;             // running mean of the onset strength, x16

static uint32_t at(int age)          // onset strength age frames ago, 0 = newest
{
    return h[((hpos - 1 - age) % H_LEN + H_LEN) % H_LEN];
}

void beat_reset(void)
{
    memset(h, 0, sizeof h);
    hpos = 0;
    nfr = 0;
    period = cand = agree = 0;
    next_us = fired_us = 0;
    hitbits = 0;
    locked = false;
    lock_ = 0;
    mean_q4 = 0;
    for (int l = 0; l <= L_MAX + 1; l++) {
        float o = l ? log2f(l / 31.25f) / 0.7f : 9;
        prior[l] = (uint16_t)(256 * expf(-0.5f * o * o));
    }
}

// Tempo by autocorrelation of the mean-removed history, weighted by the prior; phase by a comb at that period.
static void estimate(void)
{
    int n = nfr < H_LEN ? (int)nfr : H_LEN;
    int32_t x[H_LEN];
    int64_t sum = 0;
    for (int a = 0; a < n; a++) {
        sum += at(a);
    }
    int32_t m = sum / n;
    int64_t a0 = 0;
    for (int a = 0; a < n; a++) {
        x[a] = (int32_t)at(a) - m;
        a0 += (int64_t)x[a] * x[a];
    }
    if (a0 <= 0) {
        lock_ = 0;
        return;
    }
    int64_t ac[L_MAX + 2];
    int best = 0;
    int64_t bestv = 0;
    for (int l = L_MIN - 1; l <= L_MAX + 1; l++) {
        int64_t s = 0;
        for (int a = 0; a + l < n; a++) {
            s += (int64_t)x[a] * x[a + l];
        }
        ac[l] = s * n / (n - l);                              // unbiased: longer lags have fewer terms
        if (l >= L_MIN && l <= L_MAX && ac[l] > 0 && ac[l] / 256 * prior[l] > bestv) {
            bestv = ac[l] / 256 * prior[l];
            best = l;
        }
    }
    if (!best) {
        lock_ = 0;
        return;
    }
    // Sub-frame period from a parabola through the peak and its neighbours, Q8 frames.
    int64_t y0 = ac[best - 1], y1 = ac[best], y2 = ac[best + 1], den = y0 - 2 * y1 + y2;
    int32_t frac = den < 0 ? (int32_t)((y0 - y2) * 128 / den) : 0;
    frac = frac > 128 ? 128 : frac < -128 ? -128 : frac;
    int32_t lq8 = best * 256 + frac;
    int32_t np = (int32_t)((int64_t)lq8 * BEAT_FRAME_US / 256);
    uint32_t rho = (uint32_t)(y1 * 100 / a0);                 // how periodic: 100 = a perfect click track

    if (!period) {
        period = np;
    } else if (abs(np - period) < period / 16) {             // same tempo: track it slowly
        period = (3 * period + np) / 4;
        agree = 0;
    } else if (cand && abs(np - cand) < cand / 16) {          // a different tempo twice running: switch
        if (++agree >= 2) {
            period = np;
            next_us = 0;
            agree = 0;
        }
    } else {
        cand = np;
        agree = 1;
        rho /= 2;                                             // unsure while the estimate jumps around
    }
    lock_ = rho <= 10 ? 0 : rho >= 60 ? 100 : (rho - 10) * 2;    // noise alone reaches about 20

    // Phase: the offset whose comb of beats, one period apart over the history, collects the most onset strength.
    int32_t pf = (int32_t)((int64_t)period * 256 / BEAT_FRAME_US);   // period in Q8 frames
    int lf = pf >> 8;
    int bestp = 0;
    uint32_t bests = 0;
    for (int p = 0; p < lf; p++) {
        uint32_t s = 0;
        for (int k = 0;; k++) {
            int i = p + (int)(((int64_t)k * pf + 128) >> 8);
            if (i + 1 >= n) {
                break;
            }
            uint32_t v = 2 * at(i) + (i ? at(i - 1) : 0) + at(i + 1);
            s += k < 4 ? 2 * v : v;                           // the last few beats count double
        }
        if (s > bests) {
            bests = s;
            bestp = p;
        }
    }
    int64_t beat = last_t - WIN_US - (int64_t)bestp * BEAT_FRAME_US;
    while (beat <= last_t - period / 2) {
        beat += period;
    }
    if (!next_us) {
        next_us = beat;
        return;
    }
    int64_t err = beat - next_us;                             // pull the running prediction towards it: half way while
    err = ((err % period) + period + period / 2) % period - period / 2;
    next_us += locked ? err / 4 : err / 2;                    // searching, a quarter once the onsets keep it in step
}

// One fired beat checked: shift it into the hit history and decide whether to predict, with some hysteresis.
static void score(bool hit)
{
    hitbits = (uint8_t)(hitbits << 1 | hit);
    int hits = __builtin_popcount(hitbits);
    if (!locked && lock_ >= LOCK_ON && hits >= HITS_ON) {
        locked = true;
    } else if (locked && (lock_ < LOCK_OFF || hits < HITS_OFF || (hitbits & 7) == 0)) {
        locked = false;
    }
}

void beat_odf(uint32_t s, int64_t t)
{
    s = s > 65535 ? 65535 : s;
    h[hpos] = s;
    hpos = (hpos + 1) % H_LEN;
    nfr++;
    last_t = t;
    mean_q4 += ((int32_t)(s << 4) - (int32_t)mean_q4) / 64;
    // A fired beat with nothing there: look at the strength around it once its frames are in.
    if (fired_us && t >= fired_us + 3 * BEAT_FRAME_US) {
        if (!fired_hit) {
            uint32_t mx = 0;
            int a0 = (int)((t - fired_us - 3 * BEAT_FRAME_US) / BEAT_FRAME_US), a1 = a0 + 6;
            for (int a = a0; a <= a1 && a < H_LEN; a++) {
                mx = at(a) > mx ? at(a) : mx;
            }
            fired_hit = mx * 16 > mean_q4 * 2;                // twice the running mean
        }
        score(fired_hit);
        fired_us = 0;
    }
    if (nfr >= H_LEN / 2 && nfr % EST_EVERY == 0) {
        estimate();
    }
}

void beat_onset(int64_t t)
{
    if (!period || !next_us) {
        return;
    }
    int64_t near = next_us;
    if (fired_us && llabs(t - fired_us) < llabs(t - next_us)) {
        near = fired_us;
    }
    int64_t err = t - near;
    if (llabs(err) < period / 5) {
        next_us += err / 3;                                   // phase-locked loop: a third of the phase error...
        period += (int32_t)(err / 16);                        // ...and a little of it into the tempo, so it stops drifting
        if (near == fired_us) {
            fired_hit = true;
        }
    }
}

bool beat_locked(void)
{
    return period && next_us && locked;
}

bool beat_due(int64_t now, int32_t lead)
{
    if (!period || !next_us) {
        return false;
    }
    while (now > next_us + period) {                          // fell behind (a gap in the audio): catch up
        next_us += period;
    }
    if (now + lead + 4000 < next_us) {                         // asked every 8 ms block: fire in the block nearest the time
        return false;
    }
    if (fired_us) {                                           // the previous one was never checked: count it now
        score(fired_hit);
    }
    bool on = beat_locked();
    fired_us = next_us;
    fired_hit = false;
    next_us += period;
    return on;
}

uint16_t beat_bpm(void)
{
    return period ? (uint16_t)((60000000LL + period / 2) / period) : 0;
}

uint8_t beat_lock(void)
{
    return beat_locked() ? lock_ : lock_ / 2;
}
