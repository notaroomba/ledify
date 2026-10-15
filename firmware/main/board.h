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

#define NUM_LEDS       30   // length of the strip
#define PD_WANT_MV  20000   // highest PD voltage we accept
#define PD_WANT_MA   5000   // most current we ask for
#define LED_POWER_SETTLE_MS 100     // buck soft start + pixel power-on reset before the first frame
#define PD_START_DELAY_MS 2000  // debug: give the USB console time to open before PD talks
