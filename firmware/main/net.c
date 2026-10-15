// Wi-Fi station and MQTT client with Home Assistant discovery.
//
// Topics (ID = last three bytes of the Wi-Fi MAC, e.g. ledify_d3ed08):
//   ledify/ID/cmd          text commands, same as BLE ("rgb 255 0 0", "mode rainbow", ...)
//   ledify/ID/light/set    Home Assistant JSON light commands
//   ledify/ID/light/state  JSON light state
//   ledify/ID/state        full status JSON
//   ledify/ID/sensors      {"lux":..,"temp_c":..,"pressure_hpa":..,"sound":..}
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
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cmd.h"

static const char *TAG = "net";
static QueueHandle_t cmd_q;
static char ssid[33], pass[65], mqtt_url[128], mqtt_user[48], mqtt_pass[64];
static char id[16];                       // ledify_xxxxxx
static char ip[16];
static bool wifi_up, mqtt_up;
static esp_mqtt_client_handle_t mqtt;
static char t_cmd[48], t_set[48], t_lstate[48], t_state[48], t_sensors[48], t_avail[48];

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

// One retained discovery message per entity so Home Assistant creates them on its own.
static void discover(void)
{
    char topic[96], msg[640];
    const char *dev = "\"dev\":{\"ids\":[\"%s\"],\"name\":\"Ledify\",\"mf\":\"notaroomba\",\"mdl\":\"Ledify rev1\"}";
    char devs[160];
    snprintf(devs, sizeof devs, dev, id);

    snprintf(topic, sizeof topic, "homeassistant/light/%s/config", id);
    snprintf(msg, sizeof msg,
             "{\"name\":\"Ledify\",\"uniq_id\":\"%s_light\",\"schema\":\"json\",\"cmd_t\":\"%s\",\"stat_t\":\"%s\","
             "\"avty_t\":\"%s\",\"brightness\":true,\"supported_color_modes\":[\"rgb\"],\"effect\":true,"
             "\"effect_list\":[\"solid\",\"rainbow\",\"sound\",\"auto\"],%s}",
             id, t_set, t_lstate, t_avail, devs);
    esp_mqtt_client_publish(mqtt, topic, msg, 0, 1, 1);

    struct { const char *key, *name, *unit, *cls; } sensors[] = {
        {"lux", "Illuminance", "lx", "illuminance"},
        {"temp_c", "Temperature", "°C", "temperature"},
        {"pressure_hpa", "Pressure", "hPa", "atmospheric_pressure"},
        {"sound", "Sound level", "", ""},
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
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_up = true;
        ESP_LOGI(TAG, "mqtt connected");
        esp_mqtt_client_subscribe(mqtt, t_cmd, 1);
        esp_mqtt_client_subscribe(mqtt, t_set, 1);
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
            queue_cmd(ev->data, ev->data_len);
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
        ESP_LOGW(TAG, "wifi disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
        wifi_up = true;
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
    snprintf(t_cmd, sizeof t_cmd, "ledify/%s/cmd", id);
    snprintf(t_set, sizeof t_set, "ledify/%s/light/set", id);
    snprintf(t_lstate, sizeof t_lstate, "ledify/%s/light/state", id);
    snprintf(t_state, sizeof t_state, "ledify/%s/state", id);
    snprintf(t_sensors, sizeof t_sensors, "ledify/%s/sensors", id);
    snprintf(t_avail, sizeof t_avail, "ledify/%s/status", id);

    load("mqtt_url", mqtt_url, sizeof mqtt_url);
    load("mqtt_user", mqtt_user, sizeof mqtt_user);
    load("mqtt_pass", mqtt_pass, sizeof mqtt_pass);
    if (!load("ssid", ssid, sizeof ssid)) {
        ESP_LOGI(TAG, "no wifi configured, send: wifi <ssid> <password>");
        return;
    }
    load("pass", pass, sizeof pass);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, NULL);

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid - 1);
    strncpy((char *)wc.sta.password, pass, sizeof wc.sta.password - 1);
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi: joining \"%s\" as %s, mqtt %s", ssid, id, mqtt_url[0] ? mqtt_url : "not configured");
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
        snprintf(out, n, "wifi \"%s\" connecting", ssid);
    } else {
        snprintf(out, n, "wifi \"%s\" ip %s, mqtt %s", ssid, ip,
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
