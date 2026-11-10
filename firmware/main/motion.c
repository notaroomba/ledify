// Motion task: reads the IMU every 5 ms and runs the fusion. Commands from other tasks are handed over as flags
// so fusion.c only ever runs here.
#include <string.h>
#include "motion.h"
#include "sensors.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "motion";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static motion_snap_t snap;
static volatile bool want_cal, want_zero, want_hzero;
static volatile int want_axis = -1;
static bool running;

static void motion_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    int64_t last = esp_timer_get_time();
    uint8_t was_state = 0;
    int n = 0;
    int64_t last_baro = 0;
    for (;;) {
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(1000 / FUSION_HZ));
        if (want_axis >= 0) {
            fusion_set_axis(want_axis);
            want_axis = -1;
        }
        if (want_cal) {
            want_cal = false;
            fusion_reset();
            ESP_LOGI(TAG, "calibrating: hold the board still");
        }
        if (want_zero) {
            want_zero = false;
            fusion_zero();
        }
        if (want_hzero) {
            want_hzero = false;
            fusion_zero_height();
        }
        if (++n % (FUSION_HZ / 20) == 0) {               // the barometer converts at 20 Hz
            float t, pa;
            int64_t tb = esp_timer_get_time();
            if (sensors_baro_sample(&t, &pa)) {
                fusion_baro(pa, last_baro ? (tb - last_baro) / 1e6f : 0);
                last_baro = tb;
            }
        }
        float acc[3], gyr[3];
        int64_t now = esp_timer_get_time();
        if (!sensors_imu_sample(acc, gyr)) {
            continue;
        }
        fusion_update(acc, gyr, (now - last) / 1e6f);   // the real interval, so a late wake doesn't skew the turn
        last = now;
        motion_snap_t s;
        fusion_get(&s);
        portENTER_CRITICAL(&lock);
        snap = s;
        portEXIT_CRITICAL(&lock);
        if (s.state != was_state) {
            if (s.state == 1) {
                ESP_LOGI(TAG, "calibrated: gyro bias %.2f %.2f %.2f deg/s, up %.2f %.2f %.2f", s.bias[0], s.bias[1], s.bias[2],
                         s.up[0], s.up[1], s.up[2]);
            } else if (s.state == 2) {
                ESP_LOGW(TAG, "board kept moving: running uncalibrated until it rests for a second");
            }
            was_state = s.state;
        }
    }
}

bool motion_init(void)
{
    if (!sensors_have_imu()) {
        return false;
    }
    fusion_reset();
    sensors_claim_baro();
    xTaskCreate(motion_task, "motion", 3072, NULL, 4, NULL);
    running = true;
    return true;
}

void motion_snapshot(motion_snap_t *s)
{
    if (!running) {
        memset(s, 0, sizeof *s);
        s->up[2] = 1;
        return;
    }
    portENTER_CRITICAL(&lock);
    *s = snap;
    portEXIT_CRITICAL(&lock);
}

void motion_cal(void)
{
    want_cal = true;
}

void motion_zero(void)
{
    want_zero = true;
}

void motion_set_axis(int axis)
{
    if (running) {
        want_axis = axis;
    } else {
        fusion_set_axis(axis);
    }
}

void motion_zero_height(void)
{
    want_hzero = true;
}
