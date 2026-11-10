#pragma once
#include <stdint.h>
// One command line, handled on the main task. src says where it came from: setup commands (Wi-Fi, broker, OTA
// source) are only taken from the USB console, or from anywhere while the setup window is open (button held 3 s).
enum { CMD_SRC_OTHER, CMD_SRC_CONSOLE, CMD_SRC_BLE, CMD_SRC_MQTT, CMD_SRC_WS };   // OTHER: schedules and anything unnamed

typedef struct {
    char text[192];
    uint8_t src;                        // CMD_SRC_*
} cmd_t;

// The setup window's one-time pairing code while the window is open (the button was held 3 s), else NULL. The board
// reports it on ledify/ID/pair, which only the panel server may read, and gives it out over Bluetooth (and USB) only,
// so whoever is in front of the board can link their browser to it from the hosted panel.
const char *pair_code(void);
