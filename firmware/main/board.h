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
#define LED_MW_PER_CHANNEL 100   // one colour at full on one pixel; ponytail: tune per strip if the cap is off
#define LED_BUDGET_NO_PD_MW 7500 // no PD contract: assume a 5 V 1.5 A port
#define LED_BUCK_MAX_MA 10000    // LM61495 output limit
#define LED_BUCK_EFF_PCT 85      // what makes it from the PD contract to the strip
#define PD_WANT_MV  20000   // highest PD voltage we accept
#define PD_WANT_MA   5000   // most current we ask for
#define LED_POWER_SETTLE_MS 100     // buck soft start + pixel power-on reset before the first frame
#define PD_START_DELAY_MS 2000  // debug: give the USB console time to open before PD talks
