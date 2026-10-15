// WS2812-style strip on the SIG pad via RMT, plus the buck enable and 5 V / 12 V select.
#include "leds.h"
#include "board.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
static uint8_t cur_r = 255, cur_g = 255, cur_b = 255;   // "on" with no colour set gives white
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
    led_strip_clear(strip);                 // pixels past the new end keep their last frame, so blank them first
    count = n < 1 ? 1 : n > MAX_LEDS ? MAX_LEDS : n;
    make_strip();                           // the RMT device sends max_leds pixels per frame
}

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

// Scale a level down until the frame fits the power budget. sum is r+g+b over every pixel at full brightness.
static uint8_t capped(uint32_t sum, uint8_t lvl)
{
    uint32_t need = sum / 255 * lvl * LED_MW_PER_CHANNEL / 255;   // mW at this level
    return need > budget_mw ? lvl * budget_mw / need : lvl;
}

void leds_frame(leds_color_t c, uint8_t lvl, uint16_t lit, const uint8_t bands[3])
{
    uint8_t r = cur_r, g = cur_g, b = cur_b;
    if (c == COLOR_TEMP) {
        temp_rgb(&r, &g, &b);
    }
    if (c == COLOR_RAINBOW) {
        hue++;
    }
    uint32_t sum = 0;
    for (int i = 0; i < count; i++) {
        uint8_t *p = fb[i];
        p[0] = p[1] = p[2] = 0;
        if (i >= lit) {
            continue;
        }
        switch (c) {
        case COLOR_RAINBOW:
            hsv(hue + i * 256 / count, p, p + 1, p + 2);
            break;
        case COLOR_METER:                                           // green, yellow, red along the strip
            hsv(85 - i * 85 / count, p, p + 1, p + 2);
            break;
        case COLOR_SPECTRUM: {                                      // a red, green and blue bar, one per band. ponytail: fixed colours, tint from rgb if anyone asks
            int k = i * 3 / count, start = k * count / 3;
            if ((i - start) * 255 < bands[k] * ((k + 1) * count / 3 - start)) {
                p[k] = 255;
            }
            break;
        }
        default:
            p[0] = r; p[1] = g; p[2] = b;
        }
        sum += p[0] + p[1] + p[2];
    }
    last_cap = capped(sum, lvl);
    for (int i = 0; i < count; i++) {
        pixel(i, fb[i][0] * last_cap / 255, fb[i][1] * last_cap / 255, fb[i][2] * last_cap / 255);
    }
    led_strip_refresh(strip);
}
