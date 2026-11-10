#pragma once
#include <stdbool.h>
#include <stdint.h>

// IMU fusion for the motion styles: startup calibration, a complementary filter that tracks which way is up, and
// the few numbers the styles draw from. Plain C with no ESP-IDF calls, so test/fusion_test.c runs it on the host.
//
// Calibration: the first second the board holds still, the gyro's mean is its bias and the accelerometer gives the
// "zero pose" (how the board sits). If it is moving, calibration waits for a still second; after FUSION_CAL_GIVEUP_S
// it starts anyway with no bias and learns one the next time the board rests.
//
// Fusion: the up vector in board coordinates is turned by the gyro every sample (instant, smooth, drifts slowly) and
// pulled towards the accelerometer with a ~0.5 s time constant (noisy, but never drifts). The pull fades out while the
// board is being shaken (|a| far from 1 g), so a shake doesn't read as a tilt. Heading is the gyro's turn rate about
// that up axis, integrated, so it works whichever way the board is mounted. With no magnetometer it drifts a little;
// the gyro bias is re-learnt whenever the board rests for a second to keep that small.
typedef struct motion_snap {
    uint8_t state;        // 0 calibrating, 1 calibrated, 2 running without a calibration (the board never held still)
    bool still;           // resting right now
    float up[3];          // unit vector pointing up, in board coordinates
    float tilt;           // -1..1: sine of how far the strip's + end is raised, relative to the zero pose
    float side;           // -1..1: the same across the strip
    float heading;        // degrees turned about the vertical since the zero pose, 0..360
    float yaw_rate;       // degrees per second about the vertical
    float lin[3];         // acceleration with gravity removed, in g, board coordinates
    float lin_along;      // ... along the strip, g
    float energy;         // how much it is moving: about 0 at rest, 1 for a brisk shake
    uint32_t kicks;       // jolts so far (a tap or a sharp knock): compare with the last value to catch each one
    float bias[3];        // gyro bias in use, deg/s
    bool have_height;     // the barometer is feeding fusion_baro()
    float height;         // metres above where height was last zeroed (barometer + accelerometer)
    float vz;             // vertical speed, m/s
} motion_snap_t;

#define FUSION_HZ 200
#define FUSION_CAL_GIVEUP_S 6

void fusion_reset(void);                       // start calibrating again (motion cal)
void fusion_zero(void);                        // the current pose becomes level, heading 0 (motion zero)
// Which board axis the strip runs along: 0 +x, 1 +y, 2 +z, 3 -x, 4 -y, 5 -z.
void fusion_set_axis(int axis);
// One sample: acc in g, gyr in deg/s (raw, bias included), dt in seconds.
void fusion_update(const float acc[3], const float gyr[3], float dt);
void fusion_get(motion_snap_t *s);
// Height: the barometer (pressure in Pa, every dt s, ~20 Hz) gives the height without drift but with a few cm of
// noise and some lag; the accelerometer's vertical part, integrated twice between readings, gives the movement at
// once but drifts within seconds. A third-order complementary filter (as in a variometer) takes the slow part from
// the barometer and the fast part from the accelerometer, and learns the accelerometer's vertical offset too.
void fusion_baro(float pa, float dt);
void fusion_zero_height(void);                 // here is height 0
