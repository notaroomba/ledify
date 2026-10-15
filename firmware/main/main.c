// Ledify firmware: USB-PD sink, LED strip, sensors, microphone, driven over BLE and MQTT.
//
// Text commands (USB serial console, BLE UART app, or MQTT topic ledify/<id>/cmd):
//   on | off | rgb R G B | level N | count N | type ws2812|ws2811|sk6812|sk6812w|ws2816
//   mode solid|rainbow|temp|spectrum|meter|sound|vu|pulse|auto     presets, the Home Assistant effects
//   color solid|rainbow|temp|spectrum|meter | bright level|sound | overlay none|vu|pulse | auto on|off
//   gain N | lux LO HI | night N | temp LO HI      saved tunables (or one at a time: gain N, lux_lo N, ...)
//   temp 21.5                                      feed a temperature in for the temp colour
//   live N (stream loudness/bands N s) | audio L B M T P (feed from the panel) | volt 5 | volt 12 yes | status | pd | net | help
//   wifi SSID PASSWORD | wifi2 SSID PASSWORD | mqtt URL [USER PASSWORD]   (saved to NVS, then the board restarts)
// Home Assistant talks JSON to ledify/<id>/light/set; see net.c for the topics.
//
// A frame is colour x brightness x overlay, drawn by leds_frame():
//   colour      solid | rainbow | temp (blue-white-red from the temperature) | spectrum (bass/mid/treble bars) | meter
//   brightness  level, scaled by the lux curve when auto is on, by the microphone when bright is sound
//   overlay     none | vu (lit length follows the sound) | pulse (flash on each beat, a quarter in between)
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
#include "ota.h"
#include "esp_app_desc.h"

static const char *TAG = "ledify";

#define AUTO_FLOOR 20          // brightness at lux_lo and below when auto is on
#define PULSE_FLOOR 64         // brightness between beats, out of 255

typedef enum { OVER_NONE, OVER_VU, OVER_PULSE } overlay_t;
static const char *color_names[] = {"solid", "rainbow", "temp", "spectrum", "meter", "palette"};
static const char *overlay_names[] = {"none", "vu", "pulse"};
#define MODES "solid|rainbow|temp|spectrum|meter|palette|sound|vu|pulse|auto"

static leds_color_t color;
static overlay_t overlay;
static bool sound_on, auto_on, dark;        // dark: under the night threshold
static int lux = -1;                        // latest reading, -1 without a sensor
static float temp_in;                       // fed in over MQTT; the barometer's until then. ponytail: never expires, add a timeout if Home Assistant going away should fall back
static bool have_temp_in;
static int64_t live_until;                  // "live N": stream loudness/bands to MQTT until then
static uint32_t boots;                      // boot counter and a log of the last four boots (reset reason, PD result,
static char boot_log[4][96];                // whether the boot stayed up), in NVS so a trip to a charger with no
static bool up_saved, safe_boot;  // Wi-Fi can still be read afterwards

static const char *reset_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "wdt";
    case ESP_RST_USB: return "usb";
    default: return "other";
    }
}

static void write_log0(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "l0", boot_log[0]);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Shift the log, start this boot's entry. Two previous boots that never reached 15 s means something about the
// supply kills us after negotiation: stay at 5 V this time so the board at least comes up and can be asked.
static void boot_bookkeeping(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_get_u32(h, "boots", &boots);
    boots++;
    nvs_set_u32(h, "boots", boots);
    const char *keys[4] = {"l0", "l1", "l2", "l3"};
    for (int i = 3; i >= 1; i--) {
        size_t len = sizeof boot_log[i];
        if (nvs_get_str(h, keys[i - 1], boot_log[i], &len) != ESP_OK) {
            boot_log[i][0] = 0;
        }
        nvs_set_str(h, keys[i], boot_log[i]);
    }
    safe_boot = boot_log[1][0] && boot_log[2][0] && !strstr(boot_log[1], " ok") && !strstr(boot_log[2], " ok");
    snprintf(boot_log[0], sizeof boot_log[0], "#%lu %s%s", (unsigned long)boots, reset_name(), safe_boot ? " safe5v" : "");
    nvs_set_str(h, "l0", boot_log[0]);
    nvs_commit(h);
    nvs_close(h);
    if (safe_boot) {
        pd_set_max_mv(5000);
    }
}

static int pd_phase_seen;

static void save_pd_step(int phase)
{
    const pd_state_t *pd = pd_get();
    int w = strlen(boot_log[0]);
    if (phase == 1) {
        w += snprintf(boot_log[0] + w, sizeof boot_log[0] - w, " caps");
        for (int i = 0; i < pd->n_pdos && w < (int)sizeof boot_log[0] - 14; i++) {
            w += snprintf(boot_log[0] + w, sizeof boot_log[0] - w, " %d/%d", pd->pdo_mv[i] / 100, pd->pdo_ma[i] / 100);
        }
    } else if (phase == 2) {
        snprintf(boot_log[0] + w, sizeof boot_log[0] - w, " req %d/%d", pd->req_mv / 100, pd->req_ma / 100);
    } else if (phase == 3) {
        snprintf(boot_log[0] + w, sizeof boot_log[0] - w, " got %d/%d", pd->contract_mv / 100, pd->contract_ma / 100);
    } else if (phase == 4) {
        snprintf(boot_log[0] + w, sizeof boot_log[0] - w, " rej");
    } else if (phase == 5) {
        snprintf(boot_log[0] + w, sizeof boot_log[0] - w, " HARDRST");
    }
    write_log0();
}
static bool power;
static int volt = 5;
static bool have_mic;
static QueueHandle_t cmd_q;

// Saved tunables. The key is the command, the Home Assistant topic ledify/<id>/<key>/set and the status field.
static int16_t gain = 25, lux_lo = 0, lux_hi = 500, night = 0, temp_lo = 15, temp_hi = 30;
static int16_t limit;                       // see settings[]
static int16_t pdmax = 20000;
static int16_t pddelay = PD_START_DELAY_MS;
static int64_t dirty_at;                    // settings changed by a slider: written to NVS once they settle
enum { S_GAIN, S_LUX_LO, S_LUX_HI, S_NIGHT, S_TEMP_LO, S_TEMP_HI, S_LIMIT, S_PDMAX, S_PDDELAY, N_SETTINGS };
static const struct { const char *key; int16_t *val; int16_t lo, hi; } settings[N_SETTINGS] = {   // keep in step with nums[] in net.c
    {"gain", &gain, 1, 100},
    {"lux_lo", &lux_lo, 0, 10000},
    {"lux_hi", &lux_hi, 1, 20000},
    {"night", &night, 0, 1000},
    {"temp_lo", &temp_lo, -40, 100},
    {"temp_hi", &temp_hi, -40, 100},
    {"limit", &limit, 0, 1},                 // 1 = dim to the power budget, 0 = send what was asked and warn
    {"pdmax", &pdmax, 5000, 20000},          // highest PD voltage to request, in mV
    {"pddelay", &pddelay, 0, 30000},         // ms after boot before the board talks PD (experiments with picky chargers)
};

// Strip length, chip type and the tunables live in NVS next to the Wi-Fi credentials (namespace "cfg").
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

static void save_i16(const char *key, int16_t v)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i16(h, key, v);
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
        for (int i = 0; i < N_SETTINGS; i++) {
            nvs_get_i16(h, settings[i].key, settings[i].val);
        }
        nvs_close(h);
    }
    mic_set_gain(gain);
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

static int lookup(const char *name, const char *const *names, int n)
{
    for (int i = 0; i < n; i++) {
        if (!strcasecmp(name, names[i])) {
            return i;
        }
    }
    return -1;
}

static bool uses_mic(void)
{
    return sound_on || overlay != OVER_NONE || color == COLOR_SPECTRUM;
}

static const char *mic_note(void)
{
    return uses_mic() && !have_mic ? "ok, no mic" : "ok";
}

// What Home Assistant sees as the effect: the overlay if there is one, else sound, else the colour source.
static const char *effect_name(void)
{
    return overlay != OVER_NONE ? overlay_names[overlay] : sound_on ? "sound" : color_names[color];
}

// Presets: a colour resets the rest, vu brings its meter colours, sound and pulse keep whatever colour is
// showing. auto is the old name for the lux modifier.
static bool set_mode(const char *name)
{
    int c = lookup(name, color_names, 6);
    if (c >= 0) {
        color = c;
        sound_on = false;
        overlay = OVER_NONE;
    } else if (!strcasecmp(name, "sound")) {
        sound_on = true;
        overlay = OVER_NONE;
    } else if (!strcasecmp(name, "vu")) {
        color = COLOR_METER;
        overlay = OVER_VU;
        sound_on = false;
    } else if (!strcasecmp(name, "pulse")) {
        overlay = OVER_PULSE;
        sound_on = false;
    } else if (!strcasecmp(name, "auto")) {
        auto_on = true;
    } else {
        return false;
    }
    return true;
}

static int setting_idx(const char *key)
{
    for (int i = 0; i < N_SETTINGS; i++) {
        if (!strcmp(key, settings[i].key)) {
            return i;
        }
    }
    return -1;
}

static bool set_setting(int i, int v)
{
    if (v < settings[i].lo || v > settings[i].hi) {
        return false;
    }
    *settings[i].val = v;
    dirty_at = esp_timer_get_time();
    mic_set_gain(gain);
    leds_set_limit(limit);
    if (i == S_PDMAX) {
        pd_set_max_mv(pdmax);
        pd_request(0);                       // renegotiate under the new ceiling
    }
    return true;
}

// Auto brightness: AUTO_FLOOR at lux_lo and below, lvl at lux_hi and above, off under the night threshold
// (back on above twice it, so the strip's own light can't toggle it).
static uint8_t lux_level(uint8_t lvl)
{
    if (lux < 0) {
        return lvl;
    }
    dark = night && (lux < night || (dark && lux < 2 * night));
    if (dark) {
        return 0;
    }
    int f = lux_hi > lux_lo ? (lux - lux_lo) * 255 / (lux_hi - lux_lo) : 255;
    f = f < 0 ? 0 : f > 255 ? 255 : f;
    int floor = lvl < AUTO_FLOOR ? lvl : AUTO_FLOOR;
    return floor + (lvl - floor) * f / 255;
}

// Lux for the auto curve and the temperature behind the temp colour.
static void sense(void)
{
    sensor_data_t d;
    sensors_read(&d);
    lux = d.have_lux ? (int)d.lux : -1;
    float t = have_temp_in ? temp_in : d.have_baro ? d.temp_c : (temp_lo + temp_hi) / 2.0f;
    int pos = temp_hi > temp_lo ? (int)((t - temp_lo) * 255 / (temp_hi - temp_lo)) : 128;
    leds_set_temp(pos < 0 ? 0 : pos > 255 ? 255 : pos);
}

// Compose and send one frame.
static void frame(void)
{
    if (!power) {
        return;
    }
    uint8_t bands[3];
    mic_bands(bands);
    int lvl = leds_get_level();
    if (auto_on) {
        lvl = lux_level(lvl);
    }
    if (sound_on) {
        lvl = lvl * mic_loud() / 255;
    }
    if (overlay == OVER_PULSE) {
        lvl = lvl * (PULSE_FLOOR + (255 - PULSE_FLOOR) * mic_pulse() / 255) / 255;
    }
    int count = leds_get_count();
    leds_frame(color, lvl, overlay == OVER_VU ? count * mic_loud() / 255 : count, bands);
}

// Styles that move redraw every FRAME_MS; a still frame only needs the once-a-second resend.
static bool live(void)
{
    return color == COLOR_RAINBOW || uses_mic();
}

static void status(char *out, size_t n)
{
    sensor_data_t d;
    sensors_read(&d);
    const pd_state_t *pd = pd_get();
    char net[96];
    net_status(net, sizeof net);
    int w = snprintf(out, n, "{\"fw\":\"%s\",\"power\":%d,\"volt\":%d,\"mode\":\"%s\",\"color\":\"%s\",\"bright\":\"%s\",\"overlay\":\"%s\","
                     "\"auto\":%d,\"level\":%d,\"count\":%d,\"type\":\"%s\",\"cap\":%d",
                     esp_app_get_description()->version, power, volt, effect_name(), color_names[color], sound_on ? "sound" : "level", overlay_names[overlay],
                     auto_on, leds_get_level(), leds_get_count(), leds_type_name(), leds_get_cap());
    for (int i = 0; i < N_SETTINGS; i++) {
        w += snprintf(out + w, n - w, ",\"%s\":%d", settings[i].key, *settings[i].val);
    }
    w += snprintf(out + w, n - w, ",\"pd\":{\"attached\":%d,\"ready\":%d,\"mv\":%d,\"ma\":%d},\"net\":\"%s\"",
                  pd->cc != 0, pd->ready, pd->contract_mv, pd->contract_ma, net);
    w += snprintf(out + w, n - w, ",\"pdos\":[");
    for (int i = 0; i < pd->n_pdos; i++) {
        w += snprintf(out + w, n - w, "%s[%d,%d]", i ? "," : "", pd->pdo_mv[i], pd->pdo_ma[i]);
    }
    w += snprintf(out + w, n - w, "],\"need_mw\":%lu,\"budget_mw\":%lu",
                  (unsigned long)leds_get_need_mw(), (unsigned long)leds_get_budget_mw());
    w += snprintf(out + w, n - w, ",\"boots\":%lu,\"rst\":\"%s\",\"pdmax\":%d,\"boot_log\":[\"%s\",\"%s\",\"%s\",\"%s\"]",
                  (unsigned long)boots, reset_name(), pd_get_max_mv(), boot_log[0], boot_log[1], boot_log[2], boot_log[3]);
    if (d.have_lux) {
        w += snprintf(out + w, n - w, ",\"lux\":%.1f", d.lux);
    }
    if (d.have_baro) {
        w += snprintf(out + w, n - w, ",\"temp_c\":%.2f,\"pressure_pa\":%.0f", d.temp_c, d.pressure_pa);
    }
    if (have_temp_in) {
        w += snprintf(out + w, n - w, ",\"temp_in\":%.2f", temp_in);
    }
    if (d.have_imu) {
        w += snprintf(out + w, n - w, ",\"acc\":[%.2f,%.2f,%.2f],\"gyr\":[%.1f,%.1f,%.1f]",
                      d.acc[0], d.acc[1], d.acc[2], d.gyr[0], d.gyr[1], d.gyr[2]);
    }
    if (have_mic) {
        w += snprintf(out + w, n - w, ",\"sound_db\":%.1f,\"db_peak\":%.0f,\"loud\":%d", mic_db(), mic_db_peak(), mic_loud());
    }
    snprintf(out + w, n - w, "}");
}

// Push the current state to MQTT (no-op until the broker is connected).
static void publish(void)
{
    if (!net_mqtt_connected()) {
        return;
    }
    static char light[160], st[768], sens[160];
    uint8_t r, g, b;
    leds_get_color(&r, &g, &b);
    snprintf(light, sizeof light,
             "{\"state\":\"%s\",\"brightness\":%d,\"color_mode\":\"rgb\",\"color\":{\"r\":%d,\"g\":%d,\"b\":%d},\"effect\":\"%s\"}",
             power ? "ON" : "OFF", leds_get_level(), r, g, b, effect_name());
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
    const char *c = strstr(text, "\"color\":");
    if (c && json_num(c, "r", &r) && json_num(c, "g", &g) && json_num(c, "b", &b)) {
        color = COLOR_SOLID;                  // a picked colour means show that colour
        leds_fill(r & 255, g & 255, b & 255);
    }
    if (json_str(text, "effect", str, sizeof str)) {
        set_mode(str);
    }
    if (json_num(text, "brightness", &v)) {
        leds_set_level(v < 0 ? 0 : v > 255 ? 255 : v);
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
    char *tok[8] = {0};
    int n = 0;
    char *save;
    for (char *t = strtok_r(line, " \r\n", &save); t && n < 8; t = strtok_r(NULL, " \r\n", &save)) {
        tok[n++] = t;
    }
    if (!n) {
        return;
    }
    for (char *p = tok[0]; *p; p++) {
        *p = tolower((unsigned char)*p);
    }
    bool keep_case = !strcmp(tok[0], "wifi") || !strcmp(tok[0], "wifi2") || !strcmp(tok[0], "mqtt") || !strcmp(tok[0], "ota");   // credentials are case sensitive
    for (int i = 1; i < n && !keep_case; i++) {
        for (char *p = tok[i]; *p; p++) {
            *p = tolower((unsigned char)*p);
        }
    }
    static char out[768];
    int s = setting_idx(tok[0]);

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
        color = COLOR_SOLID;
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
        dirty_at = esp_timer_get_time();
        reply("ok");
    } else if (!strcmp(tok[0], "type") && n == 2) {
        if (!leds_set_type(tok[1])) {
            reply("err type " LED_TYPES);
            return;
        }
        save_type(tok[1]);
        reply("ok");
    } else if (!strcmp(tok[0], "mode") && n == 2) {
        reply(set_mode(tok[1]) ? mic_note() : "err mode " MODES);
    } else if (!strcmp(tok[0], "color") && n == 2) {
        int c = lookup(tok[1], color_names, 6);
        if (c < 0) {
            reply("err color " LED_COLORS);
            return;
        }
        color = c;
        reply(mic_note());
    } else if (!strcmp(tok[0], "bright") && n == 2) {
        if (strcmp(tok[1], "level") && strcmp(tok[1], "sound")) {
            reply("err bright level|sound");
            return;
        }
        sound_on = !strcmp(tok[1], "sound");
        reply(mic_note());
    } else if (!strcmp(tok[0], "overlay") && n == 2) {
        int o = lookup(tok[1], overlay_names, 3);
        if (o < 0) {
            reply("err overlay none|vu|pulse");
            return;
        }
        overlay = o;
        reply(mic_note());
    } else if (!strcmp(tok[0], "auto") && n == 2) {
        if (strcmp(tok[1], "on") && strcmp(tok[1], "off")) {
            reply("err auto on|off");
            return;
        }
        auto_on = !strcmp(tok[1], "on");
        reply("ok");
    } else if (!strcmp(tok[0], "lux") && n == 3) {
        reply(set_setting(S_LUX_LO, atoi(tok[1])) && set_setting(S_LUX_HI, atoi(tok[2])) ? "ok" : "err lux 0-10000 1-20000");
    } else if (!strcmp(tok[0], "temp") && n == 3) {
        reply(set_setting(S_TEMP_LO, atoi(tok[1])) && set_setting(S_TEMP_HI, atoi(tok[2])) ? "ok" : "err temp -40-100 -40-100");
    } else if (!strcmp(tok[0], "temp") && n == 2) {
        char *end;
        float t = strtof(tok[1], &end);
        if (*end) {
            reply("err temp C");
            return;
        }
        temp_in = t;
        have_temp_in = true;
        sense();
        reply("ok");
    } else if (s >= 0 && n == 2) {
        snprintf(out, sizeof out, "err %s %d-%d", tok[0], settings[s].lo, settings[s].hi);
        reply(set_setting(s, atoi(tok[1])) ? "ok" : out);
    } else if (!strcmp(tok[0], "volt") && (n == 2 || n == 3)) {
        int v = atoi(tok[1]);
        if (v != 5 && v != 12) {
            reply("err volt 5|12");
            return;
        }
        if (v == 12 && !(n == 3 && !strcmp(tok[2], "yes"))) {    // 12 V kills 5 V pixels: every channel must say so explicitly
            reply("12 V destroys 5 V strips. If the strip really is 12 V send: volt 12 yes");
            return;
        }
        const pd_state_t *pd = pd_get();
        if (v == 12 && !(pd->ready && pd->contract_mv >= 15000)) {
            if (!pd_request(15000)) {                         // the charger offers nothing at 15 V or more
                reply("err 12V needs a 15V+ charger");
                return;
            }
            for (int i = 0; i < 80 && !(pd->ready && pd->contract_mv >= 15000); i++) {
                vTaskDelay(pdMS_TO_TICKS(10));                // PS_RDY normally lands well inside 500 ms
            }
            if (!(pd->ready && pd->contract_mv >= 15000)) {
                reply("err charger refused 15V");
                return;
            }
        }
        if (v == 5) {
            pd_request(0);                                    // back to the most powerful offer
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
    } else if (!strcmp(tok[0], "listen") && n == 2) {              // stream the mic to the panel
        int s = atoi(tok[1]);
        mic_listen(s < 0 ? 0 : s > 300 ? 300 : s);
        reply("ok");
    } else if (!strcmp(tok[0], "micraw")) {                       // diagnostics on the live topic
        mic_raw_json(out, sizeof out);
        net_publish_live(out);
        reply("ok, see the live topic");
    } else if (!strcmp(tok[0], "palette") && n == 2) {           // "palette ff8800,2244ff,..." up to 8 stops
        uint8_t stops[8][3];
        int ns = 0;
        char *save2;
        for (char *h = strtok_r(tok[1], ",", &save2); h && ns < 8; h = strtok_r(NULL, ",", &save2)) {
            if (strlen(h) != 6) {
                reply("err palette rrggbb,rrggbb,...");
                return;
            }
            unsigned long v = strtoul(h, NULL, 16);
            stops[ns][0] = v >> 16;
            stops[ns][1] = v >> 8;
            stops[ns][2] = v;
            ns++;
        }
        if (!ns) {
            reply("err palette rrggbb,...");
            return;
        }
        leds_set_palette(stops, ns);
        color = COLOR_PALETTE;
        reply("ok");
    } else if (!strcmp(tok[0], "live") && n == 2) {
        int s = atoi(tok[1]);
        live_until = esp_timer_get_time() + (s < 0 ? 0 : s > 600 ? 600 : s) * 1000000LL;
        reply("ok");
    } else if (!strcmp(tok[0], "audio") && n == 6) {              // 20 Hz from the panel: no reply, no state publish
        uint8_t b[3] = {atoi(tok[2]), atoi(tok[3]), atoi(tok[4])};
        mic_feed(atoi(tok[1]), b, atoi(tok[5]));
    } else if (!strcmp(tok[0], "ota") && n == 2) {
        reply(ota_start(tok[1]) ? "ok, downloading; the board restarts when done" : "err ota busy or bad url");
    } else if (!strcmp(tok[0], "wifi2") && (n == 2 || n == 3)) {   // "wifi2 off" clears the fallback
        reply(net_save_wifi2(n == 3 ? tok[1] : "", n == 3 ? tok[2] : "") ? "ok, restarting" : "err nvs");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (!strcmp(tok[0], "mqtt") && (n == 2 || n == 4)) {
        reply(net_save_mqtt(tok[1], n == 4 ? tok[2] : NULL, n == 4 ? tok[3] : NULL) ? "ok, restarting" : "err nvs");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (!strcmp(tok[0], "help")) {
        reply("on off rgb<r g b> level<n> count<n> type<" LED_TYPES "> mode<" MODES "> color<" LED_COLORS "> "
              "bright<level|sound> overlay<none|vu|pulse> auto<on|off> gain<n> lux<lo hi> night<n> temp<lo hi> temp<c> "
              "volt<5|12 yes> palette<hex,hex,..> live<s> listen<s> audio<l b m t p> status pd net ota<url> wifi<ssid pass> wifi2<ssid pass|off> mqtt<url [user pass]>");
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
    leds_set_limit(limit);
    pd_set_max_mv(pdmax);
    boot_bookkeeping();                     // may lower it again for a safe boot
    ESP_LOGI(TAG, "boot %lu, reset %s%s; earlier: %s | %s | %s", (unsigned long)boots, reset_name(), safe_boot ? ", SAFE 5 V (last two boots died early)" : "", boot_log[1], boot_log[2], boot_log[3]);
    ESP_LOGI(TAG, "leds up, %d x %s", leds_get_count(), leds_type_name());
    vTaskDelay(pdMS_TO_TICKS(pddelay));
    pd_init(bus);
    sensors_init(bus);
    have_mic = mic_init();
    ESP_LOGI(TAG, "microphone %d", have_mic);

    cmd_q = xQueueCreate(8, sizeof(cmd_t));
    xTaskCreate(console_task, "console", 3072, NULL, 3, NULL);
    ble_init(cmd_q);
    net_init(cmd_q);

    static char snap[768];
    status(snap, sizeof snap);               // one snapshot in the log so sensors can be checked without BLE
    ESP_LOGI(TAG, "%s", snap);

    int64_t last_sense = 0, last_beat = 0, last_pub = 0, last_frame = 0, last_live = 0;
    bool image_ok = false;
    uint32_t frame_max_us = 0, frames = 0;
    cmd_t cmd;
    for (;;) {
        leds_set_budget_mw(power_budget_mw());
        if (xQueueReceive(cmd_q, &cmd, 0)) {
            bool quiet = !strncmp(cmd.text, "audio ", 6);
            handle(cmd.text);
            frame();
            if (!quiet) {
                publish();
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - last_sense > 500000) {
            sense();
            last_sense = now;
        }
        if (now > 30000000 && !image_ok) {              // survived 30 s after an OTA: keep this image
            ota_mark_ok();
            image_ok = true;
        }
        if (now - last_beat > 3000000) {
            const pd_state_t *pd = pd_get();
            char net[96];
            net_status(net, sizeof net);
            uint32_t mic_us = 0, mic_drops = 0;
            if (have_mic) {
                mic_stats(&mic_us, &mic_drops);
            }
            ESP_LOGI(TAG, "up %llds, pd cc%d ready=%d %dmV/%dmA, ble %s, %s, power %d, mode %s, "
                     "audio %luus/8000 dropped %lu, frame %luus %lu fps",
                     now / 1000000, pd->cc, pd->ready, pd->contract_mv, pd->contract_ma,
                     ble_connected() ? "connected" : "advertising", net, power, effect_name(),
                     (unsigned long)mic_us, (unsigned long)mic_drops, (unsigned long)frame_max_us,
                     (unsigned long)(frames * 1000000 / (uint32_t)(now - last_beat)));
            frame_max_us = 0;
            frames = 0;
            last_beat = now;
        }
        if (pd_get()->phase != pd_phase_seen) {                 // write each PD step at once: a supply that cuts VBUS
            pd_phase_seen = pd_get()->phase;                     // mid-handshake leaves the evidence in NVS
            save_pd_step(pd_phase_seen);
        }
        if (!up_saved && now > 15000000) {                     // stayed up: this supply works
            strncat(boot_log[0], " ok", sizeof boot_log[0] - strlen(boot_log[0]) - 1);
            write_log0();
            up_saved = true;
        }
        if (dirty_at && now - dirty_at > 2000000) {          // sliders send many values; write NVS once they settle
            save_count(leds_get_count());
            for (int i = 0; i < N_SETTINGS; i++) {
                save_i16(settings[i].key, *settings[i].val);
            }
            dirty_at = 0;
        }
        if (now < live_until && now - last_live > LIVE_MS * 1000LL) {
            uint8_t b[3] = {0};
            if (have_mic) {
                mic_bands(b);
            }
            char live[96];
            snprintf(live, sizeof live, "{\"loud\":%d,\"bands\":[%d,%d,%d],\"pulse\":%d,\"db\":%.0f,\"cap\":%d}",
                     have_mic ? mic_loud() : 0, b[0], b[1], b[2], have_mic ? mic_pulse() : 0, have_mic ? mic_db() : 0.0f, leds_get_cap());
            net_publish_live(live);
            last_live = now;
        }
        if (now - last_pub > PUBLISH_MS * 1000LL) {
            publish();
            last_pub = now;
        }
        if (power && now - last_frame > (live() ? FRAME_MS * 1000 : 1000000)) {
            frame();                                    // pixels only latch while powered, so still frames go out again once a second
            uint32_t us = esp_timer_get_time() - now;
            if (us > frame_max_us) {
                frame_max_us = us;
            }
            frames++;
            last_frame = now;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
