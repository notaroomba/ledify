// OPT4001 light sensor (I2C), BMI323 IMU and BMP581 barometer (SPI).
#include <string.h>
#include "sensors.h"
#include "board.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sensors";
static i2c_master_dev_handle_t opt;
static spi_device_handle_t imu, baro;
static int baro_dummy;                 // BMP581 reads may need one dummy byte after the address
static bool have_opt, have_imu, have_baro;

// OPT4001: 16-bit registers, big endian
static void opt_wr(uint8_t reg, uint16_t v)
{
    uint8_t b[3] = {reg, v >> 8, v & 0xFF};
    i2c_master_transmit(opt, b, 3, 50);
}

static uint16_t opt_rd(uint8_t reg)
{
    uint8_t b[2] = {0, 0};
    i2c_master_transmit_receive(opt, &reg, 1, b, 2, 50);
    return (b[0] << 8) | b[1];
}

static void spi_xfer(spi_device_handle_t h, const uint8_t *tx, uint8_t *rx, int n)
{
    spi_transaction_t t = { .length = 8 * n, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(h, &t);
}

static void spi_rd(spi_device_handle_t h, uint8_t reg, int dummy, uint8_t *out, int n)
{
    uint8_t tx[32] = {0}, rx[32] = {0};
    tx[0] = reg | 0x80;
    spi_xfer(h, tx, rx, 1 + dummy + n);
    memcpy(out, rx + 1 + dummy, n);
}

static void spi_wr(spi_device_handle_t h, uint8_t reg, const uint8_t *data, int n)
{
    uint8_t tx[8];
    tx[0] = reg & 0x7F;
    memcpy(tx + 1, data, n);
    spi_xfer(h, tx, NULL, 1 + n);
}

void sensors_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x44,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &opt) == ESP_OK) {
        have_opt = (opt_rd(0x11) & 0xFFF) == 0x121;
        if (have_opt) {
            opt_wr(0x0A, 0x3238);          // auto range, 100 ms conversions, continuous
        }
    }

    spi_bus_config_t bc = {
        .mosi_io_num = PIN_SPI_MOSI, .miso_io_num = PIN_SPI_MISO, .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 64,
    };
    if (spi_bus_initialize(SPI2_HOST, &bc, SPI_DMA_DISABLED) != ESP_OK) {
        ESP_LOGW(TAG, "spi bus init failed");
        return;
    }
    spi_device_interface_config_t dc = { .clock_speed_hz = 1000000, .mode = 0, .queue_size = 1 };
    dc.spics_io_num = PIN_CS_IMU;
    spi_bus_add_device(SPI2_HOST, &dc, &imu);
    dc.spics_io_num = PIN_CS_BARO;
    spi_bus_add_device(SPI2_HOST, &dc, &baro);

    uint8_t b[2];
    spi_rd(imu, 0x00, 1, b, 2);            // first access switches the BMI323 to SPI
    vTaskDelay(pdMS_TO_TICKS(2));
    spi_rd(imu, 0x00, 1, b, 2);
    have_imu = (b[0] == 0x43);
    if (have_imu) {
        uint8_t acc[2] = {0x18, 0x40};     // normal mode, +/-4 g, 100 Hz
        uint8_t gyr[2] = {0x48, 0x40};     // normal mode, +/-2000 dps, 100 Hz
        spi_wr(imu, 0x20, acc, 2);
        spi_wr(imu, 0x21, gyr, 2);
    }

    spi_rd(baro, 0x01, 0, b, 1);           // first access switches the BMP581 to SPI
    vTaskDelay(pdMS_TO_TICKS(2));
    for (baro_dummy = 0; baro_dummy < 2; baro_dummy++) {
        spi_rd(baro, 0x01, baro_dummy, b, 1);
        if (b[0] == 0x50) {
            have_baro = true;
            break;
        }
    }
    if (have_baro) {
        uint8_t v = 0x40 | (3 << 3) | 1;   // pressure on, 8x / 2x oversampling
        spi_wr(baro, 0x36, &v, 1);
        v = 0x80 | (0x17 << 2) | 1;        // 10 Hz, normal mode
        spi_wr(baro, 0x37, &v, 1);
    }
    ESP_LOGI(TAG, "light sensor %d, imu %d, barometer %d", have_opt, have_imu, have_baro);
}

void sensors_read(sensor_data_t *d)
{
    memset(d, 0, sizeof *d);
    if (have_opt) {
        uint16_t m = opt_rd(0x00), l = opt_rd(0x01);
        uint32_t mant = ((uint32_t)(m & 0xFFF) << 8) | (l >> 8);
        d->lux = (float)(mant << (m >> 12)) * 0.0004f;    // DNP package scale
        d->have_lux = true;
    }
    if (have_imu) {
        uint8_t b[12];
        spi_rd(imu, 0x03, 1, b, 12);
        for (int i = 0; i < 3; i++) {
            d->acc[i] = (int16_t)(b[2 * i] | (b[2 * i + 1] << 8)) / 8192.0f;
            d->gyr[i] = (int16_t)(b[6 + 2 * i] | (b[7 + 2 * i] << 8)) / 16.384f;
        }
        d->have_imu = true;
    }
    if (have_baro) {
        uint8_t b[6];
        spi_rd(baro, 0x1D, baro_dummy, b, 6);
        int32_t t = b[0] | (b[1] << 8) | (b[2] << 16);
        if (t & 0x800000) {
            t -= 1 << 24;
        }
        uint32_t p = b[3] | (b[4] << 8) | (b[5] << 16);
        d->temp_c = t / 65536.0f;
        d->pressure_pa = p / 64.0f;
        d->have_baro = true;
    }
}
