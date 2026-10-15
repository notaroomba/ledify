// Minimal USB-PD sink on the FUSB302B: wait for Source_Capabilities, request the fixed PDO with the
// most power up to PD_WANT_MV (or the best one at 15 V+ while the 12 V output is on), and note PS_RDY.
#include <string.h>
#include "pd.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "pd";
#define FUSB_ADDR 0x22

enum { R_SW0 = 0x02, R_SW1 = 0x03, R_CTL0 = 0x06, R_CTL1 = 0x07, R_CTL3 = 0x09, R_MASK = 0x0A,
       R_POWER = 0x0B, R_RESET = 0x0C, R_MASKA = 0x0E, R_MASKB = 0x0F,
       R_STATUS0 = 0x40, R_STATUS1 = 0x41, R_FIFO = 0x43 };
enum { TK_TXON = 0xA1, TK_SYNC1 = 0x12, TK_SYNC2 = 0x13, TK_PACKSYM = 0x80,
       TK_JAMCRC = 0xFF, TK_EOP = 0x14, TK_TXOFF = 0xFE };
enum { CTRL_GOODCRC = 1, CTRL_ACCEPT = 3, CTRL_REJECT = 4, CTRL_PING = 5, CTRL_PS_RDY = 6,
       CTRL_GET_SRC_CAP = 7, CTRL_GET_SNK_CAP = 8, CTRL_SOFT_RESET = 13, CTRL_NOT_SUPPORTED = 16 };
enum { DATA_SRC_CAP = 1, DATA_REQUEST = 2, DATA_SNK_CAP = 4, DATA_VDM = 15 };

static i2c_master_dev_handle_t dev;
static uint8_t msg_id;
static pd_state_t st;
static int req_mv, req_ma;
static int pdo_pos[7];                   // object position of each stored PDO in the source's list
static int want_min_mv;                  // 15000 while the 12 V output is selected
static bool rerequest;                   // main asked for a different contract; the PD task sends it
static int64_t last_ask;
static int asks;
static void pd_poll(void);

static void wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    i2c_master_transmit(dev, b, 2, 50);
}

static uint8_t rd(uint8_t reg)
{
    uint8_t v = 0;
    i2c_master_transmit_receive(dev, &reg, 1, &v, 1, 50);
    return v;
}

static void rdn(uint8_t reg, uint8_t *buf, size_t n)
{
    i2c_master_transmit_receive(dev, &reg, 1, buf, n, 50);
}

static void send(uint8_t type, const uint32_t *objs, int n)
{
    uint8_t buf[48];
    int i = 0;
    buf[i++] = R_FIFO;
    buf[i++] = TK_SYNC1; buf[i++] = TK_SYNC1; buf[i++] = TK_SYNC1; buf[i++] = TK_SYNC2;
    buf[i++] = TK_PACKSYM | (2 + 4 * n);
    uint16_t hdr = type | (1 << 6) | (msg_id << 9) | (n << 12);   // PD 2.0, sink, UFP
    msg_id = (msg_id + 1) & 7;
    buf[i++] = hdr & 0xFF;
    buf[i++] = hdr >> 8;
    for (int k = 0; k < n; k++) {
        uint32_t o = objs[k];
        buf[i++] = o; buf[i++] = o >> 8; buf[i++] = o >> 16; buf[i++] = o >> 24;
    }
    buf[i++] = TK_JAMCRC; buf[i++] = TK_EOP; buf[i++] = TK_TXOFF; buf[i++] = TK_TXON;
    i2c_master_transmit(dev, buf, i, 50);
}

// Returns the number of data objects, or -1 when there is no SOP message waiting.
static int recv(uint16_t *hdr, uint32_t *objs)
{
    if (rd(R_STATUS1) & 0x20) {                 // RX_EMPTY
        return -1;
    }
    uint8_t tok = rd(R_FIFO);
    if ((tok & 0xE0) != 0xE0) {                 // not SOP: drop whatever this is
        ESP_LOGW(TAG, "rx token 0x%02x, flushing", tok);
        wr(R_CTL1, 0x04);
        return -1;
    }
    uint8_t h[2];
    rdn(R_FIFO, h, 2);
    *hdr = h[0] | (h[1] << 8);
    int n = (*hdr >> 12) & 7;
    uint8_t d[7 * 4 + 4];                       // objects + CRC
    rdn(R_FIFO, d, 4 * n + 4);
    for (int k = 0; k < n; k++) {
        objs[k] = d[4 * k] | (d[4 * k + 1] << 8) | (d[4 * k + 2] << 16) | ((uint32_t)d[4 * k + 3] << 24);
    }
    return n;
}

static void pd_task(void *arg)
{
    for (;;) {
        pd_poll();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void pd_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FUSB_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
        return;
    }
    wr(R_RESET, 0x03);
    vTaskDelay(pdMS_TO_TICKS(10));
    wr(R_POWER, 0x0F);

    // Which CC line has the cable? Measure each with Rd pull-downs on.
    int lvl[2];
    for (int c = 0; c < 2; c++) {
        wr(R_SW0, 0x03 | (c == 0 ? 0x04 : 0x08));
        vTaskDelay(pdMS_TO_TICKS(2));
        lvl[c] = rd(R_STATUS0) & 0x03;          // BC_LVL
    }
    st.vbus = rd(R_STATUS0) & 0x80;
    st.cc = (lvl[0] == 0 && lvl[1] == 0) ? 0 : (lvl[0] >= lvl[1] ? 1 : 2);
    if (!st.cc) {
        ESP_LOGI(TAG, "no PD source on CC, staying at 5 V");
        return;
    }
    wr(R_SW0, 0x03 | (st.cc == 1 ? 0x04 : 0x08));   // Rd on both, measure the active CC
    wr(R_SW1, 0x24 | (st.cc == 1 ? 0x01 : 0x02));   // auto GoodCRC, PD 2.0, transmit on active CC
    wr(R_CTL3, 0x07);                                // auto retry, 3 retries
    wr(R_CTL0, 0x04);                                // interrupts unmasked (we poll anyway)
    wr(R_MASK, 0); wr(R_MASKA, 0); wr(R_MASKB, 0);
    wr(R_CTL0, 0x44);                                // flush TX
    wr(R_CTL1, 0x04);                                // flush RX
    wr(R_RESET, 0x02);                               // PD reset: message ids start at 0
    msg_id = 0;
    // From here on the FUSB302 acknowledges the source's offers, and a Request must follow each
    // one within 30 ms or the source hard-resets (cuts VBUS). So poll from a task, not the main loop.
    xTaskCreate(pd_task, "pd", 3072, NULL, 6, NULL);
    send(CTRL_GET_SRC_CAP, NULL, 0);                 // in case the source already gave up offering
    last_ask = esp_timer_get_time();
    asks = 1;
    ESP_LOGI(TAG, "attached on CC%d (levels %d/%d), vbus %d, status0 0x%02x status1 0x%02x",
             st.cc, lvl[0], lvl[1], st.vbus, rd(R_STATUS0), rd(R_STATUS1));
}

// Request the fixed PDO with the most power (ties go to the higher voltage) between min_mv and PD_WANT_MV.
static bool request_best(int min_mv)
{
    int best = -1;
    long best_w = -1;
    for (int i = 0; i < st.n_pdos; i++) {
        int ma = st.pdo_ma[i] < PD_WANT_MA ? st.pdo_ma[i] : PD_WANT_MA;
        long w = (long)st.pdo_mv[i] * ma;
        if (st.pdo_mv[i] >= min_mv && st.pdo_mv[i] <= PD_WANT_MV &&
            (w > best_w || (w == best_w && st.pdo_mv[i] > st.pdo_mv[best]))) {
            best = i;
            best_w = w;
        }
    }
    if (best < 0) {
        return false;
    }
    req_mv = st.pdo_mv[best];
    req_ma = st.pdo_ma[best] < PD_WANT_MA ? st.pdo_ma[best] : PD_WANT_MA;
    // bit 25 = USB communications capable: without it macOS disables USB data after the contract
    uint32_t rdo = ((uint32_t)pdo_pos[best] << 28) | (1u << 25) | (1u << 24) |
                   ((uint32_t)(req_ma / 10) << 10) | (uint32_t)(req_ma / 10);
    send(DATA_REQUEST, &rdo, 1);
    ESP_LOGI(TAG, "requesting %d mV %d mA", req_mv, req_ma);
    return true;
}

bool pd_request(int min_mv)
{
    want_min_mv = min_mv;
    for (int i = 0; i < st.n_pdos; i++) {
        if (st.pdo_mv[i] >= min_mv && st.pdo_mv[i] <= PD_WANT_MV) {
            rerequest = true;
            return true;
        }
    }
    return false;
}

static void pd_poll(void)
{
    if (rerequest && st.n_pdos) {
        rerequest = false;
        st.ready = false;
        request_best(want_min_mv);
    }
    if (!dev || !st.cc) {
        return;
    }
    uint16_t hdr;
    uint32_t objs[7];
    int n = recv(&hdr, objs);
    if (n < 0) {
        uint8_t inta = rd(0x3E);
        if (inta & 0x01) {                           // I_HARDRST: the source reset the link
            ESP_LOGW(TAG, "hard reset from source");
            st.ready = false; st.n_pdos = 0; st.contract_mv = st.contract_ma = 0;
            wr(R_RESET, 0x02);
            msg_id = 0;
            asks = 0;
            last_ask = esp_timer_get_time();
        }
        static int64_t last_dbg;
        if (!st.ready && esp_timer_get_time() - last_dbg > 500000) {
            last_dbg = esp_timer_get_time();
            ESP_LOGI(TAG, "status0 0x%02x status1 0x%02x int 0x%02x intA 0x%02x intB 0x%02x",
                     rd(R_STATUS0), rd(R_STATUS1), rd(0x42), inta, rd(0x3F));
        }
        // nothing offered yet: ask again a few times, 2 s apart
        if (!st.n_pdos && asks < 5 && esp_timer_get_time() - last_ask > 2000000) {
            send(CTRL_GET_SRC_CAP, NULL, 0);
            last_ask = esp_timer_get_time();
            asks++;
            ESP_LOGI(TAG, "asked for source capabilities again (%d), status0 0x%02x status1 0x%02x",
                     asks, rd(R_STATUS0), rd(R_STATUS1));
        }
        return;
    }
    int type = hdr & 0x0F;
    ESP_LOGI(TAG, "rx header 0x%04x type %d objects %d", hdr, type, n);

    if (n > 0 && type == DATA_SRC_CAP) {
        st.n_pdos = 0;
        for (int i = 0; i < n && st.n_pdos < 7; i++) {
            if (objs[i] >> 30) {
                ESP_LOGI(TAG, "pdo %d: type %lu (battery/variable/augmented), skipped", i + 1, (unsigned long)(objs[i] >> 30));
                continue;                            // fixed-supply PDOs only
            }
            st.pdo_mv[st.n_pdos] = ((objs[i] >> 10) & 0x3FF) * 50;
            st.pdo_ma[st.n_pdos] = (objs[i] & 0x3FF) * 10;
            pdo_pos[st.n_pdos] = i + 1;              // the Request names the PDO by its 1-based position
            st.n_pdos++;
        }
        request_best(want_min_mv);
    } else if (n == 0 && type == CTRL_PS_RDY) {
        st.contract_mv = req_mv;
        st.contract_ma = req_ma;
        st.ready = true;
        ESP_LOGI(TAG, "contract: %d mV %d mA", st.contract_mv, st.contract_ma);
    } else if (n == 0 && type == CTRL_REJECT) {
        st.contract_mv = 5000;                       // the source keeps vSafe5V; its first PDO says how much current
        st.contract_ma = st.n_pdos && st.pdo_mv[0] == 5000 ? st.pdo_ma[0] : 500;
        st.ready = true;
        ESP_LOGW(TAG, "request rejected, staying at 5 V %d mA", st.contract_ma);
    } else if (n == 0 && type == CTRL_GET_SNK_CAP) {
        uint32_t pdo = (1u << 26) | (100u << 10) | 300;   // sink 5 V 3 A, USB communications capable
        send(DATA_SNK_CAP, &pdo, 1);
    } else if (n == 0 && type == CTRL_SOFT_RESET) {
        msg_id = 0;
        send(CTRL_ACCEPT, NULL, 0);
    } else if (n > 0 && type == DATA_VDM) {
        // structured VDM command from the source (e.g. Discover Identity): answer NAK so it stops asking
        if ((objs[0] & (1u << 15)) && ((objs[0] >> 6) & 3) == 0) {
            uint32_t nak = (objs[0] & ~(3u << 6)) | (2u << 6);
            send(DATA_VDM, &nak, 1);
        }
    } else if (type != CTRL_GOODCRC && type != CTRL_ACCEPT && type != CTRL_PING && ((hdr >> 6) & 3) == 2) {
        send(CTRL_NOT_SUPPORTED, NULL, 0);            // PD 3.0 source: tell it we don't do this one
    }
}

const pd_state_t *pd_get(void)
{
    return &st;
}
