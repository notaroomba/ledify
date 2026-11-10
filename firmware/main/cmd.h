#pragma once
// One command line from the console, BLE, MQTT or the board's WebSocket, handled on the main task.
enum { CMD_SRC_LOCAL, CMD_SRC_BLE, CMD_SRC_MQTT, CMD_SRC_WS };   // LOCAL: USB console and schedules

typedef struct {
    char text[192];
    unsigned char src;                  // CMD_SRC_*: decides where "listen" sends the mic audio
} cmd_t;
