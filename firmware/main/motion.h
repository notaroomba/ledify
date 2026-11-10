#pragma once
#include <stdbool.h>
#include "fusion.h"

// The BMI323 sampled at FUSION_HZ in its own task, through fusion.c. Calibrates by itself after boot.
bool motion_init(void);                 // false without an IMU
void motion_snapshot(motion_snap_t *s);
void motion_cal(void);                  // calibrate again (hold the board still for a second)
void motion_zero(void);                 // the way the board sits now becomes level, heading 0
void motion_set_axis(int axis);         // 0 +x, 1 +y, 2 +z, 3 -x, 4 -y, 5 -z: the board axis along the strip
