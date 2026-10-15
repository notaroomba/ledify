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
static uint8_t hue;
static void show(uint8_t r, uint8_t g, uint8_t b, uint8_t lvl);
static uint8_t capped(uint32_t sum_rgb, uint8_t lvl);

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
            show(cur_r, cur_g, cur_b, level);
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
        show(cur_r, cur_g, cur_b, level);     // restore the last colour; it was cleared at power off
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
    leds_fill(cur_r, cur_g, cur_b);
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
    show(cur_r, cur_g, cur_b, level);
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
    return capped(cur_r + cur_g + cur_b, level);
}

// Scale a level down until the frame fits the power budget: count pixels at (r+g+b)/765 of full white each.
static uint8_t capped(uint32_t sum_rgb, uint8_t lvl)
{
    uint32_t need = (uint32_t)count * sum_rgb / 255 * lvl * LED_MW_PER_CHANNEL / 255;   // mW at this level
    return need > budget_mw ? lvl * budget_mw / need : lvl;
}

static void show(uint8_t r, uint8_t g, uint8_t b, uint8_t lvl)
{
    lvl = capped(r + g + b, lvl);
    for (int i = 0; i < count; i++) {
        pixel(i, r * lvl / 255, g * lvl / 255, b * lvl / 255);
    }
    led_strip_refresh(strip);
}

void leds_fill(uint8_t r, uint8_t g, uint8_t b)
{
    cur_r = r; cur_g = g; cur_b = b;
    show(r, g, b, level);
}

void leds_get_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = cur_r; *g = cur_g; *b = cur_b;
}

void leds_scaled(float f)
{
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    show(cur_r, cur_g, cur_b, (uint8_t)(level * f));
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

void leds_rainbow_step(void)
{
    hue++;
    uint8_t lvl = capped(383, level);           // a full rainbow averages r+g+b = 383
    for (int i = 0; i < count; i++) {
        uint8_t r, g, b;
        hsv(hue + i * 256 / count, &r, &g, &b);
        pixel(i, r * lvl / 255, g * lvl / 255, b * lvl / 255);
    }
    led_strip_refresh(strip);
}

void leds_refresh(void)
{
    show(cur_r, cur_g, cur_b, level);
}
