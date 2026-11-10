// Ledify firmware: USB-PD sink, LED strip, sensors, microphone, driven over BLE and MQTT.
//
// Text commands (USB serial console, BLE UART app, or MQTT topic ledify/<id>/cmd):
//   on | off | rgb R G B | level N | count N | type ws2812|ws2811|sk6812|sk6812w|ws2816
//   mode solid|rainbow|temp|spectrum|meter|eq|ripple|mirror|chroma|flow|bubble|tilt|shake|spin|slosh|height|sound|vu|pulse|auto
//                                                  presets, the Home Assistant effects
//   color solid|rainbow|temp|spectrum|meter|eq|...|slosh | bright level|sound | overlay none|vu|pulse | auto on|off
//   motion [N|cal|zero|hzero] | axis 0-5 | hstep CM IMU: snapshot (or print it N s), recalibrate, level here,
//                                                  height 0 here; strip axis; cm per colour step of the height style
//   gain N | lux LO HI | night N | temp LO HI      saved tunables (or one at a time: gain N, lux_lo N, ...)
//   temp 21.5                                      feed a temperature in for the temp colour
//   live N (stream loudness/bands N s) | audio L B M T P (feed from the panel) | volt 5 | volt 12 yes | status | pd | net | help
//   wifi SSID PASSWORD | wifi2 SSID PASSWORD|- | wifi2 list|off|del X|try X | mqtt URL [USER PASSWORD]   (saved to NVS, then the board restarts)
// Home Assistant talks JSON to ledify/<id>/light/set; see net.c for the topics.
//
// A frame is colour x brightness x overlay, drawn by leds_frame():
//   colour      solid | rainbow | temp (blue-white-red from the temperature) | spectrum (bass/mid/treble bars) | meter
//               | eq | ripple | mirror | chroma | flow (whole-strip sound styles, fx.c)
//               | bubble | tilt | shake | spin | slosh | height (whole-strip motion styles, fx.c, from the IMU and
//                 barometer via fusion.c)
//   brightness  level, scaled by the lux curve when auto is on, by the microphone when bright is sound
//   overlay     none | vu (lit length follows the sound) | pulse (flash on each beat, a quarter in between)
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "board.h"
#include "cmd.h"
#include "leds.h"
#include "pd.h"
#include "sensors.h"
#include "mic.h"
#include "motion.h"
#include "ble.h"
#include "net.h"
#include "ota.h"
#include "esp_app_desc.h"
#include "sched.h"
#include "stream.h"
#include "web.h"

static const char *TAG = "ledify";

#define AUTO_FLOOR 20          // brightness at lux_lo and below when auto is on
#define PULSE_FLOOR 64         // brightness between beats, out of 255

typedef enum { OVER_NONE, OVER_VU, OVER_PULSE } overlay_t;
static const char *color_names[] = {"solid", "rainbow", "temp", "spectrum", "meter", "palette", "cover", "eq", "ripple", "mirror", "chroma", "flow",
                                     "bubble", "tilt", "shake", "spin", "slosh", "height"};
#define N_COLORS 18
static const char *overlay_names[] = {"none", "vu", "pulse"};
#define MODES "solid|rainbow|temp|spectrum|meter|palette|cover|eq|ripple|mirror|chroma|flow|bubble|tilt|shake|spin|slosh|height|sound|vu|pulse|auto"

static leds_color_t color;
static overlay_t overlay;
static bool sound_on, auto_on, dark;        // dark: under the night threshold
static uint32_t lat_us;                     // smoothed estimate of sound-in to light-out, see frame()
static uint32_t drawn_beats;                // the beat count the last frame showed: a new beat is drawn at once
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
static int64_t sleep_end, sleep_total, rise_end, rise_total;   // sleep: fade out, then off; rise: up from dark over minutes
static bool off_pending;                    // "off" fades first; the rail goes off once the strip is dark
static int hot_scale = 255;                 // brightness left by the thermal limit, 0..255
static float board_c = -100;                // latest board temperature
static int64_t restart_at;                  // a saved network setting restarts the board shortly after
static int64_t meter_until, meter_next;     // "mic N": print the mic level on the console 4 times a second
static int64_t motion_until, motion_next;   // "motion N": the same for the IMU
static int blink_left;                      // "blink N": half-periods of blue flashing still to go, 0 = not blinking
static int64_t blink_next;
static bool blink_lit, blink_was_on;        // blink_was_on: the output was on before, so leave it on afterwards
#define BLINK_HALF_US 250000                // 2 flashes per second
// What the strip was doing, in RTC memory: it survives a software restart (an update, a saved network setting), not
// a power cut, so the strip comes back as it was after an OTA without ever powering up a strip on its own at boot.
#define KEEP_MAGIC 0x4c454431
static RTC_NOINIT_ATTR struct { uint32_t magic; uint8_t power, sound, color, overlay, level, r, g, b; } keep;
static bool restore_power;
static QueueHandle_t cmd_q;

// Saved tunables. The key is the command, the Home Assistant topic ledify/<id>/<key>/set and the status field.
static int16_t gain = 25, lux_lo = 0, lux_hi = 500, night = 0, temp_lo = 15, temp_hi = 30;
static int16_t limit;                       // see settings[]
static int16_t pdmax = 20000;
static int16_t pddelay = PD_START_DELAY_MS;
static int16_t droop;                       // see settings[]
static int16_t tmax = THERMAL_MAX_C, fade = FADE_MS, scroll, react = 60, predict = 1, axis, hstep = 10;
static int64_t dirty_at;                    // settings changed by a slider: written to NVS once they settle
enum { S_GAIN, S_LUX_LO, S_LUX_HI, S_NIGHT, S_TEMP_LO, S_TEMP_HI, S_LIMIT, S_PDMAX, S_PDDELAY, S_DROOP, S_TMAX, S_FADE, S_SCROLL, S_REACT, S_PREDICT, S_AXIS, S_HSTEP, N_SETTINGS };
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
    {"droop", &droop, 0, 100},               // tail colour fix: red cut along the strip against voltage sag, in percent
    {"tmax", &tmax, 0, 120},                 // board temperature at which the strip is fully dimmed (starts 10 lower), 0 = off
    {"fade", &fade, 0, 5000},                // transition time in ms, 0 = instant
    {"scroll", &scroll, -200, 200},          // gradient/stripes travel, pixels per second; negative = the other way
    {"react", &react, 1, 100},               // how fast the sound styles fall back: release ~1 s at 1, 40 ms at 100
    {"predict", &predict, 0, 1},             // 1 = once the tempo is steady, light beats on time from the tempo tracker
    {"axis", &axis, 0, 5},                   // board axis along the strip for the motion styles: +x +y +z -x -y -z
    {"hstep", &hstep, 1, 500},               // height style: centimetres per colour step
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

static bool save_str(const char *key, const char *val)
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

// Setup window: holding the button 3 s opens it for 2 minutes. Wi-Fi, broker and OTA-source commands are only taken
// from the USB console or while it is open, and its one-time code lets whoever is at the board link a browser to it.
#define PAIR_HOLD_US   3000000
#define PAIR_WINDOW_US (120LL * 1000000)
static int64_t pair_until;
static char pair_buf[11];
static int cmd_src;                         // CMD_SRC_* of the command being handled

const char *pair_code(void)
{
    return pair_until && esp_timer_get_time() < pair_until ? pair_buf : NULL;
}

static int pair_secs_left(void)
{
    return pair_code() ? (int)((pair_until - esp_timer_get_time()) / 1000000) + 1 : 0;
}

static bool load_str(const char *key, char *buf, size_t n)
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

// "ff8800@0,2244ff@35,..." into stops; positions optional but all-or-nothing. Returns the count, 0 on a bad string.
static int parse_palette(char *arg, uint8_t stops[][3], uint8_t *pos, bool *with_pos, const char **err)
{
    int ns = 0, np = 0;
    char *save;
    for (char *h = strtok_r(arg, ",", &save); h && ns < LED_MAX_STOPS; h = strtok_r(NULL, ",", &save)) {
        char *at = strchr(h, '@');
        if (at) {
            *at++ = 0;
            int p = atoi(at);
            if (p < 0 || p > 100) {
                *err = "err palette: position 0-100";
                return 0;
            }
            pos[ns] = p;
            np++;
        }
        if (strlen(h) != 6) {
            *err = "err palette rrggbb[@pct],rrggbb[@pct],...";
            return 0;
        }
        unsigned long v = strtoul(h, NULL, 16);
        stops[ns][0] = v >> 16;
        stops[ns][1] = v >> 8;
        stops[ns][2] = v;
        ns++;
    }
    if (!ns || (np && np != ns)) {
        *err = ns ? "err palette: give every stop a position, or none" : "err palette rrggbb,...";
        return 0;
    }
    *with_pos = np > 0;
    return ns;
}

// "4,2,2" into run lengths; "0" or anything empty means blend. Returns the count.
static int parse_stripes(char *arg, uint8_t *len)
{
    int nl = 0;
    char *save;
    for (char *t = strtok_r(arg, ",", &save); t && nl < LED_MAX_STOPS; t = strtok_r(NULL, ",", &save)) {
        int v = atoi(t);
        if (v < 1 || v > 100) {
            return 0;
        }
        len[nl++] = v;
    }
    return nl;
}

static void save_segs(void)
{
    char str[LED_MAX_SEGS * 32];
    leds_segments_str(str, sizeof str);
    save_str("segs", str);
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
        char segs[LED_MAX_SEGS * 32];
        size_t sl = sizeof segs;
        if (nvs_get_str(h, "segs", segs, &sl) == ESP_OK) {
            leds_load_segments(segs);
        }
        char stripestr[LED_MAX_STOPS * 4 + 1];
        sl = sizeof stripestr;
        if (nvs_get_str(h, "stripes", stripestr, &sl) == ESP_OK) {
            uint8_t len[LED_MAX_STOPS];
            int nl = parse_stripes(stripestr, len);
            leds_set_stripes(len, nl);
        }
        char palstr[LED_MAX_STOPS * 11 + 1];
        sl = sizeof palstr;
        if (nvs_get_str(h, "pal", palstr, &sl) == ESP_OK) {       // the user's gradient survives a restart
            uint8_t stops[LED_MAX_STOPS][3], pos[LED_MAX_STOPS];
            bool wp;
            const char *err;
            int ns = parse_palette(palstr, stops, pos, &wp, &err);
            if (ns) {
                leds_set_palette(0, stops, wp ? pos : NULL, ns);
            }
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
    cmd_t cmd = { .src = CMD_SRC_CONSOLE };
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
    net_publish_reply(s);
    web_publish_reply(s);
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
    return sound_on || overlay != OVER_NONE || color == COLOR_SPECTRUM || LED_IS_SOUND(color);
}

static bool have_imu;

static bool uses_motion(void)
{
    return LED_IS_MOTION(color);
}

static const char *mic_note(void)
{
    return uses_mic() && !have_mic ? "ok, no mic" : uses_motion() && !have_imu ? "ok, no imu" : "ok";
}

// What Home Assistant sees as the effect: the overlay if there is one, else sound, else the colour source.
static const char *effect_name(void)
{
    return overlay != OVER_NONE ? overlay_names[overlay] : sound_on ? "sound" : color_names[color];
}

static const char *motion_state_name(uint8_t st)
{
    return st == 0 ? "calibrating" : st == 1 ? "calibrated" : "uncalibrated";
}

static void motion_json(char *out, size_t n)
{
    motion_snap_t m;
    motion_snapshot(&m);
    snprintf(out, n, "{\"motion\":\"%s\",\"still\":%d,\"tilt\":%.3f,\"side\":%.3f,\"heading\":%.1f,\"yaw_rate\":%.1f,"
             "\"energy\":%.3f,\"kicks\":%lu,\"up\":[%.3f,%.3f,%.3f],\"bias\":[%.2f,%.2f,%.2f],\"axis\":%d,\"height_cm\":%.1f,\"vz\":%.2f,\"baro\":%d}",
             motion_state_name(m.state), m.still, m.tilt, m.side, m.heading, m.yaw_rate, m.energy, (unsigned long)m.kicks,
             m.up[0], m.up[1], m.up[2], m.bias[0], m.bias[1], m.bias[2], axis, m.height * 100, m.vz, m.have_height);
}

// Presets: a colour resets the rest, vu brings its meter colours, sound and pulse keep whatever colour is
// showing. auto is the old name for the lux modifier.
static bool set_mode(const char *name)
{
    int c = lookup(name, color_names, N_COLORS);
    if (c >= 0) {
        if (c == COLOR_HEIGHT && color != COLOR_HEIGHT) {
            motion_zero_height();                // the height style counts from where it was switched on
        }
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
    leds_set_droop(droop);
    leds_set_fade(fade);
    leds_set_scroll(scroll);
    leds_set_react(react);
    mic_set_predict(predict);
    motion_set_axis(axis);
    leds_set_hstep(hstep);
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
    // Thermal limit: the barometer sits on the board next to the buck. Full brightness up to tmax - 10, nothing at tmax.
    board_c = d.have_baro ? d.temp_c : -100;
    hot_scale = 255;
    if (tmax > 0 && d.have_baro && d.temp_c > tmax - 10) {
        float f = (tmax - d.temp_c) / 10.0f;
        hot_scale = f <= 0 ? 0 : (int)(f * 255);
    }
}

// Power the rail, or fade the strip out first and let the loop cut the rail once it is dark.
static void set_power(bool on)
{
    if (on) {
        power = true;
        off_pending = false;
        leds_power(true);
    } else {
        power = false;
        sleep_end = 0;
        rise_end = 0;
        blink_left = 0;
        if (fade) {
            off_pending = true;
        } else {
            leds_power(false);
        }
    }
}

// Compose and send one frame.
static void frame(void)
{
    if (!power && !off_pending) {
        return;
    }
    mic_snap_t snap;
    mic_snapshot(&snap);
    uint8_t *bands = snap.bands;
    leds_set_audio(&snap);
    motion_snap_t mo;
    motion_snapshot(&mo);
    leds_set_motion(&mo);
    mic_set_lead((int32_t)leds_wire_us() + 1500);                // predicted beats fire this early: render + wire time
    if (snap.at) {                                               // audio-to-light delay: age of the analysis, plus half a
        int64_t age = esp_timer_get_time() - snap.at;            // block (the samples' mean age), plus the wire time
        uint32_t lat = (uint32_t)(age > 0 && age < 1000000 ? age : 0) + 4000 + leds_wire_us();
        lat_us = lat_us ? (lat_us * 7 + lat) / 8 : lat;
    }
    if (blink_left && !off_pending) {                            // blue flash over whatever the strip was showing
        uint8_t r, g, b;
        leds_get_color(&r, &g, &b);
        leds_fill(0, 0, 255);
        leds_frame(COLOR_SOLID, blink_lit ? (leds_get_level() ? leds_get_level() : 128) : 0, leds_get_count(), bands, true);
        leds_fill(r, g, b);
        return;
    }
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
    int64_t now = esp_timer_get_time();
    if (sleep_end) {                                             // sleep timer: linear fade to dark
        int64_t left = sleep_end - now;
        lvl = left > 0 ? (int)(lvl * left / sleep_total) : 0;
    }
    if (rise_end) {                                              // sunrise: linear fade up from dark
        int64_t left = rise_end - now;
        lvl = left > 0 ? (int)(lvl * (rise_total - left) / rise_total) : lvl;
    }
    lvl = lvl * hot_scale / 255;
    if (off_pending) {
        lvl = 0;
    }
    int count = leds_get_count();
    leds_color_t c = leds_stream_active() ? COLOR_STREAM : color;
    leds_frame(c, lvl, overlay == OVER_VU ? count * mic_loud() / 255 : count, bands, uses_mic() || uses_motion() || c == COLOR_STREAM);
}

// Styles that move redraw every FRAME_MS; a still frame only needs the once-a-second resend.
static bool live(void)
{
    return leds_animated(color) || uses_mic() || !leds_settled() || sleep_end || rise_end;
}

// Sound styles redraw every SOUND_FRAME_MS, or as fast as the wire allows on a long strip, so a beat reaches the
// LEDs within about one frame of the 16 ms analysis. Motion styles too, so movement looks fluid.
static int64_t frame_period_us(void)
{
    if (!live()) {
        return 1000000;
    }
    if (!uses_mic() && !uses_motion()) {
        return FRAME_MS * 1000;
    }
    int64_t wire = leds_wire_us() + 2000;
    return wire > SOUND_FRAME_MS * 1000 ? wire : SOUND_FRAME_MS * 1000;
}

static void status(char *out, size_t n)
{
    sensor_data_t d;
    sensors_read(&d);
    const pd_state_t *pd = pd_get();
    char net[96];
    net_status(net, sizeof net);
    int w = snprintf(out, n, "{\"setup\":%d,\"fw\":\"%s\",\"power\":%d,\"volt\":%d,\"mode\":\"%s\",\"color\":\"%s\",\"bright\":\"%s\",\"overlay\":\"%s\","
                     "\"auto\":%d,\"level\":%d,\"count\":%d,\"type\":\"%s\",\"cap\":%d",
                     pair_secs_left(), esp_app_get_description()->version, power, volt, effect_name(), color_names[color], sound_on ? "sound" : "level", overlay_names[overlay],
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
    char pal[LED_MAX_STOPS * 11 + 1], cov[LED_MAX_STOPS * 11 + 1];
    leds_palette_str(0, pal, sizeof pal);
    leds_palette_str(1, cov, sizeof cov);
    char str[LED_MAX_STOPS * 4 + 1];
    leds_stripes_str(str, sizeof str);
    w += snprintf(out + w, n - w, "],\"need_mw\":%lu,\"budget_mw\":%lu,\"palette\":\"%s\",\"cover\":\"%s\",\"stripe\":\"%s\"",
                  (unsigned long)leds_get_need_mw(), (unsigned long)leds_get_budget_mw(), pal, cov, str);
    char tm[8];
    sched_time(tm, sizeof tm);
    int64_t now = esp_timer_get_time();
    w += snprintf(out + w, n - w, ",\"time\":\"%s\",\"tz\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\",\"clients\":%d,\"hot\":%d,\"board_c\":%.1f,\"sleep\":%ld,\"rise\":%ld,\"sched\":",
                  tm, sched_tz(), web_name(), net_ip(), web_clients(), 100 - hot_scale * 100 / 255, board_c,
                  (long)(sleep_end > now ? (sleep_end - now) / 1000000 : 0), (long)(rise_end > now ? (rise_end - now) / 1000000 : 0));
    w += sched_json(out + w, n - w);
    w += snprintf(out + w, n - w, ",\"segs\":");
    w += leds_segments_json(out + w, n - w);
    w += snprintf(out + w, n - w, ",\"stream\":");
    w += stream_json(out + w, n - w);
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
        motion_snap_t m;
        motion_snapshot(&m);
        w += snprintf(out + w, n - w, ",\"acc\":[%.2f,%.2f,%.2f],\"gyr\":[%.1f,%.1f,%.1f],\"motion\":%d",
                      d.acc[0], d.acc[1], d.acc[2], d.gyr[0], d.gyr[1], d.gyr[2], m.state);
    }
    if (have_mic) {
        w += snprintf(out + w, n - w, ",\"sound_db\":%.1f,\"db_peak\":%.0f,\"loud\":%d,\"bpm\":%u,\"lock\":%u", mic_db(), mic_db_peak(), mic_loud(),
                      mic_bpm(), mic_lock());
    }
    snprintf(out + w, n - w, "}");
}

// Push the current state to MQTT (no-op until the broker is connected).
static void publish(void)
{
    if (!net_mqtt_connected() && !web_clients()) {
        return;
    }
    static char light[160], st[2560], sens[160];
    uint8_t r, g, b;
    leds_get_color(&r, &g, &b);
    snprintf(light, sizeof light,
             "{\"state\":\"%s\",\"brightness\":%d,\"color_mode\":\"rgb\",\"color\":{\"r\":%d,\"g\":%d,\"b\":%d},\"effect\":\"%s\"}",
             power ? "ON" : "OFF", leds_get_level(), r, g, b, effect_name());
    status(st, sizeof st);
    keep.magic = KEEP_MAGIC;
    keep.power = power;
    keep.sound = sound_on;
    keep.color = color;
    keep.overlay = overlay;
    keep.level = leds_get_level();
    keep.r = r;
    keep.g = g;
    keep.b = b;
    sensor_data_t d;
    sensors_read(&d);
    snprintf(sens, sizeof sens, "{\"lux\":%.1f,\"temp_c\":%.2f,\"pressure_kpa\":%.2f,\"sound_db\":%.1f}",
             d.lux, d.temp_c, d.pressure_pa / 1000.0f, have_mic ? mic_db() : 0.0f);
    net_publish(light, st, sens);
    web_publish(light, st, sens);
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
        set_power(!strcasecmp(str, "ON"));
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
    bool keep_case = !strcmp(tok[0], "wifi") || !strcmp(tok[0], "wifi2") || !strcmp(tok[0], "mqtt") || !strcmp(tok[0], "ota")   // credentials, URLs and
                     || !strcmp(tok[0], "tz") || !strcmp(tok[0], "sched") || !strcmp(tok[0], "name") || !strcmp(tok[0], "otabase");   // TZ rules are case sensitive
    for (int i = 1; i < n && !keep_case; i++) {
        for (char *p = tok[i]; *p; p++) {
            *p = tolower((unsigned char)*p);
        }
    }
    static char out[2560];                  // status JSON: about 1.5 KB with every list filled
    int s = setting_idx(tok[0]);

    char base[96];
    bool setup_cmd = !strcmp(tok[0], "wifi") || !strcmp(tok[0], "mqtt") || !strcmp(tok[0], "otabase")
                     || (!strcmp(tok[0], "wifi2") && !(n == 2 && !strcmp(tok[1], "list")))
                     || (!strcmp(tok[0], "ota") && !load_str("ota_base", base, sizeof base));   // OTA from anywhere: setup only
    if (setup_cmd && cmd_src != CMD_SRC_CONSOLE && !pair_code()) {
        reply("err setup is locked: hold the board's button for 3 s, then send it within 2 minutes");
        return;
    }

    if (!strcmp(tok[0], "on") && n == 1) {
        sleep_end = 0;
        set_power(true);
        reply("ok");
    } else if (!strcmp(tok[0], "off") && n == 1) {
        set_power(false);
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
        int c = lookup(tok[1], color_names, N_COLORS);
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
    } else if (!strcmp(tok[0], "blink") && n <= 2) {              // blink [N]: flash the strip blue N times (default 10), 0 stops
        int k = n == 2 ? atoi(tok[1]) : 10;
        if (k < 0 || k > 1000) {
            reply("err blink 0-1000");
            return;
        }
        if (!k) {
            if (blink_left) {
                blink_left = 1;                                  // ends on the next tick, which restores the output
                blink_next = 0;
            }
            reply("ok");
            return;
        }
        if (!blink_left) {
            blink_was_on = power;
        }
        if (!power) {
            set_power(true);                                     // the rail at the selected voltage; blink never changes it
        }
        blink_left = k * 2;
        blink_lit = true;
        blink_next = esp_timer_get_time() + BLINK_HALF_US;
        reply("ok");
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
        restart_at = esp_timer_get_time() + 2000000;      // in two seconds: more settings may follow (the panel's setup sends several)
    } else if (!strcmp(tok[0], "mic") && n <= 2) {                // mic [N]: live level on the console for N s (default 10, 0 stops)
        int s = n == 2 ? atoi(tok[1]) : 10;
        if (!have_mic) {
            reply("err no mic, I2S did not start");
            return;
        }
        s = s < 0 ? 0 : s > 300 ? 300 : s;
        meter_until = s ? esp_timer_get_time() + (int64_t)s * 1000000 : 0;
        meter_next = 0;
        reply("ok, see the console");
    } else if (!strcmp(tok[0], "motion") && n <= 2) {             // motion: snapshot | N: on the console for N s | cal | zero | hzero
        if (!have_imu) {
            reply("err no imu");
        } else if (n == 2 && !strcmp(tok[1], "cal")) {
            motion_cal();
            reply("ok, hold the board still for a second");
        } else if (n == 2 && !strcmp(tok[1], "zero")) {
            motion_zero();
            reply("ok, level here");
        } else if (n == 2 && !strcmp(tok[1], "hzero")) {
            motion_zero_height();
            reply("ok, height 0 here");
        } else if (n == 2) {
            int s = atoi(tok[1]);
            s = s < 0 ? 0 : s > 300 ? 300 : s;
            motion_until = s ? esp_timer_get_time() + (int64_t)s * 1000000 : 0;
            motion_next = 0;
            reply("ok, see the console");
        } else {
            motion_json(out, sizeof out);
            reply(out);
        }
    } else if (!strcmp(tok[0], "micraw")) {                       // diagnostics on the live topic
        mic_raw_json(out, sizeof out);
        net_publish_live(out);
        reply("ok, see the live topic");
    } else if (!strcmp(tok[0], "stripe") && n == 2) {             // stripe 0 | N | N,N,...: pixels per gradient colour, cycling
        uint8_t len[LED_MAX_STOPS];
        int nl = strcmp(tok[1], "0") ? parse_stripes(tok[1], len) : 0;
        if (!nl && strcmp(tok[1], "0")) {
            reply("err stripe 0 | px | px,px,... (1-100 each)");
            return;
        }
        leds_set_stripes(len, nl);
        char str[LED_MAX_STOPS * 4 + 1];
        leds_stripes_str(str, sizeof str);
        save_str("stripes", str);
        reply("ok");
    } else if ((!strcmp(tok[0], "palette") || !strcmp(tok[0], "cover")) && n == 2) {   // "palette ff8800@0,2244ff@35,...": the user's gradient (saved); "cover ...": the album cover's
        uint8_t stops[LED_MAX_STOPS][3], pos[LED_MAX_STOPS];
        bool with_pos;
        const char *err;
        int ns = parse_palette(tok[1], stops, pos, &with_pos, &err);
        if (!ns) {
            reply(err);
            return;
        }
        int slot = tok[0][0] == 'c';
        leds_set_palette(slot, stops, with_pos ? pos : NULL, ns);
        if (!slot) {
            color = COLOR_PALETTE;                                // the user's gradient shows at once; the cover only when that effect is chosen
            char str[LED_MAX_STOPS * 11 + 1];
            leds_palette_str(0, str, sizeof str);
            save_str("pal", str);
        }
        reply("ok");
    } else if (!strcmp(tok[0], "live") && n == 2) {
        int s = atoi(tok[1]);
        live_until = esp_timer_get_time() + (s < 0 ? 0 : s > 600 ? 600 : s) * 1000000LL;
        reply("ok");
    } else if (!strcmp(tok[0], "audio")) {                        // 20 Hz from the panel: no reply, no state publish
        if (n == 6 || n == 7) {                                   // optional 7th: the 16-band spectrum, 2 hex digits per band
            uint8_t b[3] = {atoi(tok[2]), atoi(tok[3]), atoi(tok[4])}, sp[MIC_SPEC];
            bool have_sp = n == 7 && strlen(tok[6]) == MIC_SPEC * 2;
            for (int k = 0; have_sp && k < MIC_SPEC; k++) {
                char hx[3] = {tok[6][2 * k], tok[6][2 * k + 1], 0}, *end;
                sp[k] = strtoul(hx, &end, 16);
                have_sp = *end == 0;
            }
            mic_feed(atoi(tok[1]), b, atoi(tok[5]), have_sp ? sp : NULL);
        }
    } else if (!strcmp(tok[0], "ota") && n == 2) {
        char base[96];
        if (load_str("ota_base", base, sizeof base) && strncmp(tok[1], base, strlen(base))) {   // "otabase URL" pins where images may come from
            reply("err ota: the url must start with the saved otabase");
            return;
        }
        reply(ota_start(tok[1]) ? "ok, downloading; the board restarts when done" : "err ota busy or bad url");
    } else if (!strcmp(tok[0], "wifi2") && n == 2 && !strcmp(tok[1], "list")) {
        net_fallback_list(out, sizeof out);
        reply(out);
    } else if (!strcmp(tok[0], "wifi2") && n == 3 && !strcmp(tok[1], "try")) {   // until the next boot, saves nothing
        reply(net_fallback_try(tok[2]) ? "ok, joining" : "err no such fallback");
    } else if (!strcmp(tok[0], "wifi2") && (n == 2 || n == 3)) {   // the panel sends no password for an open network, "-" says so explicitly
        bool ok = n == 2 && !strcmp(tok[1], "off") ? net_fallback_clear()
                  : n == 3 && !strcmp(tok[1], "del") ? net_fallback_del(tok[2])
                  : net_fallback_add(tok[1], n == 3 && strcmp(tok[2], "-") ? tok[2] : "");
        if (!ok) {
            reply("err fallback list full, name too long, or no such entry");
            return;
        }
        reply("ok, restarting");
        restart_at = esp_timer_get_time() + 2000000;      // in two seconds: more settings may follow (the panel's setup sends several)
    } else if (!strcmp(tok[0], "mqtt") && (n == 2 || n == 4)) {
        reply(net_save_mqtt(tok[1], n == 4 ? tok[2] : NULL, n == 4 ? tok[3] : NULL) ? "ok, restarting" : "err nvs");
        restart_at = esp_timer_get_time() + 2000000;      // in two seconds: more settings may follow (the panel's setup sends several)
    } else if (!strcmp(tok[0], "sleep") && n == 2) {              // fade out over N minutes, then off; 0 cancels
        int m = atoi(tok[1]);
        if (m < 0 || m > 720) {
            reply("err sleep 0-720 minutes");
            return;
        }
        if (!m) {
            sleep_end = 0;
            reply("ok, sleep cancelled");
            return;
        }
        if (!power) {
            set_power(true);
        }
        rise_end = 0;
        sleep_total = m * 60000000LL;
        sleep_end = esp_timer_get_time() + sleep_total;
        reply("ok");
    } else if (!strcmp(tok[0], "rise") && n == 2) {               // from dark up to the set level over N minutes
        int m = atoi(tok[1]);
        if (m < 1 || m > 720) {
            reply("err rise 1-720 minutes");
            return;
        }
        sleep_end = 0;
        rise_total = m * 60000000LL;
        rise_end = esp_timer_get_time() + rise_total;
        set_power(true);
        reply("ok");
    } else if (!strcmp(tok[0], "tz") && n == 2) {                 // POSIX rule, e.g. EST5EDT,M3.2.0,M11.1.0
        reply(sched_set_tz(tok[1]) ? "ok" : "err tz: a POSIX TZ string");
    } else if (!strcmp(tok[0], "sched")) {                        // sched | sched add HH:MM command | sched del N | sched clear
        if (n == 1) {
            sched_json(out, sizeof out);
            reply(out);
        } else if (!strcmp(tok[1], "add") && n >= 4) {
            char line[64] = "";
            for (int i = 3; i < n; i++) {
                strlcat(line, tok[i], sizeof line);
                if (i + 1 < n) {
                    strlcat(line, " ", sizeof line);
                }
            }
            reply(sched_add(tok[2], line) ? "ok" : "err sched add HH:MM command (8 entries, 47 chars)");
        } else if (!strcmp(tok[1], "del") && n == 3) {
            reply(sched_del(atoi(tok[2])) ? "ok" : "err sched del N");
        } else if (!strcmp(tok[1], "clear")) {
            sched_clear();
            reply("ok");
        } else {
            reply("err sched add HH:MM command | del N | clear");
        }
    } else if (!strcmp(tok[0], "seg")) {                          // seg | seg N START END [color [rrggbb]] | seg del N | seg off
        if (n == 1) {
            leds_segments_json(out, sizeof out);
            reply(out);
        } else if (!strcmp(tok[1], "off") || !strcmp(tok[1], "clear")) {
            leds_clear_segments();
            save_segs();
            reply("ok");
        } else if (!strcmp(tok[1], "del") && n == 3) {
            leds_clear_segment(atoi(tok[2]) - 1);
            save_segs();
            reply("ok");
        } else if (n >= 4) {
            int c = n >= 5 ? lookup(tok[4], color_names, 7) : COLOR_SOLID;
            uint8_t r, g, b;
            leds_get_color(&r, &g, &b);
            if (n >= 6 && strlen(tok[5]) == 6) {
                unsigned long v = strtoul(tok[5], NULL, 16);
                r = v >> 16; g = v >> 8; b = v;
            }
            if (c < 0 || !leds_set_segment(atoi(tok[1]) - 1, atoi(tok[2]), atoi(tok[3]), c, r, g, b)) {
                reply("err seg N(1-6) START END [" LED_COLORS " [rrggbb]], END after START");
                return;
            }
            save_segs();
            reply("ok");
        } else {
            reply("err seg N START END [color [rrggbb]] | del N | off");
        }
    } else if (!strcmp(tok[0], "name") && n == 2) {               // mDNS hostname: http://NAME.local
        reply(web_set_name(tok[1]) ? "ok" : "err name: letters, digits and dashes");
    } else if (!strcmp(tok[0], "otabase") && n == 2) {            // only accept OTA images from under this URL ("any" clears)
        reply(save_str("ota_base", strcmp(tok[1], "any") ? tok[1] : "") ? "ok" : "err nvs");
    } else if (!strcmp(tok[0], "pair") && n == 1) {               // the setup window's code, never over MQTT
        const char *c = pair_code();
        char p[48];
        snprintf(p, sizeof p, "pair %s %s", net_id(), c ? c : "none");
        if (cmd_src == CMD_SRC_BLE) {
            ble_send(p);
        } else if (cmd_src == CMD_SRC_WS) {
            web_send_pair(net_id(), c ? c : "none");
        } else if (cmd_src == CMD_SRC_CONSOLE) {
            ESP_LOGI(TAG, "%s", p);
        } else {
            reply("err pair: only over USB, Bluetooth or the board's own page");
        }
    } else if (!strcmp(tok[0], "help")) {
        reply("on off blink<n> rgb<r g b> level<n> count<n> type<" LED_TYPES "> mode<" MODES "> color<" LED_COLORS "> "
              "bright<level|sound> overlay<none|vu|pulse> auto<on|off> gain<n> lux<lo hi> night<n> temp<lo hi> temp<c> tmax<c> fade<ms> "
              "volt<5|12 yes> droop<n> palette<hex[@pct],..> cover<hex[@pct],..> stripe<0|px|px,px,..> scroll<px/s> seg<n a b [color [hex]]|del n|off> sleep<min> rise<min> tz<posix> sched<add hh:mm cmd|del n|clear> "
              "name<host> pair live<s> mic<s> motion<|s|cal|zero|hzero> axis<0-5> hstep<cm> audio<l b m t p [spec]> status pd net ota<url> otabase<url|any> wifi<ssid pass> wifi2<ssid pass|ssid -|list|off|del x|try x> mqtt<url [user pass]>");
    } else {
        reply("err, send help");
    }
}

static void pair_open(void)
{
    static const char abc[] = "23456789abcdefghjkmnpqrstuvwxyz";     // no 0/o, 1/l/i: read off a screen
    for (int i = 0; i < 10; i++) {
        pair_buf[i] = abc[esp_random() % (sizeof abc - 1)];
    }
    pair_buf[10] = 0;
    pair_until = esp_timer_get_time() + PAIR_WINDOW_US;
    ESP_LOGI(TAG, "setup window open for 2 minutes");
    net_publish_pair();
    web_send_pair(net_id(), pair_buf);
    char b[] = "blink 3";                                           // three blue flashes: the window is open
    cmd_src = CMD_SRC_OTHER;
    handle(b);
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
    if (esp_reset_reason() == ESP_RST_SW && keep.magic == KEEP_MAGIC && keep.color < COLOR_STREAM && keep.overlay <= OVER_PULSE) {
        color = keep.color;                                      // back to what the strip showed before the update
        overlay = keep.overlay;
        sound_on = keep.sound;
        leds_fill(keep.r, keep.g, keep.b);
        leds_set_level(keep.level);
        restore_power = keep.power;
        ESP_LOGI(TAG, "restoring the strip from before the restart (power %d)", keep.power);
    }
    keep.magic = 0;
    leds_set_limit(limit);
    leds_set_droop(droop);
    leds_set_fade(fade);
    leds_set_scroll(scroll);
    leds_set_react(react);
    leds_set_hstep(hstep);
    mic_set_predict(predict);
    pd_set_max_mv(pdmax);
    boot_bookkeeping();                     // may lower it again for a safe boot
    ESP_LOGI(TAG, "boot %lu, reset %s%s; earlier: %s | %s | %s", (unsigned long)boots, reset_name(), safe_boot ? ", SAFE 5 V (last two boots died early)" : "", boot_log[1], boot_log[2], boot_log[3]);
    ESP_LOGI(TAG, "leds up, %d x %s", leds_get_count(), leds_type_name());
    vTaskDelay(pdMS_TO_TICKS(pddelay));
    pd_init(bus);
    sensors_init(bus);
    motion_set_axis(axis);
    have_imu = motion_init();                // calibrates by itself once the board has been still for a second
    have_mic = mic_init();
    ESP_LOGI(TAG, "microphone %d, imu %d", have_mic, have_imu);

    cmd_q = xQueueCreate(8, sizeof(cmd_t));
    xTaskCreate(console_task, "console", 3072, NULL, 3, NULL);
    ble_init(cmd_q);
    net_init(cmd_q);
    sched_init();
    stream_init();
    web_init(cmd_q);
    gpio_config_t btn = { .pin_bit_mask = 1ULL << PIN_BUTTON, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&btn);

    static char snap[2560];
    status(snap, sizeof snap);               // one snapshot in the log so sensors can be checked without BLE
    ESP_LOGI(TAG, "%s", snap);

    int64_t last_sense = 0, last_beat = 0, last_pub = 0, last_frame = 0, last_live = 0, pub_due = 0, btn_down = 0;
    bool image_ok = false, btn_was = false;
    uint32_t frame_max_us = 0, frames = 0;
    cmd_t cmd;
    for (;;) {
        leds_set_budget_mw(power_budget_mw());
        if (xQueueReceive(cmd_q, &cmd, 0)) {
            bool quiet = !strncmp(cmd.text, "audio ", 6);
            cmd_src = cmd.src;
            handle(cmd.text);
            frame();
            if (!quiet && !pub_due) {
                pub_due = esp_timer_get_time() + 150000;   // a slider sends many commands: one state publish per 150 ms
            }
        }
        int64_t now = esp_timer_get_time();
        const char *due = sched_due();                          // schedules run as if typed
        if (due) {
            cmd_t c = {0};
            strlcpy(c.text, due, sizeof c.text);
            xQueueSend(cmd_q, &c, 0);
        }
        bool btn = !gpio_get_level(PIN_BUTTON);                  // a short press toggles the output
        if (btn && !btn_was) {
            btn_down = now;
        } else if (!btn && btn_was && now - btn_down > 40000 && now - btn_down < 1000000) {
            set_power(!power);
            frame();
            pub_due = now + 150000;
        } else if (btn && btn_was && btn_down && now - btn_down > PAIR_HOLD_US) {   // held 3 s: open the setup window
            btn_down = 0;
            pair_open();
            pub_due = now + 150000;
        }
        btn_was = btn;
        if (blink_left && now >= blink_next) {
            blink_lit = !blink_lit;
            blink_next = now + BLINK_HALF_US;
            if (!--blink_left && !blink_was_on) {
                set_power(false);                                // it was off before the blink: back off
            }
            frame();                                             // the next flash, or back to the normal style
            last_frame = now;
            if (!blink_left) {
                pub_due = now + 150000;
            }
        }
        if (sleep_end && now >= sleep_end) {
            sleep_end = 0;
            set_power(false);
            pub_due = now + 150000;
        }
        if (rise_end && now >= rise_end) {
            rise_end = 0;
            pub_due = now + 150000;
        }
        if (off_pending && leds_settled()) {                    // the fade-out is done: cut the rail
            off_pending = false;
            leds_power(false);
        }
        if (restart_at && now > restart_at) {
            publish();                                           // snapshot for the restore
            esp_restart();
        }
        if (restore_power && (pd_get()->ready || now > 3000000)) {   // once the supply is negotiated (or it is not coming)
            restore_power = false;
            set_power(true);
            pub_due = now + 150000;
        }
        if (power && stream_take_frame()) {                     // a LAN frame arrived: show it right away
            frame();
            last_frame = now;
        }
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
                     "audio %luus/8000 dropped %lu, frame %luus %lu fps, sound to light ~%lums",
                     now / 1000000, pd->cc, pd->ready, pd->contract_mv, pd->contract_ma,
                     ble_connected() ? "connected" : "advertising", net, power, effect_name(),
                     (unsigned long)mic_us, (unsigned long)mic_drops, (unsigned long)frame_max_us,
                     (unsigned long)(frames * 1000000 / (uint32_t)(now - last_beat)), (unsigned long)(lat_us / 1000));
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
        if (now < meter_until && now >= meter_next) {          // "mic": loudest 8 ms block of each quarter second
            int32_t mn, mx, mean;
            float r = mic_meter(&mn, &mx, &mean);
            float dbfs = r > 0 ? 20 * log10f(r * 1.4142f) : -120;   // 0 dBFS = full-scale sine
            int bar = dbfs < -90 ? 0 : (int)((dbfs + 90) / 3);          // one # per 3 dB above -90 dBFS
            char bars[31];
            memset(bars, '#', sizeof bars - 1);
            bars[bar < 30 ? bar : 30] = 0;
            ESP_LOGI(TAG, "mic %6.1f dBFS ~%3.0f dB SPL  raw min %ld max %ld mean %ld  |%-30s|", dbfs,
                     dbfs + MIC_DB_FULL_SCALE, (long)mn, (long)mx, (long)mean, bars);
            meter_next = now + 250000;
        }
        if (now < motion_until && now >= motion_next) {        // "motion N": the fused numbers 4 times a second
            motion_snap_t m;
            motion_snapshot(&m);
            ESP_LOGI(TAG, "motion %s%s tilt %+.2f side %+.2f heading %5.1f turn %+6.1f deg/s energy %.2f kicks %lu height %+.1f cm",
                     motion_state_name(m.state), m.still ? " still" : "", m.tilt, m.side, m.heading, m.yaw_rate, m.energy,
                     (unsigned long)m.kicks, m.height * 100);
            motion_next = now + 250000;
        }
        if (now < live_until && now - last_live > LIVE_MS * 1000LL) {
            mic_snap_t a;
            mic_snapshot(&a);                                     // the mic, or what the panel feeds in
            char live[320];
            int w = snprintf(live, sizeof live, "{\"loud\":%d,\"bands\":[%d,%d,%d],\"pulse\":%d,\"db\":%.0f,\"cap\":%d,\"lat\":%lu,"
                             "\"bpm\":%u,\"lock\":%u,\"spec\":[",
                             a.loud, a.bands[0], a.bands[1], a.bands[2], a.pulse, have_mic ? mic_db() : 0.0f, leds_get_cap(),
                             (unsigned long)(lat_us / 1000), have_mic ? mic_bpm() : 0, have_mic ? mic_lock() : 0);
            for (int k = 0; k < MIC_SPEC; k++) {
                w += snprintf(live + w, sizeof live - w, "%s%d", k ? "," : "", a.spec[k]);
            }
            w += snprintf(live + w, sizeof live - w, "]");
            if (have_imu) {                                       // motion for the panel's previews: tilt, side, energy x100
                motion_snap_t m;
                motion_snapshot(&m);
                w += snprintf(live + w, sizeof live - w, ",\"mo\":[%d,%d,%d,%d,%d]", (int)(m.tilt * 100), (int)(m.side * 100),
                              (int)m.heading, (int)(m.energy * 100), (int)lroundf(m.height * 100));
            }
            snprintf(live + w, sizeof live - w, "}");
            net_publish_live(live);
            web_publish_live(live);
            last_live = now;
        }
        if (pub_due && now >= pub_due) {
            publish();
            pub_due = 0;
            last_pub = now;
        }
        if (now - last_pub > PUBLISH_MS * 1000LL) {
            publish();
            last_pub = now;
        }
        // A new beat is drawn at once rather than at the next 16 ms tick, as soon as the wire is free again.
        bool beat_now = power && uses_mic() && mic_beats() != drawn_beats && now - last_frame >= (int64_t)leds_wire_us() + 1000;
        if ((power || off_pending) && (beat_now || now - last_frame >= frame_period_us())) {
            drawn_beats = mic_beats();
            frame();                                    // pixels only latch while powered, so still frames go out again once a second
            uint32_t us = esp_timer_get_time() - now;
            if (us > frame_max_us) {
                frame_max_us = us;
            }
            frames++;
            last_frame = now;
        }
        vTaskDelay(pdMS_TO_TICKS(power && (uses_mic() || uses_motion()) ? 1 : 5));   // sound and motion styles: draw on time
    }
}
