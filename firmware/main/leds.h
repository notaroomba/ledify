#pragma once
#include <stdbool.h>
#include <stdint.h>

void leds_init(void);
bool leds_set_type(const char *name);           // ws2812 | ws2811 | sk6812 | sk6812w | ws2816
const char *leds_type_name(void);
#define LED_TYPES "ws2812|ws2811|sk6812|sk6812w|ws2816"
void leds_power(bool on);                       // LED buck enable
void leds_select_12v(bool v);                   // turns the output off first
void leds_set_level(uint8_t level);             // brightness cap 0..255
uint8_t leds_get_level(void);
void leds_set_count(uint8_t n);                 // pixels driven, 1..NUM_LEDS; the rest stay dark
uint8_t leds_get_count(void);
void leds_fill(uint8_t r, uint8_t g, uint8_t b);
void leds_get_color(uint8_t *r, uint8_t *g, uint8_t *b);
void leds_scaled(float f);                      // current colour scaled by 0..1, for sound mode
void leds_rainbow_step(void);
void leds_refresh(void);                        // resend the current frame
