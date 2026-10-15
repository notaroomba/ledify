// ICS-43434 I2S microphone, left channel, 32 kHz (its normal-power clock range), and the sound analysis the
// light styles read: loudness with auto gain, beat onsets and bass/mid/treble bands. A task takes 256 samples
// (8 ms) at a time so the main loop never waits for audio.
#include <math.h>
#include <string.h>
#include "mic.h"
#include "board.h"
#include "fft.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define N 256
#define FULL 32767             // RMS x gain at this value is "as loud as it gets"
#define PEAK_DECAY 8           // peak follower loses 1/256 per block: about 2 s to fall by e
#define BEAT_RATIO 150         // onset: a block this % above the 1 s running average...
#define BEAT_MIN 2048          // ...and above this (gain applied), so a quiet room never beats
#define BEAT_HOLD 12           // blocks (100 ms) before the next onset counts

static i2s_chan_handle_t rx;
static int32_t raw[N];         // one I2S read, 24-bit samples in the top bits
static int16_t win[N];         // last 16 ms decimated to 16 kHz: the FFT input
static uint8_t gain = 25;
static volatile uint16_t rms;  // Q15, latest block
static volatile uint8_t loud, pulse, bands[3];
static uint32_t peak, bpeak[3];
static int32_t avg;
static int hold, nblock;

// One 8 ms block: RMS -> gain -> peak follower -> loudness, onset detection, and the FFT bands every 4th block.
static void block(void)
{
    int64_t sum = 0, acc = 0;
    for (int i = 0; i < N; i++) {
        sum += raw[i] >> 8;
    }
    int32_t mean = sum / N;                                  // the mic's DC offset is not sound
    for (int i = 0; i < N; i++) {
        int32_t s = (raw[i] >> 8) - mean;
        acc += (int64_t)s * s;
    }
    rms = (uint32_t)sqrtf((float)(acc / N)) >> 8;
    uint32_t x = (uint32_t)rms * gain;
    peak = x > peak ? x : peak - (peak >> PEAK_DECAY);
    uint32_t l = x * 255 / (peak > FULL ? peak : FULL);
    loud = l > loud ? l : loud - ((loud - l + 7) >> 3);     // instant attack, ~60 ms release
    avg += ((int32_t)x - avg) / 128;
    if (hold) {
        hold--;
    }
    if (!hold && x > BEAT_MIN && x * 100 > (uint32_t)avg * BEAT_RATIO) {
        pulse = 255;
        hold = BEAT_HOLD;
    } else {
        pulse -= (pulse + 15) >> 4;
    }
    memmove(win, win + N / 2, N / 2 * sizeof win[0]);
    for (int i = 0; i < N / 2; i++) {
        win[N / 2 + i] = ((raw[2 * i] >> 16) + (raw[2 * i + 1] >> 16)) / 2;
    }
    if (++nblock % 4 == 0) {
        uint32_t m[3];
        fft_bands(win, m);
        for (int k = 0; k < 3; k++) {
            uint32_t b = (m[k] >> 5) * gain;                // onto the RMS scale, roughly
            bpeak[k] = b > bpeak[k] ? b : bpeak[k] - (bpeak[k] >> 6);
            bands[k] = (uint64_t)b * 255 / (bpeak[k] > FULL ? bpeak[k] : FULL);
        }
    }
}

static void mic_task(void *arg)
{
    size_t n;
    for (;;) {
        if (i2s_channel_read(rx, raw, sizeof raw, &n, 1000) == ESP_OK && n == sizeof raw) {
            block();
        }
    }
}

bool mic_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, NULL, &rx) != ESP_OK) {
        return false;
    }
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(32000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = PIN_MIC_SCK, .ws = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED, .din = PIN_MIC_SD,
        },
    };
    cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;      // L/R pin is tied low
    if (i2s_channel_init_std_mode(rx, &cfg) != ESP_OK) {
        return false;
    }
    if (i2s_channel_enable(rx) != ESP_OK) {
        return false;
    }
    size_t n;
    for (int i = 0; i < 12; i++) {                   // discard ~100 ms: the mic's DC filter settles after power-on
        i2s_channel_read(rx, raw, sizeof raw, &n, 100);
    }
    xTaskCreate(mic_task, "mic", 4096, NULL, 4, NULL);
    return true;
}

float mic_level(void)
{
    return rms / 32768.0f;
}

void mic_set_gain(uint8_t g)
{
    gain = g ? g : 1;
}

uint8_t mic_loud(void)
{
    return loud;
}

uint8_t mic_pulse(void)
{
    return pulse;
}

void mic_bands(uint8_t out[3])
{
    for (int k = 0; k < 3; k++) {
        out[k] = bands[k];
    }
}

// Approximate dB SPL: a full-scale sine is MIC_DB_FULL_SCALE, and RMS of a full-scale sine is 1/sqrt(2).
float mic_db(void)
{
    float r = mic_level();
    float db = r > 0 ? MIC_DB_FULL_SCALE + 20 * log10f(r * 1.4142f) : 0;
    return db < MIC_DB_FLOOR ? MIC_DB_FLOOR : db;
}
