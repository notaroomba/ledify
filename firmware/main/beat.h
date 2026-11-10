#pragma once
#include <stdbool.h>
#include <stdint.h>

// Tempo tracking and beat prediction. The mic task feeds an onset-strength value every 16 ms (spectral flux) and
// every detected onset; the tracker finds the tempo by autocorrelation over the last 4 s, the beat phase with a
// comb over the same history, nudges the phase with each onset, and says when the next beat is due so the LEDs
// can be lit for it slightly early, landing on the beat instead of trailing it.
// Plain C with no ESP-IDF calls: test/beat_test.c runs it on the host.

#define BEAT_FRAME_US 16000          // one onset-strength value per FFT frame

void beat_reset(void);
void beat_odf(uint32_t strength, int64_t t_us);   // one onset-strength value, t_us = end of its window
void beat_onset(int64_t t_us);                    // an onset from the fast loudness detector
// True once per predicted beat, as soon as now_us + lead_us reaches it. Only while locked.
bool beat_due(int64_t now_us, int32_t lead_us);
bool beat_locked(void);
uint16_t beat_bpm(void);              // current tempo estimate, 0 before there is one
uint8_t beat_lock(void);              // 0..100 confidence, shown in status
