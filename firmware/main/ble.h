#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "cmd.h"

void ble_init(QueueHandle_t cmd_queue);   // lines written to the RX characteristic land here
void ble_send(const char *s);            // notify on the TX characteristic, chunked to the MTU
bool ble_connected(void);
