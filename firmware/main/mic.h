#pragma once
#include <stdbool.h>
#include <stdint.h>

// ICS-43434 capture runs in its own task; everything here returns the latest value without waiting.
bool mic_init(void);
float mic_level(void);          // RMS of the last 8 ms, 0..1 of full scale (status and the sound sensor)
void mic_set_gain(uint8_t g);   // 1..100: how much a quiet room is amplified
uint8_t mic_loud(void);         // 0..255: RMS x gain, held under a peak follower so loud rooms don't pin at 255
uint8_t mic_pulse(void);        // 0..255: 255 at a beat onset, fades over about 300 ms
void mic_bands(uint8_t out[3]); // bass, mid, treble 0..255, each against its own recent peak
