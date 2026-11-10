// Host check for main/fusion.c:  cc -O2 -I../main fusion_test.c ../main/fusion.c -lm && ./a.out
// A pretend BMI323 at 200 Hz: the true orientation is kept as the up vector in board coordinates and turned with the
// same angular velocity the gyro reports, plus a gyro bias and sensor noise.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "fusion.h"

#define DT (1.0f / FUSION_HZ)
#define D2R 0.017453293f
static float up[3] = {0, 0, 1};                 // the truth
static float bias[3] = {0.8f, -0.5f, 0.3f};     // deg/s, a typical BMI323 offset
static motion_snap_t m;

static float gauss(void)
{
    float u1 = (rand() + 1.0f) / (RAND_MAX + 2.0f), u2 = (rand() + 1.0f) / (RAND_MAX + 2.0f);
    return sqrtf(-2 * logf(u1)) * cosf(6.2831853f * u2);
}

// One sample with the board turning at w (deg/s, board coordinates) and accelerating by lin (g, board coordinates).
static void step(const float w[3], const float lin[3])
{
    float r[3] = {w[0] * D2R, w[1] * D2R, w[2] * D2R}, d[3] = {up[1] * r[2] - up[2] * r[1], up[2] * r[0] - up[0] * r[2], up[0] * r[1] - up[1] * r[0]};
    float l = 0;
    for (int i = 0; i < 3; i++) {
        up[i] += d[i] * DT;
        l += up[i] * up[i];
    }
    float acc[3], gyr[3];
    for (int i = 0; i < 3; i++) {
        up[i] /= sqrtf(l);
        acc[i] = up[i] + (lin ? lin[i] : 0) + 0.002f * gauss();   // BMI323: ~180 ug/sqrt(Hz) at 50 Hz bandwidth
        gyr[i] = w[i] + bias[i] + 0.06f * gauss();                 // ~0.008 dps/sqrt(Hz)
    }
    fusion_update(acc, gyr, DT);
    fusion_get(&m);
}

static void hold(float secs)
{
    static const float z[3] = {0, 0, 0};
    for (int k = 0; k < secs * FUSION_HZ; k++) {
        step(z, NULL);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    fusion_set_axis(0);
    fusion_reset();

    // Startup: still on a desk. Calibrated after a second, bias found.
    hold(1.2f);
    printf("calibration: state %d, bias %.2f %.2f %.2f (true %.2f %.2f %.2f)\n", m.state, m.bias[0], m.bias[1], m.bias[2], bias[0], bias[1], bias[2]);
    assert(m.state == 1);
    for (int i = 0; i < 3; i++) assert(fabsf(m.bias[i] - bias[i]) < 0.05f);
    assert(fabsf(m.tilt) < 0.01f && fabsf(m.side) < 0.01f);

    // Raise the strip's + end 20 degrees over a second (a turn about -y), then hold: tilt reads sin 20.
    float w[3] = {0, -20, 0};
    float worst_step = 0, prev = m.tilt;
    for (int k = 0; k < FUSION_HZ; k++) {
        step(w, NULL);
        worst_step = fmaxf(worst_step, fabsf(m.tilt - prev));
        prev = m.tilt;
    }
    printf("tilt after raising 20 deg: %.3f (sin 20 = %.3f), largest change in one sample %.4f\n", m.tilt, sinf(20 * D2R), worst_step);
    assert(fabsf(m.tilt - sinf(20 * D2R)) < 0.02f);
    assert(worst_step < 0.01f);                      // smooth: no jumps
    hold(1);
    assert(fabsf(m.tilt - sinf(20 * D2R)) < 0.01f);
    w[1] = 20;
    for (int k = 0; k < FUSION_HZ; k++) step(w, NULL);
    hold(1);
    assert(fabsf(m.tilt) < 0.01f);

    // Shaking along the strip (0.8 g at 3 Hz, no turning) must not read as tilt; energy rises and falls back.
    float max_tilt = 0, max_e = 0;
    static const float z[3] = {0, 0, 0};
    for (int k = 0; k < 2 * FUSION_HZ; k++) {
        float lin[3] = {0.8f * sinf(6.2831853f * 3 * k * DT), 0, 0};
        step(z, lin);
        max_tilt = fmaxf(max_tilt, fabsf(m.tilt));
        if (k > FUSION_HZ / 2) max_e = fmaxf(max_e, m.energy);
    }
    float e_shake = m.energy;
    hold(1.5f);
    printf("shake 0.8 g at 3 Hz: worst false tilt %.3f, energy %.2f (peak %.2f), at rest %.3f\n", max_tilt, e_shake, max_e, m.energy);
    assert(max_tilt < 0.08f);
    assert(max_e > 0.5f && m.energy < 0.05f);

    // A knock: one sharp 2 g spike counts one kick.
    uint32_t k0 = m.kicks;
    for (int k = 0; k < 4; k++) { float lin[3] = {0, 2, 0}; step(z, lin); }
    hold(0.2f);
    assert(m.kicks == k0 + 1);

    // Turn 90 degrees about the vertical in a second, then rest 3 s: heading 90 and no creep.
    w[1] = 0; w[2] = 90;
    for (int k = 0; k < FUSION_HZ; k++) step(w, NULL);
    float h1 = m.heading;
    hold(3);
    printf("heading after a 90 deg turn: %.1f, after 3 s still: %.1f\n", h1, m.heading);
    assert(fabsf(h1 - 90) < 2 && fabsf(m.heading - h1) < 0.5f);

    // Mounted on a wall (the board's +y points up): heading still follows a turn about the vertical.
    up[0] = 0; up[1] = 1; up[2] = 0;
    fusion_reset();
    hold(1.2f);
    assert(m.state == 1 && fabsf(m.up[1] - 1) < 0.01f);
    w[0] = 0; w[1] = -45; w[2] = 0;                 // -y is down: a turn about the vertical
    for (int k = 0; k < FUSION_HZ; k++) step(w, NULL);
    printf("wall mount, 45 deg turn: heading %.1f\n", m.heading);
    assert(fabsf(m.heading - 45) < 2 || fabsf(m.heading - 315) < 2);

    // Never still at startup: runs uncalibrated after FUSION_CAL_GIVEUP_S, learns the bias at the first rest.
    up[0] = 0; up[1] = 0; up[2] = 1;
    fusion_reset();
    for (int k = 0; k < (FUSION_CAL_GIVEUP_S + 1) * FUSION_HZ; k++) {
        float ww[3] = {30 * sinf(k * DT * 5), 20 * cosf(k * DT * 3), 10};
        step(ww, NULL);
    }
    printf("kept moving: state %d\n", m.state);
    assert(m.state == 2);
    hold(1.2f);
    printf("then rested: state %d, bias %.2f %.2f %.2f\n", m.state, m.bias[0], m.bias[1], m.bias[2]);
    assert(m.state == 1);
    for (int i = 0; i < 3; i++) assert(fabsf(m.bias[i] - bias[i]) < 0.1f);

    // Height: barometer (BMP581 at 32x: ~0.5 Pa rms assumed, i.e. ~4 cm) every 50 ms, the accelerometer at 200 Hz
    // reading 0.02 g high. Lift the board 30 cm in 0.6 s, hold, lower it again, and again with a 50 cm lift.
    up[0] = 0; up[1] = 0; up[2] = 1;
    fusion_reset();
    float off[3] = {0, 0, 0.02f};
    for (int k = 0; k < 1.2f * FUSION_HZ; k++) step(z, off);
    const float pa0 = 101325, PI = 3.14159265f, T = 0.6f;
    fusion_baro(pa0 + 4, 0.05f);                     // a stale first reading (the old configuration's): ignored
    float worst_still = 0, worst_move = 0, truth = 0;
    int k = 0;
    for (float t = 0; t < 14; t = ++k * DT) {
        // d = D (1 - cos(pi u / T)) / 2 over each move: 0 -> 30 cm at 2 s, back at 6 s, 0 -> 50 cm at 9 s
        float D = 0, u = -1;
        truth = 0;
        if (t >= 2 && t < 6) { D = 0.3f; u = t - 2; truth = 0.3f; }
        if (t >= 6 && t < 9) { D = -0.3f; u = t - 6; truth = 0; }
        if (t >= 9) { D = 0.5f; u = t - 9; truth = 0.5f; }
        float a = 0;
        if (u >= 0 && u < T) {
            float base = D > 0 ? 0 : 0.3f;
            truth = base + D * (1 - cosf(PI * u / T)) / 2;
            a = D / 2 * (PI / T) * (PI / T) * cosf(PI * u / T);
        }
        float lin[3] = {0, 0, a / 9.80665f + 0.02f};
        step(z, lin);
        if (k % 10 == 0) fusion_baro(pa0 - truth * 11.95f + 0.5f * gauss(), 0.05f);
        if (t < 1.4f) assert(!m.have_height);     // still settling: no height yet
        float err = fabsf(m.height - truth);
        bool moving = u >= 0 && u < T + 1;
        if (moving) worst_move = fmaxf(worst_move, err); else worst_still = fmaxf(worst_still, err);
    }
    printf("height (0.5 Pa baro noise, 0.02 g accel offset): worst error while moving %.1f cm, while held %.1f cm; at 50 cm reads %.1f\n",
           worst_move * 100, worst_still * 100, m.height * 100);
    assert(worst_move < 0.05f && worst_still < 0.03f && fabsf(m.height - 0.5f) < 0.02f);

    puts("fusion ok");
    return 0;
}
