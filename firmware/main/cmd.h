#pragma once
// One command line from the console, BLE, MQTT or the board's WebSocket, handled on the main task.
typedef struct {
    char text[192];
} cmd_t;
