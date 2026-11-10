// Host check for main/fx.c:  cc -O2 -I../main fx_test.c ../main/fx.c -lm && ./a.out
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "fx.h"

#define N 120
static uint8_t px[N][3];
static const uint8_t white[3] = {255, 255, 255};

static int lum(int i) { return px[i][0] + px[i][1] + px[i][2]; }
static int lit_count(void) { int c = 0; for (int i = 0; i < N; i++) c += lum(i) > 0; return c; }
static motion_snap_t mo;
// Brightness-weighted centre of the strip, in pixels.
static float centroid(void) { float s = 0, w = 0; for (int i = 0; i < N; i++) { s += i * lum(i); w += lum(i); } return w ? s / w : -1; }
static int brightest(void) { int b = 0; for (int i = 1; i < N; i++) if (lum(i) > lum(b)) b = i; return b; }
static void run(fx_t k, mic_snap_t *a, int frames) { for (int f = 0; f < frames; f++) fx_render(k, a, &mo, px, N, 16, white); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    mic_snap_t a;
    memset(&a, 0, sizeof a);
    fx_set_react(60);

    // eq: only the zone of the loud band lights, and it lights on the very first frame.
    a.spec[5] = 255;
    fx_render(FX_EQ, &a, &mo, px, N, 16, white);
    for (int i = 0; i < N; i++) {
        int k = i * 16 / N;
        assert(k == 5 ? lum(i) > 200 : lum(i) == 0);
    }
    // release: react 60 falls to under half within ~150 ms, react 1 still holds most of it.
    a.spec[5] = 0;
    for (int f = 0; f < 10; f++) fx_render(FX_EQ, &a, &mo, px, N, 16, white);   // 160 ms
    int fast = lum(5 * N / 16 + 1);
    fx_set_react(1);
    a.spec[5] = 255; fx_render(FX_EQ, &a, &mo, px, N, 16, white); a.spec[5] = 0;
    for (int f = 0; f < 10; f++) fx_render(FX_EQ, &a, &mo, px, N, 16, white);
    int slow = lum(5 * N / 16 + 1);
    printf("eq after 160 ms of silence: react 60 -> %d, react 1 -> %d (of 255)\n", fast, slow);
    assert(fast < 60 && slow > 150);
    fx_set_react(100);
    for (int f = 0; f < 60; f++) fx_render(FX_EQ, &a, &mo, px, N, 16, white);
    assert(lit_count() == 0);

    // mirror: loudness 128 lights about half of each half, symmetric around the centre.
    a.loud = 128;
    fx_render(FX_MIRROR, &a, &mo, px, N, 16, white);
    int c = lit_count();
    printf("mirror at loud 128: %d of %d lit\n", c, N);
    assert(c >= N / 2 - 4 && c <= N / 2 + 4);
    for (int i = 0; i < N; i++) assert(lum(i) == lum(N - 1 - i));
    assert(lum(N / 2) > 0 && lum(0) == 0);
    // the peak dot stays after the sound stops
    a.loud = 0;
    for (int f = 0; f < 10; f++) fx_render(FX_MIRROR, &a, &mo, px, N, 16, white);
    assert(lit_count() == 2);

    // ripple: a beat starts a ring at the centre on the next frame, and it travels outward.
    fx_set_react(60);
    a.loud = 200; a.bands[0] = 255;
    fx_render(FX_RIPPLE, &a, &mo, px, N, 16, white);              // primes the beat counter
    a.beats++;
    fx_render(FX_RIPPLE, &a, &mo, px, N, 16, white);
    int far0 = 0;
    for (int i = N / 2; i < N; i++) if (px[i][0] > 100) far0 = i;
    for (int f = 0; f < 10; f++) fx_render(FX_RIPPLE, &a, &mo, px, N, 16, white);
    int far1 = 0;
    for (int i = N / 2; i < N; i++) if (px[i][0] > 60 && px[i][0] > px[i][2]) far1 = i;
    printf("ripple front: %d px after one frame, %d px after 176 ms\n", far0 - N / 2, far1 - N / 2);
    assert(far0 > N / 2 && far0 - N / 2 < 12 && far1 > far0 + 10);
    for (int i = 0; i < N; i++) assert(lum(i) == lum(N - 1 - i));
    a.loud = 0;
    for (int f = 0; f < 60; f++) fx_render(FX_RIPPLE, &a, &mo, px, N, 16, white);   // ring and glow gone after a second
    assert(lit_count() == 0);

    // chroma: bass only is red, treble only is blue.
    memset(&a, 0, sizeof a);
    fx_set_react(100);
    for (int f = 0; f < 30; f++) fx_render(FX_CHROMA, &a, &mo, px, N, 16, white);
    a.bands[0] = 255;
    fx_render(FX_CHROMA, &a, &mo, px, N, 16, white);
    assert(px[7][0] > 250 && px[7][1] == 0 && px[7][2] == 0);
    a.bands[0] = 0; a.bands[2] = 255;
    for (int f = 0; f < 30; f++) fx_render(FX_CHROMA, &a, &mo, px, N, 16, white);
    assert(px[7][0] == 0 && px[7][2] > 250);

    // flow: a burst at the start scrolls along the strip.
    memset(&a, 0, sizeof a);
    for (int f = 0; f < 200; f++) fx_render(FX_FLOW, &a, &mo, px, N, 16, white);
    a.loud = 255; a.bands[0] = 255;
    fx_render(FX_FLOW, &a, &mo, px, N, 50, white);
    a.loud = 0;
    fx_render(FX_FLOW, &a, &mo, px, N, 16, white);
    int head = -1;
    for (int i = 0; i < N; i++) if (lum(i) > 100) head = i;
    for (int f = 0; f < 30; f++) fx_render(FX_FLOW, &a, &mo, px, N, 16, white);   // 0.48 s at 60 px/s = ~29 px
    int head2 = -1;
    for (int i = 0; i < N; i++) if (lum(i) > 100) head2 = i;
    printf("flow: burst at %d, then at %d after 480 ms\n", head, head2);
    assert(head >= 0 && head2 > head + 20 && head2 < head + 40);

    // bubble: level sits in the middle; raising the + end 20 degrees glides it towards the + end without overshoot.
    memset(&a, 0, sizeof a);
    mo.up[2] = 1;
    run(FX_BUBBLE, &a, 60);
    float c0 = (float)brightest();
    mo.tilt = 0.342f;
    float prev = c0, overshoot = 0;
    int moved_at = -1;
    for (int f = 0; f < 60; f++) {
        run(FX_BUBBLE, &a, 1);
        float c = (float)brightest();
        if (moved_at < 0 && c > c0 + 5) moved_at = f;
        if (c < prev) overshoot = 1;     // it should only ever move one way
        prev = c;
    }
    printf("bubble: level at %.1f, 20 deg tilt at %.1f (moved 5 px after %d frames)\n", c0, prev, moved_at);
    assert(c0 > N / 2 - 2 && c0 < N / 2 + 1 && prev > c0 + 30 && moved_at >= 0 && moved_at < 10 && !overshoot);

    // tilt: level shows the base colour all along; tilted, the raised (+) end is dimmer than the low end.
    const uint8_t red[3] = {255, 0, 0};
    mo.tilt = 0; mo.side = 0;
    for (int f = 0; f < 60; f++) fx_render(FX_TILT, &a, &mo, px, N, 16, red);
    assert(px[0][0] == px[N - 1][0] && px[0][0] > 240 && px[5][1] == 0);
    mo.tilt = 0.3f;
    for (int f = 0; f < 60; f++) fx_render(FX_TILT, &a, &mo, px, N, 16, red);
    printf("tilt: low end %d, raised end %d\n", lum(0), lum(N - 1));
    assert(lum(0) > lum(N - 1) + 100);

    // shake: nothing at rest; a shake lights sparks; a knock flashes the whole strip.
    memset(&mo, 0, sizeof mo);
    fx_set_react(60);
    run(FX_SHAKE, &a, 60);
    assert(lit_count() == 0);
    mo.energy = 1;
    run(FX_SHAKE, &a, 30);
    int sparks = lit_count();
    mo.energy = 0;
    run(FX_SHAKE, &a, 60);
    int after = lit_count();
    mo.kicks++;
    run(FX_SHAKE, &a, 1);
    printf("shake: %d px lit while shaking, %d a second after, %d on a knock\n", sparks, after, lit_count());
    assert(sparks > N / 4 && after == 0 && lit_count() == N);

    // spin: a quarter turn slides the rainbow a quarter of the strip back.
    memset(&mo, 0, sizeof mo);
    run(FX_SPIN, &a, 1);
    uint8_t first[3];
    memcpy(first, px[3 * N / 4], 3);
    mo.heading = 90;
    run(FX_SPIN, &a, 1);
    printf("spin: pixel 0 after a quarter turn %d,%d,%d, was at %d: %d,%d,%d\n", px[0][0], px[0][1], px[0][2], 3 * N / 4, first[0], first[1], first[2]);
    assert(abs(px[0][0] - first[0]) < 12 && abs(px[0][1] - first[1]) < 12 && abs(px[0][2] - first[2]) < 12);

    // slosh: liquid fills half the strip; it runs to the low (-) end when + is raised, and rocks when pushed.
    memset(&mo, 0, sizeof mo);
    run(FX_SLOSH, &a, 200);
    float mid = centroid();
    int filled = lit_count();
    mo.tilt = 0.4f;
    run(FX_SLOSH, &a, 200);
    float low = centroid();
    mo.tilt = 0;
    run(FX_SLOSH, &a, 200);
    mo.lin_along = 1;                            // a 1 g shove along + for 50 ms
    run(FX_SLOSH, &a, 3);
    mo.lin_along = 0;
    float lo_c = 1e9f, hi_c = -1e9f;
    for (int f = 0; f < 120; f++) { run(FX_SLOSH, &a, 1); float c = centroid(); lo_c = c < lo_c ? c : lo_c; hi_c = c > hi_c ? c : hi_c; }
    run(FX_SLOSH, &a, 400);
    printf("slosh: %d px of liquid, centre %.1f level, %.1f tilted, after a shove swings %.1f..%.1f, settles at %.1f\n",
           filled, mid, low, lo_c, hi_c, centroid());
    assert(filled >= N / 2 - 1 && filled <= N / 2 + 2 && fabsf(mid - (N - 1) / 2.0f) < 1.5f);
    assert(low < N / 4 + 3);
    assert(lo_c < mid - 5 && hi_c > mid + 2);    // lagged back, then rocked past the middle
    assert(fabsf(centroid() - mid) < 1.5f);

    // height: your colour at 0; 10 cm up is another colour; hovering 1 cm either side of 10 cm doesn't flip it back.
    memset(&mo, 0, sizeof mo);
    fx_set_hstep(10);
    const uint8_t blue[3] = {0, 0, 255};
    for (int f = 0; f < 60; f++) fx_render(FX_HEIGHT, &a, &mo, px, N, 16, blue);
    assert(px[3][2] > 250 && px[3][0] == 0);
    uint8_t at0[3]; memcpy(at0, px[3], 3);
    mo.height = 0.14f;
    for (int f = 0; f < 60; f++) fx_render(FX_HEIGHT, &a, &mo, px, N, 16, blue);
    uint8_t at1[3]; memcpy(at1, px[3], 3);
    printf("height: 0 cm %d,%d,%d; 14 cm %d,%d,%d\n", at0[0], at0[1], at0[2], at1[0], at1[1], at1[2]);
    assert(abs(at1[0] - at0[0]) + abs(at1[1] - at0[1]) + abs(at1[2] - at0[2]) > 150);
    for (int f = 0; f < 200; f++) { mo.height = f & 1 ? 0.09f : 0.11f; fx_render(FX_HEIGHT, &a, &mo, px, N, 16, blue); }
    assert(abs(px[3][0] - at1[0]) < 4 && abs(px[3][2] - at1[2]) < 4);
    mo.height = -0.04f;                                       // back below the line by more than the hysteresis
    for (int f = 0; f < 60; f++) fx_render(FX_HEIGHT, &a, &mo, px, N, 16, blue);
    assert(abs(px[3][2] - at0[2]) < 4 && px[3][0] < 4);
    fx_set_hstep(25);
    mo.height = 0.2f;                                         // a bigger step: 20 cm is still step 0
    for (int f = 0; f < 60; f++) fx_render(FX_HEIGHT, &a, &mo, px, N, 16, blue);
    assert(px[3][2] > 250 && px[3][0] < 4);

    // odd lengths and tiny strips don't go out of bounds
    uint8_t one[1][3], odd[7][3];
    for (int k = 0; k < FX_N; k++) {
        a.loud = 255; a.beats++;
        mo.tilt = k & 1 ? 1 : -1; mo.energy = 2; mo.kicks++; mo.heading = 359; mo.lin_along = 3;
        fx_render(k, &a, &mo, one, 1, 16, white);
        fx_render(k, &a, &mo, odd, 7, 16, white);
    }
    puts("fx ok");
    return 0;
}
