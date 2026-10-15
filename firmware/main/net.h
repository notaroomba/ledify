#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Wi-Fi station + MQTT. Settings live in NVS (namespace "cfg": ssid, pass, mqtt_url, mqtt_user,
// mqtt_pass), never in the code. Commands arriving over MQTT are queued like BLE ones.
void net_init(QueueHandle_t cmd_queue);
bool net_save_wifi(const char *ssid, const char *pass);
bool net_save_wifi2(const char *ssid, const char *pass);   // fallback network, "" to clear
bool net_save_mqtt(const char *url, const char *user, const char *pass);
void net_status(char *out, size_t n);        // short human-readable line
bool net_mqtt_connected(void);
// Publish state. light_json follows Home Assistant's JSON light schema; status_json is the
// full status() output; sensors_json carries the readings used by the discovered sensors.
void net_publish(const char *light_json, const char *status_json, const char *sensors_json);
