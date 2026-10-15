#pragma once
#include <stdbool.h>
#include <stddef.h>
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
typedef enum { COLOR_SOLID, COLOR_RAINBOW, COLOR_TEMP, COLOR_SPECTRUM, COLOR_METER, COLOR_PALETTE, COLOR_STREAM } leds_color_t;
#define LED_COLORS "solid|rainbow|temp|spectrum|meter|palette"
#define LED_MAX_STOPS 16
void leds_set_palette(const uint8_t (*stops)[3], const uint8_t *pos, int n);   // 1..LED_MAX_STOPS colour stops; pos = % along the strip per stop, NULL spreads them evenly
void leds_palette_str(char *out, size_t n);     // the stops as "rrggbb@pct,..."
void leds_set_droop(uint8_t pct);               // tail colour fix 0..100: cut red along the strip against voltage sag, 0 = off
// lvl: brightness after every modifier. lit: pixels on from the start of the strip (count for all of them).
// bands: bass/mid/treble 0..255 for the spectrum bars.
// instant: no transition (sound-reactive styles and LAN streams are already moving).
void leds_frame(leds_color_t c, uint8_t lvl, uint16_t lit, const uint8_t bands[3], bool instant);
const char *leds_color_name(leds_color_t c);
bool leds_animated(leds_color_t c);             // the strip or a segment moves on its own (rainbow): redraw every frame

// Transitions: colour and brightness ease towards each new frame; the main loop keeps drawing until settled.
void leds_set_fade(uint16_t ms);                // about how long a change takes, 0 = instant
bool leds_settled(void);

// Segments: zones [a,b) with their own colour source and solid colour; pixels outside every zone use the strip's.
#define LED_MAX_SEGS 6
bool leds_set_segment(int i, uint16_t a, uint16_t b, leds_color_t c, uint8_t r, uint8_t g, uint8_t bl);
void leds_clear_segment(int i);
void leds_clear_segments(void);
int leds_segments_json(char *out, size_t n);    // [{"i":1,"a":0,"b":100,"color":"solid","rgb":"ff0000"},...]
int leds_segments_str(char *out, size_t n);     // compact form for NVS: "1:0-100:solid:ff0000;..."
void leds_load_segments(const char *s);

// LAN pixel streams (DDP, E1.31): pixels land here and COLOR_STREAM shows them while they keep coming.
void leds_stream_pixels(uint16_t first, const uint8_t *rgb, int npx);
bool leds_stream_active(void);
