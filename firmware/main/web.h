#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Local control without the cloud: mDNS (http://<name>.local), a web server that carries the panel itself, and a
// WebSocket that speaks the MQTT payloads ("state {...}", "live {...}", "reply ok", binary audio) with commands
// going up as plain text.
void web_init(QueueHandle_t cmd_queue);
void web_on_ip(void);                        // start mDNS and the server once the network is up
bool web_set_name(const char *name);         // hostname, letters/digits/dashes; saved and applied at once
const char *web_name(void);
int web_clients(void);                       // WebSocket connections right now
void web_publish(const char *light_json, const char *status_json, const char *sensors_json);
void web_publish_live(const char *json);
void web_publish_reply(const char *text);
void web_publish_audio(const void *pcm, int len);
