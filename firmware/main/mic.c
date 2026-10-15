// ICS-43434 I2S microphone, left channel, 32 kHz (its normal-power clock range).
#include <math.h>
#include "mic.h"
#include "board.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static i2s_chan_handle_t rx;
static int32_t buf[256];

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
        i2s_channel_read(rx, buf, sizeof buf, &n, 100);
    }
    return true;
}

float mic_level(void)
{
    size_t n = 0;
    if (!rx || i2s_channel_read(rx, buf, sizeof buf, &n, 100) != ESP_OK || n < 8) {
        return 0;
    }
    int cnt = n / 4;
    double mean = 0, acc = 0;
    for (int i = 0; i < cnt; i++) {
        mean += (buf[i] >> 8) / 8388608.0f;          // 24-bit sample, -1..1
    }
    mean /= cnt;
    for (int i = 0; i < cnt; i++) {
        float v = (buf[i] >> 8) / 8388608.0f - mean; // drop the mic's DC offset, it is not sound
        acc += v * v;
    }
    return sqrtf(acc / cnt);
}

// Approximate dB SPL: a full-scale sine is MIC_DB_FULL_SCALE, and RMS of a full-scale sine is 1/sqrt(2).
float mic_db(void)
{
    float r = mic_level();
    float db = r > 0 ? MIC_DB_FULL_SCALE + 20 * log10f(r * 1.4142f) : 0;
    return db < MIC_DB_FLOOR ? MIC_DB_FLOOR : db;
}
