#pragma once
// One command line from BLE or MQTT, handled on the main task.
typedef struct {
    char text[192];
} cmd_t;
