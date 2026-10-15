#pragma once

// Ledify rev 1 pin map (ESP32-C6 GPIO numbers)
#define PIN_LED_DATA    5   // LED_CON, the SIG pad
#define PIN_LED_ENABLE  3   // LED buck enable, high = on
#define PIN_LED_12V     2   // low = 5 V, high = 12 V
#define PIN_I2C_SDA    10   // FUSB302 + OPT4001
#define PIN_I2C_SCL    11
#define PIN_PD_INT     16   // FUSB302 interrupt, polled instead
#define PIN_SPI_MISO   18
#define PIN_SPI_MOSI   19
#define PIN_SPI_SCK    20
#define PIN_CS_IMU     21   // BMI323
#define PIN_CS_BARO     7   // BMP581
#define PIN_MIC_SCK    23   // ICS-43434 bit clock
#define PIN_MIC_WS     22   // ICS-43434 word select
#define PIN_MIC_SD      6   // ICS-43434 data
#define MIC_DB_FULL_SCALE 120  // ICS-43434: 94 dB SPL reads -26 dBFS, so 0 dBFS is 120 dB SPL; ponytail: no A-weighting
#define MIC_DB_FLOOR 30        // below the mic's own noise floor the number means nothing

#define NUM_LEDS      300   // pixels driven until "count" is set
#define MAX_LEDS     1000   // most the firmware will drive (3 KB pixel buffer, 30 ms per frame)
#define PUBLISH_MS   5000   // state + sensors to MQTT this often (the panel shows them live)
#define FRAME_MS       33   // live styles redraw at 30 fps; 300 pixels take 9 ms of that on the wire
// Per-pixel draw, FastLED's WS2812 power model at 5 V: 16/11/15 mA per channel at full, 1 mA dark. WLED's limiter
// uses the same idea with 55 mA per full-white pixel. ponytail: measure your strip and adjust if the cap is off.
#define LED_RED_MW    80
#define LED_GREEN_MW  55
#define LED_BLUE_MW   75
#define LED_DARK_MW    5
#define MIC_FEED_HOLD_MS 1000   // audio fed from the panel overrides the mic for this long after the last packet
#define LIVE_MS        100      // "live N" streams loudness/bands to MQTT this often
#define LED_BUDGET_NO_PD_MW 7500 // no PD contract: assume a 5 V 1.5 A port
#define LED_BUCK_MAX_MA 10000    // LM61495 output limit
#define LED_BUCK_EFF_PCT 85      // what makes it from the PD contract to the strip
#define PD_WANT_MV  20000   // highest PD voltage we accept
#define PD_WANT_MA   5000   // most current we ask for
#define LED_POWER_SETTLE_MS 100     // buck soft start + pixel power-on reset before the first frame
#define WIFI_FAILS_BEFORE_SWITCH 4  // failed joins in a row before trying the fallback network
#define PD_START_DELAY_MS 100   // the source starts sending capabilities right away; answer them promptly
