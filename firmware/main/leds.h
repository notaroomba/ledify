#pragma once
#include <stdbool.h>
#include <stdint.h>

void leds_init(void);
bool leds_set_type(const char *name);           // ws2812 | ws2811 | sk6812 | sk6812w | ws2816
const char *leds_type_name(void);
#define LED_TYPES "ws2812|ws2811|sk6812|sk6812w|ws2816"
void leds_power(bool on);                       // LED buck enable; draw a frame after turning on
void leds_select_12v(bool v);                   // turns the output off first
void leds_set_level(uint8_t level);             // brightness cap 0..255
uint8_t leds_get_level(void);
void leds_set_count(uint16_t n);                // pixels driven, 1..MAX_LEDS
uint16_t leds_get_count(void);
void leds_set_budget_mw(uint32_t mw);           // power available to the strip; every frame is capped to fit
uint8_t leds_get_cap(void);
void leds_set_limit(bool on);                   // scale frames to the budget; off = send what was asked and just report
uint32_t leds_get_need_mw(void);                // estimated draw of the last frame at the requested level
uint32_t leds_get_budget_mw(void);                     // the level the last frame actually went out at
void leds_fill(uint8_t r, uint8_t g, uint8_t b);   // the solid colour
void leds_get_color(uint8_t *r, uint8_t *g, uint8_t *b);
void leds_set_temp(uint8_t pos);                // temperature colour: 0 blue, 128 white, 255 red

// Colour sources. leds_frame() is the only way pixels go out, so every style passes the power cap. The setters
// above only change state; whoever changes something draws the next frame.
typedef enum { COLOR_SOLID, COLOR_RAINBOW, COLOR_TEMP, COLOR_SPECTRUM, COLOR_METER } leds_color_t;
#define LED_COLORS "solid|rainbow|temp|spectrum|meter"
// lvl: brightness after every modifier. lit: pixels on from the start of the strip (count for all of them).
// bands: bass/mid/treble 0..255 for the spectrum bars.
void leds_frame(leds_color_t c, uint8_t lvl, uint16_t lit, const uint8_t bands[3]);
