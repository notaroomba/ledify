#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Wi-Fi station + MQTT. Settings live in NVS (namespace "cfg": ssid, pass, mqtt_url, mqtt_user,
// mqtt_pass), never in the code. Commands arriving over MQTT are queued like BLE ones.
void net_init(QueueHandle_t cmd_queue);
bool net_save_wifi(const char *ssid, const char *pass);
bool net_fallback_add(const char *ssid, const char *pass);   // add or update, pass "" for an open network; false if full or too long
bool net_fallback_del(const char *ssid_or_n);                // by name or list number
bool net_fallback_clear(void);
void net_fallback_list(char *out, size_t n);                 // names only, never the passwords
bool net_fallback_try(const char *ssid_or_n);                // join this fallback now and stay on it until the next boot
bool net_save_mqtt(const char *url, const char *user, const char *pass);
void net_status(char *out, size_t n);        // short human-readable line
bool net_mqtt_connected(void);
const char *net_ip(void);                    // "" until connected
// Publish state. light_json follows Home Assistant's JSON light schema; status_json is the
// full status() output; sensors_json carries the readings used by the discovered sensors.
void net_publish(const char *light_json, const char *status_json, const char *sensors_json);
void net_publish_live(const char *json);   // not retained, QoS 0
void net_publish_reply(const char *text);  // command replies, not retained
