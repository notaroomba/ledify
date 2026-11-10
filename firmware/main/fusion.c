// IMU fusion: see fusion.h. A few dozen float operations per sample at 200 Hz, a couple of percent of the C6's
// time in software floating point.
#include <math.h>
#include <string.h>
#include "fusion.h"

#define D2R 0.017453293f
#define KP 2.0f                 // pull towards the accelerometer, 1/s: a 0.5 s time constant
#define TRUST_G 0.2f            // the pull fades to nothing as |a| strays this far from 1 g
#define STILL_GYR 3.0f          // deg/s from the bias: anything faster is not resting
#define STILL_ACC 0.05f         // g from 1 g
#define STILL_SD 0.8f           // a still second's gyro may scatter this much per axis (standard deviation, deg/s)
#define STILL_ACC_SD 0.008f     // and the accelerometer's magnitude this much (g)
#define HOLD_G 0.012f           // height hold: gravity-free acceleration under this (smoothed over 50 ms)
#define HOLD_DPS 3.0f           // and turning slower than this, for a whole second, means the board isn't being moved
#define KICK_G 1.2f             // a jolt (a knock, a sharp flick): gravity-free acceleration over this
#define KICK_REARM_G 0.4f       // and back under this before the next one counts

static const float axes[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
static int axis;
static motion_snap_t s;
static float u0[3] = {0, 0, 1};             // up in the zero pose
static float across[3];                     // perpendicular to the strip and to u0
static int cal_n, cal_total;                // samples in this calibration try, and since calibration started
static int run_n;                           // rest detector: samples in the current quiet run
static float run_sum[3], run_sq[3], run_asum, run_asq, rest_g;   // rest_g: |a| over the last still second
static float sm_dev[3], sm_an = 1;          // rest detector input, smoothed over 50 ms so one noisy sample can't trip it
static float hold_l, hold_w;                // height hold inputs, smoothed the same way
static float acc_lp[3] = {0, 0, 1};         // the accelerometer over the last ~0.5 s: a hold judges the change from it
static int quiet_n;                         // samples in a row the board hasn't been moved
static bool was_hold;
static float held_h, ep_v, ep_vmax;         // a break in the hold: the height before it, and the fastest the
#define JIGGLE_MS 0.05f                     // accelerometer alone saw it go; under this (m/s) it wasn't lifted
static float cal_acc[3];
static bool kick_armed = true;
// Height filter: ALT_W is the crossover in rad/s; below it the barometer wins, above it the accelerometer.
#define ALT_W 0.8f
static float h_ref;                         // barometric altitude of height 0, m
static bool h_have;
static int h_warm;                          // barometer readings seen before height starts
static float h_sum;
#define H_SKIP 20                           // the first second after (re)configuring the BMP581 is not trusted
#define H_AVG 10                            // then half a second averaged is height 0
static float acc_off;                       // learnt vertical accelerometer offset, m/s^2

static float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float len(const float *a) { return sqrtf(dot(a, a)); }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

static void cross(const float *a, const float *b, float *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static void norm(float *a)
{
    float l = len(a);
    if (l > 1e-6f) {
        a[0] /= l;
        a[1] /= l;
        a[2] /= l;
    }
}

// The across direction: perpendicular to the strip and to up in the zero pose. If the strip itself points up,
// any perpendicular will do.
static void set_across(void)
{
    cross(axes[axis], u0, across);
    if (len(across) < 0.2f) {
        cross(axes[axis], axes[(axis + 1) % 3], across);
    }
    norm(across);
}

static void run_restart(void)
{
    run_n = 0;
    for (int i = 0; i < 3; i++) {
        run_sum[i] = 0;
        run_sq[i] = 0;
    }
    run_asum = 0;
    run_asq = 0;
}

void fusion_reset(void)
{
    uint32_t kicks = s.kicks;
    memset(&s, 0, sizeof s);
    s.kicks = kicks;
    h_have = s.have_height = false;
    was_hold = false;
    ep_vmax = 1;
    h_warm = 0;
    h_sum = 0;
    acc_off = 0;
    s.up[2] = 1;
    cal_n = cal_total = 0;
    memset(cal_acc, 0, sizeof cal_acc);
    run_restart();
}

void fusion_zero(void)
{
    memcpy(u0, s.up, sizeof u0);
    s.heading = 0;
    set_across();
}

void fusion_set_axis(int a)
{
    axis = a < 0 || a > 5 ? 0 : a;
    set_across();
}

// Feed the rest detector one sample; returns true each time a whole still second has gone by, with its mean gyro.
static bool rest(const float acc[3], const float gyr[3], float mean[3])
{
    float an = len(acc);
    const float a = 0.1f;                        // 50 ms at 200 Hz
    for (int i = 0; i < 3; i++) {
        sm_dev[i] += (gyr[i] - s.bias[i] - sm_dev[i]) * a;
    }
    sm_an += (an - sm_an) * a;
    bool quiet = len(sm_dev) < STILL_GYR && fabsf(sm_an - 1) < STILL_ACC;
    if (s.state == 0) {
        quiet = fabsf(sm_an - 1) < 0.15f;       // no bias known yet: judge by the spread alone
    }
    if (!quiet) {
        run_restart();
        s.still = false;
        return false;
    }
    run_n++;
    for (int i = 0; i < 3; i++) {
        run_sum[i] += gyr[i];
        run_sq[i] += gyr[i] * gyr[i];
    }
    run_asum += an;
    run_asq += an * an;
    if (run_n < FUSION_HZ) {
        return false;
    }
    rest_g = run_asum / run_n;
    bool ok = run_asq / run_n - rest_g * rest_g < STILL_ACC_SD * STILL_ACC_SD;
    for (int i = 0; i < 3; i++) {
        mean[i] = run_sum[i] / run_n;
        ok = ok && run_sq[i] / run_n - mean[i] * mean[i] < STILL_SD * STILL_SD;
    }
    run_restart();
    s.still = ok;
    return ok;
}

static void start(const float up[3], const float *bias, uint8_t state)
{
    memcpy(s.up, up, sizeof s.up);
    norm(s.up);
    if (bias) {
        memcpy(s.bias, bias, sizeof s.bias);
    }
    s.state = state;
    fusion_zero();
}

void fusion_update(const float acc[3], const float gyr[3], float dt)
{
    float an = len(acc), mean[3];
    if (an < 0.05f || dt <= 0) {
        return;                                  // a dead read
    }
    dt = dt > 0.05f ? 0.05f : dt;
    bool still_second = rest(acc, gyr, mean);

    if (s.state == 0) {                          // calibrating: wait for a still second
        cal_total++;
        for (int i = 0; i < 3; i++) {
            cal_acc[i] += acc[i];
            s.up[i] = acc[i] / an;
        }
        cal_n++;
        if (still_second) {
            start(cal_acc, mean, 1);
            acc_off = (rest_g - 1) * 9.80665f;   // at rest the vertical reading is exactly 1 g: the rest is offset
            fusion_zero_height();                // and wherever it rests at boot is height 0
        } else if (cal_total > FUSION_CAL_GIVEUP_S * FUSION_HZ) {
            start(acc, NULL, 2);                 // never held still: go on, learn the bias at the next rest
        } else if (run_n == 0) {
            cal_n = 0;                           // it moved: the next try averages from here
            memset(cal_acc, 0, sizeof cal_acc);
        }
        return;
    }
    if (still_second) {                          // resting: follow the gyro's drift (fully, if it was never calibrated)
        float k = s.state == 2 ? 1.0f : 0.3f;
        for (int i = 0; i < 3; i++) {
            s.bias[i] += (mean[i] - s.bias[i]) * k;
        }
        s.state = 1;
        acc_off += ((rest_g - 1) * 9.80665f - acc_off) * k;
        s.vz = 0;                                // resting: not climbing either
    }

    // Turn up by the gyro: a vector fixed in the room moves as v' = v x w in board coordinates.
    float w[3], d[3];
    for (int i = 0; i < 3; i++) {
        w[i] = (gyr[i] - s.bias[i]) * D2R;
    }
    cross(s.up, w, d);
    for (int i = 0; i < 3; i++) {
        s.up[i] += d[i] * dt;
    }
    // Pull towards the accelerometer, less the harder it is being shaken.
    float trust = clampf(1 - fabsf(an - 1) / TRUST_G, 0, 1), k = KP * trust * dt;
    for (int i = 0; i < 3; i++) {
        s.up[i] += (acc[i] / an - s.up[i]) * k;
    }
    norm(s.up);

    s.yaw_rate = s.still ? 0 : dot(w, s.up) / D2R;   // resting: no creep
    s.heading = fmodf(s.heading + s.yaw_rate * dt + 360.0f, 360.0f);
    s.tilt = clampf(dot(s.up, axes[axis]) - dot(u0, axes[axis]), -1, 1);
    s.side = clampf(dot(s.up, across), -1, 1);
    for (int i = 0; i < 3; i++) {
        s.lin[i] = acc[i] - s.up[i];
    }
    s.lin_along = dot(s.lin, axes[axis]);
    float l = len(s.lin), wd = len(w) / D2R;
    if (l > KICK_G && kick_armed) {
        s.kicks++;
        kick_armed = false;
    } else if (l < KICK_REARM_G) {
        kick_armed = true;
    }
    // Energy: a fast attack (20 ms) so a flick shows at once, a 300 ms release so it doesn't flicker.
    float e = fmaxf(0, l - 0.03f) * 1.5f + fmaxf(0, wd - 5) / 300;
    float tau = e > s.energy ? 0.02f : 0.3f;
    s.energy += (e - s.energy) * (dt / (tau + dt));

    float jig[3];                                // height hold: has anyone moved the board in the last second? Judged by the
    for (int i = 0; i < 3; i++) {                // change in acceleration, so the sensor's own offset doesn't count
        acc_lp[i] += (acc[i] - acc_lp[i]) * (dt / (0.5f + dt));
        jig[i] = acc[i] - acc_lp[i];
    }
    hold_l += (len(jig) - hold_l) * 0.1f;
    hold_w += (wd - hold_w) * 0.1f;
    quiet_n = hold_l < HOLD_G && hold_w < HOLD_DPS ? quiet_n + 1 : 0;
    s.hold = s.state == 1 && quiet_n > FUSION_HZ;

    if (h_have) {                                // height: predict from the vertical acceleration
        float az = (dot(acc, s.up) - 1) * 9.80665f - acc_off;
        s.vz += az * dt;
        s.height += s.vz * dt;
        if (s.hold) {
            s.vz = 0;                            // resting: it isn't climbing, whatever the integral says
        }
        // A break in the hold (a button pressed, a knock, the board nudged): the accelerometer on its own says how
        // fast it really moved. If it never reached JIGGLE_MS (10 cm in a second peaks at 0.16 m/s), it wasn't lifted,
        // and whatever the barometer did meanwhile (a finger near its vent, noise) is undone once it rests again.
        if (!s.hold) {
            if (was_hold) {
                held_h = s.height;
                ep_v = ep_vmax = 0;
            }
            ep_v += az * dt;
            ep_vmax = fmaxf(ep_vmax, fabsf(ep_v));
        } else if (!was_hold && ep_vmax < JIGGLE_MS) {
            h_ref += s.height - held_h;
            s.height = held_h;
            quiet_n = 4 * FUSION_HZ + 1;         // and no settling window: nothing moved to settle
        }
    }
    was_hold = s.hold;
}

void fusion_baro(float pa, float dt)
{
    if (pa < 30000 || pa > 120000 || dt <= 0) {
        return;
    }
    float alt = 44330.0f * (1 - powf(pa / 101325.0f, 0.190295f));
    s.baro_pa = pa;
    s.baro_h = h_have ? alt - h_ref : 0;
    s.acc_off = acc_off;
    if (!h_have) {                               // settle, then average a reference rather than trust one reading
        if (++h_warm > H_SKIP) {
            h_sum += alt;
        }
        if (h_warm == H_SKIP + H_AVG) {
            h_ref = h_sum / H_AVG;
            s.height = s.vz = 0;
            h_have = s.have_height = true;
        }
        return;
    }
    dt = dt > 0.5f ? 0.5f : dt;
    float err = (alt - h_ref) - s.height;
    if (s.hold && quiet_n > 4 * FUSION_HZ) {
        // Resting (the IMU has seen no movement for 4 s: the first 3 s of a rest still let the barometer finish
        // settling the height after a move): the board isn't changing height, so whatever the
        // barometer says now is the air (a door, the heating, the weather, the sensor warming), not a climb. Let height
        // 0 follow the pressure (over ~2 s, which also averages the noise) and keep the height as it is. On the board
        // the pressure crept ~1.5 Pa a minute (13 cm/min) after a restart; this keeps that from adding up.
        h_ref += err * (dt / (2 + dt));
        s.vz = 0;
        return;
    }
    s.height += 3 * ALT_W * err * dt;            // poles all at -ALT_W: no overshoot
    s.vz += 3 * ALT_W * ALT_W * err * dt;
    acc_off -= ALT_W * ALT_W * ALT_W * err * dt;
}

void fusion_zero_height(void)
{
    held_h -= s.height;                          // a break in progress keeps its place relative to the new zero
    h_ref += s.height;
    s.height = 0;
    s.vz = 0;
}

void fusion_get(motion_snap_t *o)
{
    *o = s;
}
