// Ledify firmware: USB-PD sink, LED strip, sensors, microphone, driven over BLE and MQTT.
//
// Text commands (USB serial console, BLE UART app, or MQTT topic ledify/<id>/cmd):
//   on | off | rgb R G B | level N | count N | type ws2812|ws2811|sk6812|sk6812w|ws2816
//   mode solid|rainbow|sound|auto
//   volt 5 | volt 12 | status | pd | net | help
//   wifi SSID PASSWORD | wifi2 SSID PASSWORD | mqtt URL [USER PASSWORD]   (saved to NVS, then the board restarts)
// Home Assistant talks JSON to ledify/<id>/light/set; see net.c for the topics.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "board.h"
#include "cmd.h"
#include "leds.h"
#include "pd.h"
#include "sensors.h"
#include "mic.h"
#include "ble.h"
#include "net.h"

static const char *TAG = "ledify";

#define SOUND_GAIN 25.0f      // ponytail: fixed gain, make it a command if rooms differ a lot

typedef enum { MODE_SOLID, MODE_RAINBOW, MODE_SOUND, MODE_AUTO } led_mode_t;
static const char *mode_names[] = {"solid", "rainbow", "sound", "auto"};

static led_mode_t mode = MODE_SOLID;
static bool power;
static int volt = 5;
static bool have_mic;
static QueueHandle_t cmd_q;

// Strip length and chip type live in NVS next to the Wi-Fi credentials (namespace "cfg").
static void save_count(uint16_t n)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "count");            // older images stored it as u8
        nvs_set_u16(h, "count", n);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void save_type(const char *t)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "type", t);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void load_cfg(void)
{
    nvs_handle_t h;
    uint16_t n;
    char t[16];
    size_t len = sizeof t;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "type", t, &len) == ESP_OK) {
            leds_set_type(t);
        }
        if (nvs_get_u16(h, "count", &n) == ESP_OK) {
            leds_set_count(n);
        }
        nvs_close(h);
    }
}

// Commands typed on the USB serial console (idf.py monitor). Without the USB-Serial-JTAG driver installed,
// read() on stdin returns one byte when the 64-byte FIFO has one and EWOULDBLOCK otherwise, so just poll.
// (Don't set O_NONBLOCK: in that mode the console VFS only looks at the driver's buffer, which doesn't exist.)
static void console_task(void *arg)
{
    cmd_t cmd;
    int len = 0;
    for (;;) {
        char c;
        if (read(0, &c, 1) != 1) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (c == '\n' || c == '\r') {
            if (len) {
                cmd.text[len] = 0;
                xQueueSend(cmd_q, &cmd, 0);
                len = 0;
            }
        } else if (len < (int)sizeof cmd.text - 1) {
            cmd.text[len++] = c;
        }
    }
}

// Power the strip may draw: the PD contract minus buck losses, never more than the buck can deliver.
static uint32_t power_budget_mw(void)
{
    const pd_state_t *pd = pd_get();
    uint32_t in = pd->ready ? (uint32_t)pd->contract_mv * pd->contract_ma / 1000 * LED_BUCK_EFF_PCT / 100
                            : LED_BUDGET_NO_PD_MW;
    uint32_t out = (uint32_t)volt * LED_BUCK_MAX_MA;
    return in < out ? in : out;
}

static void reply(const char *s)
{
    ESP_LOGI(TAG, "> %s", s);
    ble_send(s);
}

static bool set_mode(const char *name)
{
    for (int i = 0; i < 4; i++) {
        if (!strcasecmp(name, mode_names[i])) {
            if (i == MODE_SOUND && !have_mic) {
                return false;
            }
            mode = i;
            return true;
        }
    }
    return false;
}

static void status(char *out, size_t n)
{
    sensor_data_t d;
    sensors_read(&d);
    const pd_state_t *pd = pd_get();
    char net[96];
    net_status(net, sizeof net);
    int w = snprintf(out, n, "{\"power\":%d,\"volt\":%d,\"mode\":\"%s\",\"level\":%d,\"count\":%d,\"type\":\"%s\",\"cap\":%d,"
                     "\"pd\":{\"attached\":%d,\"ready\":%d,\"mv\":%d,\"ma\":%d},\"net\":\"%s\"",
                     power, volt, mode_names[mode], leds_get_level(), leds_get_count(), leds_type_name(), leds_get_cap(),
                     pd->cc != 0, pd->ready, pd->contract_mv, pd->contract_ma, net);
    if (d.have_lux) {
        w += snprintf(out + w, n - w, ",\"lux\":%.1f", d.lux);
    }
    if (d.have_baro) {
        w += snprintf(out + w, n - w, ",\"temp_c\":%.2f,\"pressure_pa\":%.0f", d.temp_c, d.pressure_pa);
    }
    if (d.have_imu) {
        w += snprintf(out + w, n - w, ",\"acc\":[%.2f,%.2f,%.2f],\"gyr\":[%.1f,%.1f,%.1f]",
                      d.acc[0], d.acc[1], d.acc[2], d.gyr[0], d.gyr[1], d.gyr[2]);
    }
    if (have_mic) {
        w += snprintf(out + w, n - w, ",\"sound_db\":%.1f", mic_db());
    }
    snprintf(out + w, n - w, "}");
}

// Push the current state to MQTT (no-op until the broker is connected).
static void publish(void)
{
    if (!net_mqtt_connected()) {
        return;
    }
    char light[160], st[400], sens[160];
    uint8_t r, g, b;
    leds_get_color(&r, &g, &b);
    snprintf(light, sizeof light,
             "{\"state\":\"%s\",\"brightness\":%d,\"color_mode\":\"rgb\",\"color\":{\"r\":%d,\"g\":%d,\"b\":%d},\"effect\":\"%s\"}",
             power ? "ON" : "OFF", leds_get_level(), r, g, b, mode_names[mode]);
    status(st, sizeof st);
    sensor_data_t d;
    sensors_read(&d);
    snprintf(sens, sizeof sens, "{\"lux\":%.1f,\"temp_c\":%.2f,\"pressure_kpa\":%.2f,\"sound_db\":%.1f}",
             d.lux, d.temp_c, d.pressure_pa / 1000.0f, have_mic ? mic_db() : 0.0f);
    net_publish(light, st, sens);
}

// Home Assistant JSON light schema: {"state":"ON","brightness":128,"color":{"r":..},"effect":"rainbow"}
// The format is fixed and small, so a few string searches beat pulling in a JSON library.
static bool json_num(const char *text, const char *key, int *out)
{
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(text, pat);
    return p && sscanf(p + strlen(pat), " %d", out) == 1;
}

static bool json_str(const char *text, const char *key, char *out, size_t n)
{
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(text, pat);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pat), '"');
    if (!p) {
        return false;
    }
    const char *e = strchr(++p, '"');
    if (!e || (size_t)(e - p) >= n) {
        return false;
    }
    memcpy(out, p, e - p);
    out[e - p] = 0;
    return true;
}

static void handle_json(const char *text)
{
    char str[16];
    int v, r, g, b;
    if (json_str(text, "effect", str, sizeof str)) {
        set_mode(str);
    }
    if (json_num(text, "brightness", &v)) {
        leds_set_level(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    const char *color = strstr(text, "\"color\":");
    if (color && json_num(color, "r", &r) && json_num(color, "g", &g) && json_num(color, "b", &b)) {
        if (mode == MODE_RAINBOW) {
            mode = MODE_SOLID;
        }
        leds_fill(r & 255, g & 255, b & 255);
    }
    if (json_str(text, "state", str, sizeof str)) {
        power = !strcasecmp(str, "ON");
        leds_power(power);
    }
}

static void handle(char *line)
{
    while (*line == ' ') {
        line++;
    }
    if (*line == '{') {
        handle_json(line);
        return;
    }
    char *tok[5] = {0};
    int n = 0;
    char *save;
    for (char *t = strtok_r(line, " \r\n", &save); t && n < 5; t = strtok_r(NULL, " \r\n", &save)) {
        tok[n++] = t;
    }
    if (!n) {
        return;
    }
    for (char *p = tok[0]; *p; p++) {
        *p = tolower((unsigned char)*p);
    }
    bool keep_case = !strcmp(tok[0], "wifi") || !strcmp(tok[0], "wifi2") || !strcmp(tok[0], "mqtt");   // credentials are case sensitive
    for (int i = 1; i < n && !keep_case; i++) {
        for (char *p = tok[i]; *p; p++) {
            *p = tolower((unsigned char)*p);
        }
    }
    char out[400];

    if (!strcmp(tok[0], "on") && n == 1) {
        power = true;
        leds_power(true);
        reply("ok");
    } else if (!strcmp(tok[0], "off") && n == 1) {
        power = false;
        leds_power(false);
        reply("ok");
    } else if (!strcmp(tok[0], "rgb") && n == 4) {
        int r = atoi(tok[1]), g = atoi(tok[2]), b = atoi(tok[3]);
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
            reply("err rgb 0-255");
            return;
        }
        if (mode == MODE_RAINBOW) {
            mode = MODE_SOLID;
        }
        leds_fill(r, g, b);
        reply("ok");
    } else if (!strcmp(tok[0], "level") && n == 2) {
        int l = atoi(tok[1]);
        if (l < 0 || l > 255) {
            reply("err level 0-255");
            return;
        }
        leds_set_level(l);
        reply("ok");
    } else if (!strcmp(tok[0], "count") && n == 2) {
        int c = atoi(tok[1]);
        if (c < 1 || c > MAX_LEDS) {
            snprintf(out, sizeof out, "err count 1-%d", MAX_LEDS);
            reply(out);
            return;
        }
        leds_set_count(c);
        save_count(c);
        reply("ok");
    } else if (!strcmp(tok[0], "type") && n == 2) {
        if (!leds_set_type(tok[1])) {
            reply("err type " LED_TYPES);
            return;
        }
        save_type(tok[1]);
        reply("ok");
    } else if (!strcmp(tok[0], "mode") && n == 2) {
        reply(set_mode(tok[1]) ? "ok" : "err mode solid|rainbow|sound|auto");
    } else if (!strcmp(tok[0], "volt") && n == 2) {
        int v = atoi(tok[1]);
        if (v != 5 && v != 12) {
            reply("err volt 5|12");
            return;
        }
        const pd_state_t *pd = pd_get();
        if (v == 12 && !(pd->ready && pd->contract_mv >= 15000)) {
            reply("err 12V needs a 15V+ PD contract");
            return;
        }
        power = false;
        leds_select_12v(v == 12);
        volt = v;
        reply("ok, output is off, send on");
    } else if (!strcmp(tok[0], "status")) {
        status(out, sizeof out);
        reply(out);
    } else if (!strcmp(tok[0], "pd")) {
        const pd_state_t *pd = pd_get();
        int w = snprintf(out, sizeof out, "cc%d ready=%d contract=%dmV/%dmA pdos:", pd->cc, pd->ready,
                         pd->contract_mv, pd->contract_ma);
        for (int i = 0; i < pd->n_pdos; i++) {
            w += snprintf(out + w, sizeof out - w, " %dmV/%dmA", pd->pdo_mv[i], pd->pdo_ma[i]);
        }
        reply(out);
    } else if (!strcmp(tok[0], "net")) {
        net_status(out, sizeof out);
        reply(out);
    } else if (!strcmp(tok[0], "wifi") && n == 3) {
        reply(net_save_wifi(tok[1], tok[2]) ? "ok, restarting" : "err nvs");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (!strcmp(tok[0], "wifi2") && (n == 2 || n == 3)) {   // "wifi2 off" clears the fallback
        reply(net_save_wifi2(n == 3 ? tok[1] : "", n == 3 ? tok[2] : "") ? "ok, restarting" : "err nvs");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (!strcmp(tok[0], "mqtt") && (n == 2 || n == 4)) {
        reply(net_save_mqtt(tok[1], n == 4 ? tok[2] : NULL, n == 4 ? tok[3] : NULL) ? "ok, restarting" : "err nvs");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (!strcmp(tok[0], "help")) {
        reply("on off rgb<r g b> level<n> count<n> type<" LED_TYPES "> mode<solid|rainbow|sound|auto> volt<5|12> status pd net "
              "wifi<ssid pass> wifi2<ssid pass|off> mqtt<url [user pass]>");
    } else {
        reply("err, send help");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "app start");
    esp_err_t err = nvs_flash_init();
    ESP_LOGI(TAG, "nvs %s", esp_err_to_name(err));
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_PD_INT, .mode = GPIO_MODE_INPUT };
    gpio_config(&io);                        // release UART0 TX so the FUSB302 can drive INT

    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));
    ESP_LOGI(TAG, "i2c up");

    leds_init();
    load_cfg();
    ESP_LOGI(TAG, "leds up, %d x %s", leds_get_count(), leds_type_name());
    vTaskDelay(pdMS_TO_TICKS(PD_START_DELAY_MS));
    pd_init(bus);
    sensors_init(bus);
    have_mic = mic_init();
    ESP_LOGI(TAG, "microphone %d", have_mic);

    cmd_q = xQueueCreate(8, sizeof(cmd_t));
    xTaskCreate(console_task, "console", 3072, NULL, 3, NULL);
    ble_init(cmd_q);
    net_init(cmd_q);

    char snap[400];
    status(snap, sizeof snap);               // one snapshot in the log so sensors can be checked without BLE
    ESP_LOGI(TAG, "%s", snap);

    int64_t last_anim = 0, last_lux = 0, last_beat = 0, last_pub = 0, last_frame = 0;
    cmd_t cmd;
    for (;;) {
        leds_set_budget_mw(power_budget_mw());
        if (xQueueReceive(cmd_q, &cmd, 0)) {
            handle(cmd.text);
            publish();
        }
        int64_t now = esp_timer_get_time();
        if (now - last_beat > 3000000) {
            const pd_state_t *pd = pd_get();
            char net[96];
            net_status(net, sizeof net);
            ESP_LOGI(TAG, "up %llds, pd cc%d ready=%d %dmV/%dmA, ble %s, %s, power %d, mode %s",
                     now / 1000000, pd->cc, pd->ready, pd->contract_mv, pd->contract_ma,
                     ble_connected() ? "connected" : "advertising", net, power, mode_names[mode]);
            last_beat = now;
        }
        if (now - last_pub > 30000000) {
            publish();
            last_pub = now;
        }
        if (power && mode == MODE_SOLID && now - last_frame > 1000000) {
            leds_refresh();                             // pixels only latch while powered, so resend once a second
            last_frame = now;
        }
        if (power && mode == MODE_RAINBOW && now - last_anim > 20000) {
            leds_rainbow_step();
            last_anim = now;
        } else if (power && mode == MODE_SOUND) {
            leds_scaled(mic_level() * SOUND_GAIN);      // reads 8 ms of audio
        } else if (mode == MODE_AUTO && now - last_lux > 500000) {
            sensor_data_t d;
            sensors_read(&d);
            if (d.have_lux) {
                float l = 20 + d.lux / 2;               // 0 lux -> 20, 470 lux and up -> 255
                leds_set_level(l > 255 ? 255 : (uint8_t)l);
            }
            last_lux = now;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
