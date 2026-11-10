// Host check for main/fx.c:  cc -O2 -I../main fx_test.c ../main/fx.c -lm && ./a.out
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fx.h"

#define N 120
static uint8_t px[N][3];
static const uint8_t white[3] = {255, 255, 255};

static int lum(int i) { return px[i][0] + px[i][1] + px[i][2]; }
static int lit_count(void) { int c = 0; for (int i = 0; i < N; i++) c += lum(i) > 0; return c; }

int main(void)
{
    mic_snap_t a;
    memset(&a, 0, sizeof a);
    fx_set_react(60);

    // eq: only the zone of the loud band lights, and it lights on the very first frame.
    a.spec[5] = 255;
    fx_render(FX_EQ, &a, px, N, 16, white);
    for (int i = 0; i < N; i++) {
        int k = i * 16 / N;
        assert(k == 5 ? lum(i) > 200 : lum(i) == 0);
    }
    // release: react 60 falls to under half within ~150 ms, react 1 still holds most of it.
    a.spec[5] = 0;
    for (int f = 0; f < 10; f++) fx_render(FX_EQ, &a, px, N, 16, white);   // 160 ms
    int fast = lum(5 * N / 16 + 1);
    fx_set_react(1);
    a.spec[5] = 255; fx_render(FX_EQ, &a, px, N, 16, white); a.spec[5] = 0;
    for (int f = 0; f < 10; f++) fx_render(FX_EQ, &a, px, N, 16, white);
    int slow = lum(5 * N / 16 + 1);
    printf("eq after 160 ms of silence: react 60 -> %d, react 1 -> %d (of 255)\n", fast, slow);
    assert(fast < 60 && slow > 150);
    fx_set_react(100);
    for (int f = 0; f < 60; f++) fx_render(FX_EQ, &a, px, N, 16, white);
    assert(lit_count() == 0);

    // mirror: loudness 128 lights about half of each half, symmetric around the centre.
    a.loud = 128;
    fx_render(FX_MIRROR, &a, px, N, 16, white);
    int c = lit_count();
    printf("mirror at loud 128: %d of %d lit\n", c, N);
    assert(c >= N / 2 - 4 && c <= N / 2 + 4);
    for (int i = 0; i < N; i++) assert(lum(i) == lum(N - 1 - i));
    assert(lum(N / 2) > 0 && lum(0) == 0);
    // the peak dot stays after the sound stops
    a.loud = 0;
    for (int f = 0; f < 10; f++) fx_render(FX_MIRROR, &a, px, N, 16, white);
    assert(lit_count() == 2);

    // ripple: a beat starts a ring at the centre on the next frame, and it travels outward.
    fx_set_react(60);
    a.loud = 200; a.bands[0] = 255;
    fx_render(FX_RIPPLE, &a, px, N, 16, white);              // primes the beat counter
    a.beats++;
    fx_render(FX_RIPPLE, &a, px, N, 16, white);
    int far0 = 0;
    for (int i = N / 2; i < N; i++) if (px[i][0] > 100) far0 = i;
    for (int f = 0; f < 10; f++) fx_render(FX_RIPPLE, &a, px, N, 16, white);
    int far1 = 0;
    for (int i = N / 2; i < N; i++) if (px[i][0] > 60 && px[i][0] > px[i][2]) far1 = i;
    printf("ripple front: %d px after one frame, %d px after 176 ms\n", far0 - N / 2, far1 - N / 2);
    assert(far0 > N / 2 && far0 - N / 2 < 12 && far1 > far0 + 10);
    for (int i = 0; i < N; i++) assert(lum(i) == lum(N - 1 - i));
    a.loud = 0;
    for (int f = 0; f < 60; f++) fx_render(FX_RIPPLE, &a, px, N, 16, white);   // ring and glow gone after a second
    assert(lit_count() == 0);

    // chroma: bass only is red, treble only is blue.
    memset(&a, 0, sizeof a);
    fx_set_react(100);
    for (int f = 0; f < 30; f++) fx_render(FX_CHROMA, &a, px, N, 16, white);
    a.bands[0] = 255;
    fx_render(FX_CHROMA, &a, px, N, 16, white);
    assert(px[7][0] > 250 && px[7][1] == 0 && px[7][2] == 0);
    a.bands[0] = 0; a.bands[2] = 255;
    for (int f = 0; f < 30; f++) fx_render(FX_CHROMA, &a, px, N, 16, white);
    assert(px[7][0] == 0 && px[7][2] > 250);

    // flow: a burst at the start scrolls along the strip.
    memset(&a, 0, sizeof a);
    for (int f = 0; f < 200; f++) fx_render(FX_FLOW, &a, px, N, 16, white);
    a.loud = 255; a.bands[0] = 255;
    fx_render(FX_FLOW, &a, px, N, 50, white);
    a.loud = 0;
    fx_render(FX_FLOW, &a, px, N, 16, white);
    int head = -1;
    for (int i = 0; i < N; i++) if (lum(i) > 100) head = i;
    for (int f = 0; f < 30; f++) fx_render(FX_FLOW, &a, px, N, 16, white);   // 0.48 s at 60 px/s = ~29 px
    int head2 = -1;
    for (int i = 0; i < N; i++) if (lum(i) > 100) head2 = i;
    printf("flow: burst at %d, then at %d after 480 ms\n", head, head2);
    assert(head >= 0 && head2 > head + 20 && head2 < head + 40);

    // odd lengths and tiny strips don't go out of bounds
    uint8_t one[1][3], odd[7][3];
    for (int k = 0; k < FX_N; k++) {
        a.loud = 255; a.beats++;
        fx_render(k, &a, one, 1, 16, white);
        fx_render(k, &a, odd, 7, 16, white);
    }
    puts("fx ok");
    return 0;
}
