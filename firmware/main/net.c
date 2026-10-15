// Wi-Fi station and MQTT client with Home Assistant discovery.
//
// Topics (ID = last three bytes of the Wi-Fi MAC, e.g. ledify_d3ed08):
//   ledify/ID/cmd          text commands, same as BLE ("rgb 255 0 0", "mode rainbow", ...)
//   ledify/ID/light/set    Home Assistant JSON light commands
//   ledify/ID/KEY/set      a bare value from a Home Assistant entity, run as the command "KEY value": count, type,
//                          auto (ON/OFF), gain, lux_lo, lux_hi, night, temp_lo, temp_hi, temp (a reading in C)
//   ledify/ID/light/state  JSON light state
//   ledify/ID/state        full status JSON
//   ledify/ID/audio        16 kHz 16-bit PCM while "listen N" is active (the panel plays it)
//   ledify/ID/live         {"loud","bands","pulse","db","cap"} at 10 Hz while "live N" is active
//   ledify/ID/audio/set    "L B M T P" loudness, bands and pulse from the panel (overrides the mic)
//   ledify/ID/sensors      {"lux":..,"temp_c":..,"pressure_kpa":..,"sound_db":..}
//   ledify/ID/status       online / offline (retained, last will)
#include <string.h>
#include <stdio.h>
#include "net.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_crt_bundle.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cmd.h"
#include "board.h"

static const char *TAG = "net";
static QueueHandle_t cmd_q;
static char ssid[33], pass[65], ssid2[33], pass2[65], mqtt_url[128], mqtt_user[48], mqtt_pass[64];
static int which, fails;                  // which network we are on (0 primary, 1 fallback) and failed joins in a row
static char id[16];                       // ledify_xxxxxx
static char ip[16];
static bool wifi_up, mqtt_up;
static esp_mqtt_client_handle_t mqtt;
static char prefix[32];                   // ledify/ID/
static char t_cmd[48], t_set[48], t_any[48], t_live[48], t_audio[48], t_lstate[48], t_state[48], t_sensors[48], t_avail[48];

static bool load(const char *key, char *buf, size_t n)
{
    nvs_handle_t h;
    buf[0] = 0;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = n;
    esp_err_t e = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return e == ESP_OK && buf[0];
}

static bool save(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t e = nvs_set_str(h, key, val);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e == ESP_OK;
}

bool net_save_wifi(const char *s, const char *p)
{
    return save("ssid", s) && save("pass", p);
}

bool net_save_wifi2(const char *s, const char *p)
{
    return save("ssid2", s) && save("pass2", p);
}

// Point the station at network i; the connect itself happens on the next STA_START / DISCONNECTED event.
static void apply_wifi(int i)
{
    const char *s = i ? ssid2 : ssid, *p = i ? pass2 : pass;
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, s, sizeof wc.sta.ssid - 1);
    strncpy((char *)wc.sta.password, p, sizeof wc.sta.password - 1);
    wc.sta.threshold.authmode = p[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_LOGI(TAG, "wifi: joining \"%s\"%s", s, i ? " (fallback)" : "");
}

bool net_save_mqtt(const char *url, const char *user, const char *pw)
{
    return save("mqtt_url", url) && save("mqtt_user", user ? user : "") && save("mqtt_pass", pw ? pw : "");
}

static void queue_cmd(const char *data, int len)
{
    cmd_t c = {0};
    if (len >= (int)sizeof c.text) {
        len = sizeof c.text - 1;
    }
    memcpy(c.text, data, len);
    xQueueSend(cmd_q, &c, 0);
}

static bool is_topic(esp_mqtt_event_handle_t ev, const char *t)
{
    return ev->topic_len == (int)strlen(t) && !strncmp(ev->topic, t, ev->topic_len);
}

// ledify/ID/KEY/set: pull KEY out of the topic.
static bool set_key(esp_mqtt_event_handle_t ev, char *key, size_t n)
{
    int pl = strlen(prefix), kl = ev->topic_len - pl - 4;
    if (kl < 1 || kl >= (int)n || strncmp(ev->topic, prefix, pl) || strncmp(ev->topic + pl + kl, "/set", 4)) {
        return false;
    }
    memcpy(key, ev->topic + pl, kl);
    key[kl] = 0;
    return true;
}

// Home Assistant entities publish a bare value; turn it into the matching text command.
static void queue_prefixed(const char *cmd, const char *data, int len)
{
    char c[48];
    snprintf(c, sizeof c, "%s %.*s", cmd, len > 16 ? 16 : len, data);
    queue_cmd(c, strlen(c));
}

// One retained discovery message per entity so Home Assistant creates them on its own.
static void discover(void)
{
    char topic[96], msg[640], devs[160], cmd[64];
    snprintf(devs, sizeof devs, "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Ledify\",\"mf\":\"notaroomba\",\"mdl\":\"Ledify rev1\"}", id);

    snprintf(topic, sizeof topic, "homeassistant/light/%s/config", id);
    snprintf(msg, sizeof msg,
             "{\"name\":\"Ledify\",\"uniq_id\":\"%s_light\",\"schema\":\"json\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"avty_t\":\"%s\",\"brightness\":true,\"supported_color_modes\":[\"rgb\"],\"effect\":true,"
             "\"effect_list\":[\"solid\",\"rainbow\",\"temp\",\"spectrum\",\"meter\",\"palette\",\"sound\",\"vu\",\"pulse\"],%s}",   // keep in step with MODES in main.c
             id, t_set, t_lstate, t_avail, devs);
    esp_mqtt_client_publish(mqtt, topic, msg, 0, 1, 1);

    snprintf(topic, sizeof topic, "homeassistant/select/%s_type/config", id);
    snprintf(cmd, sizeof cmd, "%stype/set", prefix);
    snprintf(msg, sizeof msg,
             "{\"name\":\"Ledify chip\",\"uniq_id\":\"%s_type\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"val_tpl\":\"{{ value_json.type }}\",\"avty_t\":\"%s\",\"icon\":\"mdi:chip\","
             "\"options\":[\"ws2812\",\"ws2811\",\"sk6812\",\"sk6812w\",\"ws2816\"],%s}",   // keep in step with LED_TYPES
             id, cmd, t_state, t_avail, devs);
    esp_mqtt_client_publish(mqtt, topic, msg, 0, 1, 1);

    snprintf(topic, sizeof topic, "homeassistant/switch/%s_auto/config", id);
    snprintf(cmd, sizeof cmd, "%sauto/set", prefix);
    snprintf(msg, sizeof msg,
             "{\"name\":\"Ledify auto brightness\",\"uniq_id\":\"%s_auto\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"val_tpl\":\"{{ 'ON' if value_json.auto else 'OFF' }}\",\"avty_t\":\"%s\",\"icon\":\"mdi:brightness-auto\",%s}",
             id, cmd, t_state, t_avail, devs);
    esp_mqtt_client_publish(mqtt, topic, msg, 0, 1, 1);

    // One number per saved value; the key is also the command and the status field.
    static const struct { const char *key, *name, *unit, *icon, *mode; int min, max; } nums[] = {   // keep in step with settings[] in main.c
        {"count", "pixels", "", "mdi:led-strip-variant", "slider", 1, MAX_LEDS},
        {"gain", "gain", "", "mdi:microphone", "slider", 1, 100},
        {"lux_lo", "lux low", "lx", "mdi:brightness-4", "box", 0, 10000},
        {"lux_hi", "lux high", "lx", "mdi:brightness-7", "box", 1, 20000},
        {"night", "night lux", "lx", "mdi:weather-night", "box", 0, 1000},
        {"temp_lo", "temp low", "°C", "mdi:snowflake", "slider", -40, 100},
        {"temp_hi", "temp high", "°C", "mdi:fire", "slider", -40, 100},
    };
    for (int i = 0; i < sizeof nums / sizeof nums[0]; i++) {
        snprintf(topic, sizeof topic, "homeassistant/number/%s_%s/config", id, nums[i].key);
        snprintf(cmd, sizeof cmd, "%s%s/set", prefix, nums[i].key);
        int w = snprintf(msg, sizeof msg,
                         "{\"name\":\"Ledify %s\",\"uniq_id\":\"%s_%s\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
                         "\"val_tpl\":\"{{ value_json.%s }}\",\"avty_t\":\"%s\",\"min\":%d,\"max\":%d,\"step\":1,"
                         "\"mode\":\"%s\",\"icon\":\"%s\"",
                         nums[i].name, id, nums[i].key, cmd, t_state, nums[i].key, t_avail, nums[i].min, nums[i].max,
                         nums[i].mode, nums[i].icon);
        if (nums[i].unit[0]) {
            w += snprintf(msg + w, sizeof msg - w, ",\"unit_of_meas\":\"%s\"", nums[i].unit);
        }
        snprintf(msg + w, sizeof msg - w, ",%s}", devs);
        esp_mqtt_client_publish(mqtt, topic, msg, 0, 1, 1);
    }

    struct { const char *key, *name, *unit, *cls; } sensors[] = {
        {"lux", "Illuminance", "lx", "illuminance"},
        {"temp_c", "Temperature", "°C", "temperature"},
        {"pressure_kpa", "Pressure", "kPa", "atmospheric_pressure"},
        {"sound_db", "Sound level", "dB", "sound_pressure"},
    };
    for (int i = 0; i < 4; i++) {
        snprintf(topic, sizeof topic, "homeassistant/sensor/%s_%s/config", id, sensors[i].key);
        int w = snprintf(msg, sizeof msg,
                         "{\"name\":\"Ledify %s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"avty_t\":\"%s\","
                         "\"val_tpl\":\"{{ value_json.%s }}\"",
                         sensors[i].name, id, sensors[i].key, t_sensors, t_avail, sensors[i].key);
        if (sensors[i].unit[0]) {
            w += snprintf(msg + w, sizeof msg - w, ",\"unit_of_meas\":\"%s\"", sensors[i].unit);
        }
        if (sensors[i].cls[0]) {
            w += snprintf(msg + w, sizeof msg - w, ",\"dev_cla\":\"%s\"", sensors[i].cls);
        }
        snprintf(msg + w, sizeof msg - w, ",%s}", devs);
        esp_mqtt_client_publish(mqtt, topic, msg, 0, 1, 1);
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    char key[16];
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_up = true;
        ESP_LOGI(TAG, "mqtt connected");
        esp_mqtt_client_subscribe(mqtt, t_cmd, 1);
        esp_mqtt_client_subscribe(mqtt, t_any, 1);
        esp_mqtt_client_publish(mqtt, t_avail, "online", 0, 1, 1);
        discover();
        queue_cmd("status", 6);                       // publish a first state right away
        break;
    case MQTT_EVENT_DISCONNECTED:
        mqtt_up = false;
        ESP_LOGW(TAG, "mqtt disconnected");
        break;
    case MQTT_EVENT_DATA:
        if (ev->total_data_len == ev->data_len) {    // small payloads only; we never need more
            if (is_topic(ev, t_cmd) || is_topic(ev, t_set)) {
                queue_cmd(ev->data, ev->data_len);
            } else if (set_key(ev, key, sizeof key)) {   // a slider sends a bare number, maybe "300.0"
                queue_prefixed(key, ev->data, ev->data_len);
            }
        }
        break;
    default:
        break;
    }
}

static void mqtt_start(void)
{
    if (mqtt || !mqtt_url[0]) {
        return;
    }
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = mqtt_url,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,   // used for mqtts://
        .credentials.username = mqtt_user[0] ? mqtt_user : NULL,
        .credentials.authentication.password = mqtt_pass[0] ? mqtt_pass : NULL,
        .credentials.client_id = id,
        .session.last_will = { .topic = t_avail, .msg = "offline", .qos = 1, .retain = true },
        .session.keepalive = 30,
    };
    mqtt = esp_mqtt_client_init(&mc);
    esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(mqtt);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_up = false;
        if (ssid2[0] && ++fails >= WIFI_FAILS_BEFORE_SWITCH) {   // the other network may be the one in range
            fails = 0;
            which ^= 1;
            apply_wifi(which);
        } else {
            ESP_LOGW(TAG, "wifi disconnected, retrying");
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
        wifi_up = true;
        fails = 0;
        ESP_LOGI(TAG, "wifi connected, ip %s", ip);
        mqtt_start();
    }
}

void net_init(QueueHandle_t q)
{
    cmd_q = q;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(id, sizeof id, "ledify_%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(prefix, sizeof prefix, "ledify/%s/", id);
    snprintf(t_cmd, sizeof t_cmd, "%scmd", prefix);
    snprintf(t_set, sizeof t_set, "%slight/set", prefix);
    snprintf(t_any, sizeof t_any, "%s+/set", prefix);
    snprintf(t_live, sizeof t_live, "%slive", prefix);
    snprintf(t_audio, sizeof t_audio, "%saudio", prefix);
    snprintf(t_lstate, sizeof t_lstate, "%slight/state", prefix);
    snprintf(t_state, sizeof t_state, "%sstate", prefix);
    snprintf(t_sensors, sizeof t_sensors, "%ssensors", prefix);
    snprintf(t_avail, sizeof t_avail, "%sstatus", prefix);

    load("mqtt_url", mqtt_url, sizeof mqtt_url);
    load("mqtt_user", mqtt_user, sizeof mqtt_user);
    load("mqtt_pass", mqtt_pass, sizeof mqtt_pass);
    if (!load("ssid", ssid, sizeof ssid)) {
        ESP_LOGI(TAG, "no wifi configured, send: wifi <ssid> <password>");
        return;
    }
    load("pass", pass, sizeof pass);
    if (load("ssid2", ssid2, sizeof ssid2)) {
        load("pass2", pass2, sizeof pass2);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_wifi(0);
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);          // mains powered: no modem sleep, so keepalives and commands are never late
    ESP_LOGI(TAG, "id %s, fallback network %s, mqtt %s", id, ssid2[0] ? ssid2 : "none", mqtt_url[0] ? mqtt_url : "not configured");
}

bool net_mqtt_connected(void)
{
    return mqtt_up;
}

void net_status(char *out, size_t n)
{
    if (!ssid[0]) {
        snprintf(out, n, "wifi not configured");
    } else if (!wifi_up) {
        snprintf(out, n, "wifi %s connecting", which ? ssid2 : ssid);
    } else {
        snprintf(out, n, "wifi %s ip %s, mqtt %s", which ? ssid2 : ssid, ip,
                 !mqtt_url[0] ? "not configured" : mqtt_up ? "connected" : "connecting");
    }
}

void net_publish(const char *light_json, const char *status_json, const char *sensors_json)
{
    if (!mqtt_up) {
        return;
    }
    esp_mqtt_client_publish(mqtt, t_lstate, light_json, 0, 0, 1);
    esp_mqtt_client_publish(mqtt, t_state, status_json, 0, 0, 1);
    esp_mqtt_client_publish(mqtt, t_sensors, sensors_json, 0, 0, 0);
}

void net_publish_live(const char *json)
{
    if (mqtt_up) {
        esp_mqtt_client_publish(mqtt, t_live, json, 0, 0, 0);
    }
}

// Raw microphone audio while "listen" is active: 16 kHz, 16-bit, little endian, 1 KB per message. Enqueued
// (not sent inline) so the mic task never waits on the network; the outbox just drops behind if Wi-Fi is slow.
static StreamBufferHandle_t audio_sb;

// A small task drains the stream buffer with blocking QoS 0 publishes, so the mic task never touches the socket.
static void audio_task(void *arg)
{
    static uint8_t buf[1024];
    for (;;) {
        size_t n = xStreamBufferReceive(audio_sb, buf, sizeof buf, portMAX_DELAY);
        if (n && mqtt_up) {
            esp_mqtt_client_publish(mqtt, t_audio, (const char *)buf, n, 0, 0);
        }
    }
}

void net_publish_audio(const void *pcm, int len)
{
    if (!audio_sb) {
        audio_sb = xStreamBufferCreate(8192, 1024);                 // a quarter second of audio
        xTaskCreate(audio_task, "audio", 4096, NULL, 3, NULL);
    }
    if (mqtt_up) {
        xStreamBufferSend(audio_sb, pcm, len, 0);                   // drops the chunk if Wi-Fi is behind
    }
}
