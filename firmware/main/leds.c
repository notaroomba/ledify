// WS2812-style strip on the SIG pad via RMT, plus the buck enable and 5 V / 12 V select.
#include "leds.h"
#include "board.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

// Chip families the RMT encoder knows. WS2812B/C/D/E, WS2813 and WS2815 all speak WS2812 timing in GRB;
// WS2811 is the 12 V one-IC-per-three-LEDs chip in RGB; sk6812w is the RGBW variant.
static const struct {
    const char *name;
    led_model_t model;
    bool rgb_order;
    bool white;
} types[] = {
    {"ws2812", LED_MODEL_WS2812, false, false},
    {"ws2811", LED_MODEL_WS2811, true, false},
    {"sk6812", LED_MODEL_SK6812, false, false},
    {"sk6812w", LED_MODEL_SK6812, false, true},
    {"ws2816", LED_MODEL_WS2816, false, false},
};
static int type_idx;

static led_strip_handle_t strip;
static uint8_t level = 128;
static uint16_t count = NUM_LEDS;
static uint32_t budget_mw = LED_BUDGET_NO_PD_MW;
static uint32_t need_mw;                   // the last frame's estimated draw at the requested level
static bool limit_on;                      // scale brightness to the budget (else just report)
static uint8_t cur_r = 255, cur_g = 255, cur_b = 255;   // "on" with no colour set gives white
static uint8_t pal[2][LED_MAX_STOPS][3] = {{{255, 255, 255}}, {{255, 255, 255}}};   // gradient stops per slot (user, cover), in strip order
static uint8_t pal_pos[2][LED_MAX_STOPS];                 // and their position in %
static int npal[2] = {1, 1};
static uint8_t droop;                                     // tail colour fix in %, see leds_frame()
static uint16_t fade_ms = FADE_MS;
static uint8_t shown[MAX_LEDS][3];                        // what is on the strip now (before brightness), eased towards fb
static uint8_t shown_lvl;                                 // the brightness on the strip now
static bool settled = true;
static int64_t last_us;
typedef struct { uint16_t a, b; leds_color_t c; uint8_t r, g, bl; bool on; } seg_t;
static seg_t segs[LED_MAX_SEGS];
static const char *src_names[] = {"solid", "rainbow", "temp", "spectrum", "meter", "palette", "cover", "stream"};
static uint8_t stream_fb[MAX_LEDS][3];
static int64_t stream_at;
static uint8_t hue, temp_pos = 128, last_cap;
static uint8_t fb[MAX_LEDS][3];                          // one frame at full brightness, before the power cap

// (Re)create the RMT device for the selected chip: timing comes from the model, byte order from the format.
static void make_strip(void)
{
    if (strip) {
        led_strip_del(strip);
    }
    led_strip_config_t sc = {
        .strip_gpio_num = PIN_LED_DATA,
        .max_leds = count,
        .led_model = types[type_idx].model,
        .color_component_format = types[type_idx].white ? LED_STRIP_COLOR_COMPONENT_FMT_GRBW
                                : types[type_idx].rgb_order ? LED_STRIP_COLOR_COMPONENT_FMT_RGB
                                : LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = { .with_dma = false },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&sc, &rc, &strip));
}

// One pixel in the selected byte order. ponytail: RGBW strips get W=0, mix white in if someone owns one.
static void pixel(int i, uint8_t r, uint8_t g, uint8_t b)
{
    if (types[type_idx].white) {
        led_strip_set_pixel_rgbw(strip, i, r, g, b, 0);
    } else {
        led_strip_set_pixel(strip, i, r, g, b);
    }
}

void leds_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LED_ENABLE) | (1ULL << PIN_LED_12V),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_LED_ENABLE, 0);
    gpio_set_level(PIN_LED_12V, 0);

    make_strip();
    led_strip_clear(strip);
}

bool leds_set_type(const char *name)
{
    for (int i = 0; i < sizeof types / sizeof types[0]; i++) {
        if (!strcmp(name, types[i].name)) {
            type_idx = i;
            make_strip();
            return true;
        }
    }
    return false;
}

const char *leds_type_name(void)
{
    return types[type_idx].name;
}

void leds_power(bool on)
{
    if (!on) {
        led_strip_clear(strip);
        memset(shown, 0, sizeof shown);
        shown_lvl = 0;
        settled = true;
    }
    gpio_set_level(PIN_LED_ENABLE, on);
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(LED_POWER_SETTLE_MS));   // the pixels have to be powered before they can latch a frame
    }
}

void leds_select_12v(bool v)
{
    leds_power(false);               // never switch the rail while the strip is powered
    gpio_set_level(PIN_LED_12V, v);
}

void leds_set_level(uint8_t l)
{
    level = l;
}

uint8_t leds_get_level(void)
{
    return level;
}

void leds_set_count(uint16_t n)
{
    n = n < 1 ? 1 : n > MAX_LEDS ? MAX_LEDS : n;
    if (n == count) {
        return;
    }
    if (n < count) {                        // shrink: send one frame at the old length with the tail black, so the
        for (int i = n; i < count; i++) {   // dropped pixels go out without blanking the ones that stay lit
            pixel(i, 0, 0, 0);
        }
        led_strip_refresh(strip);
    }
    count = n;
    make_strip();                           // the RMT device sends max_leds pixels per frame; the main loop draws the
}                                           // next frame right after the command, so nothing flashes

uint16_t leds_get_count(void)
{
    return count;
}

void leds_set_budget_mw(uint32_t mw)
{
    budget_mw = mw;
}

uint8_t leds_get_cap(void)
{
    return last_cap;
}

void leds_fill(uint8_t r, uint8_t g, uint8_t b)
{
    cur_r = r; cur_g = g; cur_b = b;
}

void leds_get_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = cur_r; *g = cur_g; *b = cur_b;
}

void leds_set_temp(uint8_t pos)
{
    temp_pos = pos;
}

static void hsv(uint8_t h, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region = h / 43, rem = (h - region * 43) * 6;
    uint8_t q = 255 - rem, t = rem;
    switch (region) {
    case 0:  *r = 255; *g = t;   *b = 0;   break;
    case 1:  *r = q;   *g = 255; *b = 0;   break;
    case 2:  *r = 0;   *g = 255; *b = t;   break;
    case 3:  *r = 0;   *g = q;   *b = 255; break;
    case 4:  *r = t;   *g = 0;   *b = 255; break;
    default: *r = 255; *g = 0;   *b = q;   break;
    }
}

// Blue through white to red as the temperature position goes 0..255.
static void temp_rgb(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (temp_pos < 128) {
        *r = *g = temp_pos * 2;
        *b = 255;
    } else {
        *r = 255;
        *g = *b = (255 - temp_pos) * 2;
    }
}

// Scale a level down until the frame fits the power budget. sum is the frame's draw at full brightness in mW x 255
// (per-channel mW weighted by each pixel's colour); the dark current does not scale with brightness.
static uint8_t capped(uint32_t sum, uint8_t lvl)
{
    uint32_t dark = (uint32_t)count * LED_DARK_MW;
    uint32_t lit = sum / 255 * lvl / 255;                          // mW at this level, pixels only
    need_mw = dark + lit;
    if (dark + lit <= budget_mw || lit == 0) {
        return lvl;
    }
    uint32_t avail = budget_mw > dark ? budget_mw - dark : 0;
    return (uint32_t)lvl * avail / lit;
}

// One pixel of a colour source: i of n within its run (the whole strip, or a segment).
static void render(leds_color_t c, int i, int n, uint8_t r, uint8_t g, uint8_t b, const uint8_t bands[3], uint8_t *p)
{
    switch (c) {
    case COLOR_RAINBOW:
        hsv(hue + i * 256 / n, p, p + 1, p + 2);
        break;
    case COLOR_PALETTE:
    case COLOR_COVER: {                                         // blend between the two stops around this pixel, 8.8 fixed point
        int sl = c == COLOR_COVER, np = npal[sl];
        uint32_t x = n > 1 ? (uint32_t)i * 100 * 256 / (n - 1) : 0;   // position along the run in %, 8.8
        int k = 0;
        while (k < np - 2 && x >= (uint32_t)pal_pos[sl][k + 1] << 8) {
            k++;
        }
        int k1 = k + 1 < np ? k + 1 : k;
        uint32_t p0 = (uint32_t)pal_pos[sl][k] << 8, p1 = (uint32_t)pal_pos[sl][k1] << 8;
        uint32_t f = x <= p0 || p1 <= p0 ? 0 : x >= p1 ? 256 : (x - p0) * 256 / (p1 - p0);   // before the first stop and after the last: that stop
        for (int ch = 0; ch < 3; ch++) {
            p[ch] = (pal[sl][k][ch] * (256 - f) + pal[sl][k1][ch] * f) >> 8;
        }
        break;
    }
    case COLOR_METER:                                           // green, yellow, red along the run
        hsv(85 - i * 85 / n, p, p + 1, p + 2);
        break;
    case COLOR_SPECTRUM: {                                      // a red, green and blue bar, one per band. ponytail: fixed colours, tint from rgb if anyone asks
        int k = i * 3 / n, start = k * n / 3;
        if ((i - start) * 255 < bands[k] * ((k + 1) * n / 3 - start)) {
            p[k] = 255;
        }
        break;
    }
    case COLOR_TEMP:
        temp_rgb(p, p + 1, p + 2);
        break;
    default:
        p[0] = r; p[1] = g; p[2] = b;
    }
}

static const seg_t *seg_at(int i)
{
    for (int k = 0; k < LED_MAX_SEGS; k++) {
        if (segs[k].on && i >= segs[k].a && i < segs[k].b) {
            return &segs[k];
        }
    }
    return NULL;
}

// Move cur towards to by alpha/256 of the distance, always by at least one, so a transition really ends.
static uint8_t ease(uint8_t cur, uint8_t to, uint32_t alpha)
{
    int d = (int)to - cur;
    if (!d) {
        return cur;
    }
    settled = false;
    int step = (d * (int)alpha + (d > 0 ? 255 : -255)) / 256;
    if (!step) {
        step = d > 0 ? 1 : -1;
    }
    return cur + step;
}

void leds_frame(leds_color_t c, uint8_t lvl, uint16_t lit, const uint8_t bands[3], bool instant)
{
    if (leds_animated(c)) {
        hue++;
    }
    uint32_t sum = 0;
    for (int i = 0; i < count; i++) {
        uint8_t *p = fb[i];
        p[0] = p[1] = p[2] = 0;
        if (i >= lit) {
            continue;
        }
        if (c == COLOR_STREAM) {
            p[0] = stream_fb[i][0];
            p[1] = stream_fb[i][1];
            p[2] = stream_fb[i][2];
        } else {
            const seg_t *s = seg_at(i);
            if (s) {
                render(s->c, i - s->a, s->b - s->a, s->r, s->g, s->bl, bands, p);
            } else {
                render(c, i, count, cur_r, cur_g, cur_b, bands, p);
            }
        }
        sum += p[0] * LED_RED_MW + p[1] * LED_GREEN_MW + p[2] * LED_BLUE_MW;
    }
    last_cap = capped(sum, lvl);
    uint8_t use = limit_on ? last_cap : lvl;

    // Transitions: what is on the strip eases towards the new frame and level. alpha is the share of the remaining
    // distance covered this frame, so a change settles in about fade_ms whatever the frame rate.
    int64_t now = esp_timer_get_time();
    uint32_t dt = last_us ? (uint32_t)((now - last_us) / 1000) : 1000;
    last_us = now;
    uint32_t alpha = (!fade_ms || instant) ? 256 : dt * 768 / fade_ms;
    if (alpha > 256) {
        alpha = 256;
    }
    settled = true;
    shown_lvl = ease(shown_lvl, use, alpha);

    // A strip fed from one end sags towards the far end, where blue and green drop out of regulation before red, so
    // white turns warm along the strip. droop cuts red (and half as much green) with a parabolic profile, scaled
    // by how much of a full-white load this frame draws; the knob absorbs the strip's resistance. Feeding 5 V in
    // at the far end as well is the real fix.
    uint32_t load = droop ? (sum / 255 * shown_lvl / 255) * 256 / ((uint32_t)count * (LED_RED_MW + LED_GREEN_MW + LED_BLUE_MW)) : 0;   // 0..256 of full white
    for (int i = 0; i < count; i++) {
        for (int ch = 0; ch < 3; ch++) {
            shown[i][ch] = ease(shown[i][ch], fb[i][ch], alpha);
        }
        uint32_t r = shown[i][0] * shown_lvl / 255, g = shown[i][1] * shown_lvl / 255, b = shown[i][2] * shown_lvl / 255;
        if (load) {
            uint32_t prof = ((uint32_t)2 * count * i - (uint32_t)i * i) * 256 / ((uint32_t)count * count);   // 0 at the feed, 256 at the far end
            uint32_t sag = droop * load * prof / (100 * 256);                                               // 0..256
            r = r * (256 - sag) >> 8;
            g = g * (256 - sag / 2) >> 8;
        }
        pixel(i, r, g, b);
    }
    led_strip_refresh(strip);
}

void leds_set_limit(bool on)
{
    limit_on = on;
}

uint32_t leds_get_need_mw(void)
{
    return need_mw;
}

uint32_t leds_get_budget_mw(void)
{
    return budget_mw;
}

void leds_set_palette(int sl, const uint8_t (*stops)[3], const uint8_t *pos, int n)
{
    sl = sl ? 1 : 0;
    int np = npal[sl] = n < 1 ? 1 : n > LED_MAX_STOPS ? LED_MAX_STOPS : n;
    uint8_t (*p)[3] = pal[sl], *pp = pal_pos[sl];
    for (int i = 0; i < np; i++) {
        for (int ch = 0; ch < 3; ch++) {
            p[i][ch] = stops[i][ch];
        }
        pp[i] = pos ? (pos[i] > 100 ? 100 : pos[i]) : np > 1 ? i * 100 / (np - 1) : 0;
    }
    for (int i = 1; i < np; i++) {                               // keep the stops in strip order (stable insertion sort)
        for (int j = i; j > 0 && pp[j - 1] > pp[j]; j--) {
            uint8_t t = pp[j];
            pp[j] = pp[j - 1];
            pp[j - 1] = t;
            for (int ch = 0; ch < 3; ch++) {
                t = p[j][ch];
                p[j][ch] = p[j - 1][ch];
                p[j - 1][ch] = t;
            }
        }
    }
}

void leds_palette_str(int sl, char *out, size_t n)
{
    sl = sl ? 1 : 0;
    size_t w = 0;
    for (int i = 0; i < npal[sl] && w < n; i++) {
        w += snprintf(out + w, n - w, "%s%02x%02x%02x@%d", i ? "," : "", pal[sl][i][0], pal[sl][i][1], pal[sl][i][2], pal_pos[sl][i]);
    }
}

void leds_set_droop(uint8_t pct)
{
    droop = pct > 100 ? 100 : pct;
}

const char *leds_color_name(leds_color_t c)
{
    return src_names[c];
}

bool leds_animated(leds_color_t c)
{
    if (c == COLOR_RAINBOW) {
        return true;
    }
    for (int k = 0; k < LED_MAX_SEGS; k++) {
        if (segs[k].on && segs[k].c == COLOR_RAINBOW) {
            return true;
        }
    }
    return false;
}

void leds_set_fade(uint16_t ms)
{
    fade_ms = ms;
}

bool leds_settled(void)
{
    return settled;
}

bool leds_set_segment(int i, uint16_t a, uint16_t b, leds_color_t c, uint8_t r, uint8_t g, uint8_t bl)
{
    if (i < 0 || i >= LED_MAX_SEGS || a >= b || b > MAX_LEDS || c == COLOR_STREAM) {
        return false;
    }
    segs[i] = (seg_t){ .a = a, .b = b, .c = c, .r = r, .g = g, .bl = bl, .on = true };
    return true;
}

void leds_clear_segment(int i)
{
    if (i >= 0 && i < LED_MAX_SEGS) {
        segs[i].on = false;
    }
}

void leds_clear_segments(void)
{
    memset(segs, 0, sizeof segs);
}

int leds_segments_json(char *out, size_t n)
{
    int w = snprintf(out, n, "[");
    bool first = true;
    for (int k = 0; k < LED_MAX_SEGS; k++) {
        if (segs[k].on) {
            w += snprintf(out + (w < (int)n ? w : n), w < (int)n ? n - w : 0, "%s{\"i\":%d,\"a\":%d,\"b\":%d,\"color\":\"%s\",\"rgb\":\"%02x%02x%02x\"}",
                          first ? "" : ",", k + 1, segs[k].a, segs[k].b, src_names[segs[k].c], segs[k].r, segs[k].g, segs[k].bl);
            first = false;
        }
    }
    w += snprintf(out + (w < (int)n ? w : n), w < (int)n ? n - w : 0, "]");
    return w;
}

int leds_segments_str(char *out, size_t n)
{
    int w = 0;
    out[0] = 0;
    for (int k = 0; k < LED_MAX_SEGS; k++) {
        if (segs[k].on) {
            w += snprintf(out + (w < (int)n ? w : n), w < (int)n ? n - w : 0, "%s%d:%d-%d:%s:%02x%02x%02x",
                          w ? ";" : "", k + 1, segs[k].a, segs[k].b, src_names[segs[k].c], segs[k].r, segs[k].g, segs[k].bl);
        }
    }
    return w;
}

void leds_load_segments(const char *str)
{
    char buf[LED_MAX_SEGS * 32];
    strncpy(buf, str, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    char *save;
    for (char *e = strtok_r(buf, ";", &save); e; e = strtok_r(NULL, ";", &save)) {
        int i, a, b;
        char name[12];
        unsigned hex;
        if (sscanf(e, "%d:%d-%d:%11[^:]:%x", &i, &a, &b, name, &hex) != 5) {
            continue;
        }
        for (int c = 0; c < COLOR_STREAM; c++) {
            if (!strcmp(name, src_names[c])) {
                leds_set_segment(i - 1, a, b, c, hex >> 16, hex >> 8, hex);
            }
        }
    }
}

void leds_stream_pixels(uint16_t first, const uint8_t *rgb, int npx)
{
    if (first >= MAX_LEDS) {
        return;
    }
    if (first + npx > MAX_LEDS) {
        npx = MAX_LEDS - first;
    }
    memcpy(stream_fb[first], rgb, npx * 3);
    stream_at = esp_timer_get_time();
}

bool leds_stream_active(void)
{
    return stream_at && esp_timer_get_time() - stream_at < STREAM_TIMEOUT_MS * 1000LL;
}
