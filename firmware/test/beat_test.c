// Host check for main/beat.c:  cc -O2 -I../main beat_test.c ../main/beat.c -lm && ./a.out
// A pretend mic task: 8 ms blocks, an onset-strength frame every 16 ms with a spike on each beat (plus noise and
// some missing beats), the fast detector's onsets with jitter, and beat_due() asked every block.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "beat.h"

#define MISS_MAX_EXPECTED 4   // beat.c falls back after 3 empty beats

static unsigned rnd = 1;
static int noise(int n) { rnd = rnd * 1103515245 + 12345; return (rnd >> 16) % (n + 1); }

// Runs `secs` of music at bpm starting at t0; returns the mean |error| in us of predicted beats after `settle` s.
static double run(double bpm, double secs, int64_t *t0, double settle, int beats_present, int *fired, int *locked_end)
{
    int64_t period = (int64_t)(60e6 / bpm), first = *t0 + 300000;
    double err = 0;
    int n = 0;
    *fired = 0;
    for (int64_t t = *t0 + 8000; t < *t0 + (int64_t)(secs * 1e6); t += 8000) {
        int64_t k = (t - first + period / 2) / period, beat_t = first + k * period;   // nearest true beat
        bool here = beats_present && k >= 0 && (k % 16) != 7;                       // one beat in 16 is silent
        if (here && t - 8000 < beat_t && t >= beat_t) {
            beat_onset(t - 4000 + noise(4000) - 2000);                                // as mic.c: the block's middle
        }
        if ((t / 8000) % 2 == 0) {
            int64_t mid = t - 8000;                                                    // window centre
            uint32_t s = 20 + noise(40);
            if (here && llabs(mid - beat_t) < 8000) {
                s += 600;
            } else if (here && llabs(mid - (beat_t + period / 2)) < 8000) {
                s += 150;                                                              // off-beat hats
            }
            beat_odf(s, t);
        }
        if (beat_due(t, 12000)) {
            (*fired)++;
            int64_t target = t + 12000;                                                // when the LEDs light
            int64_t kk = (target - first + period / 2) / period, e = target - (first + kk * period);
            if (t - *t0 > settle * 1e6) {
                err += llabs(e);
                n++;
            }
        }
    }
    *t0 += (int64_t)(secs * 1e6);
    *locked_end = beat_locked();
    return n ? err / n : -1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    beat_reset();
    int64_t t = 0;
    int fired, locked;
    double e = run(128, 12, &t, 6, 1, &fired, &locked);
    printf("128 BPM: tempo %u, lock %u, %d beats fired, mean error %.1f ms after 6 s\n", beat_bpm(), beat_lock(), fired, e / 1000);
    assert(abs(beat_bpm() - 128) <= 2 && locked && e >= 0 && e < 12000);

    e = run(100, 14, &t, 8, 1, &fired, &locked);
    printf("then 100 BPM: tempo %u, lock %u, locked %d, %d fired, mean error %.1f ms after 8 s\n", beat_bpm(), beat_lock(), locked, fired, e / 1000);
    assert(abs(beat_bpm() - 100) <= 2 && locked && e < 12000);

    e = run(100, 5, &t, 0, 0, &fired, &locked);
    printf("silence: %d beats predicted while locked, locked at the end: %d\n", fired, locked);
    assert(!locked && fired <= MISS_MAX_EXPECTED);

    beat_reset();
    t = 0;
    for (int64_t u = 8000; u < 10000000; u += 8000) {           // noise only: never locks
        if ((u / 8000) % 2 == 0) {
            beat_odf(20 + noise(200), u);
        }
        if (noise(30) == 0) {
            beat_onset(u);
        }
        if (beat_due(u, 12000)) {
            printf("noise: fired at %.2f s, tempo %u, lock %u\n", u / 1e6, beat_bpm(), beat_lock());
            assert(!"predicted a beat in noise");
        }
    }
    printf("noise: lock %u, never fired\n", beat_lock());
    puts("beat ok");
    return 0;
}
