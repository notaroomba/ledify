#pragma once
#include <stdbool.h>
#include "driver/i2c_master.h"

typedef struct {
    bool have_lux, have_imu, have_baro;
    float lux;
    float acc[3];          // g
    float gyr[3];          // deg/s
    float temp_c;
    float pressure_pa;
} sensor_data_t;

void sensors_init(i2c_master_bus_handle_t bus);
void sensors_read(sensor_data_t *d);
// The IMU belongs to the motion task (motion.c): it reads here 200 times a second, sensors_read() hands out its
// latest sample. SPI devices may only be used from one task each.
bool sensors_have_imu(void);
bool sensors_imu_sample(float acc[3], float gyr[3]);
