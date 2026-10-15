// Nordic UART service on NimBLE: text commands in on RX, text replies out on TX.
#include <string.h>
#include "ble.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";
static QueueHandle_t cmd_q;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t tx_handle;
static uint8_t own_addr_type;

// 6E400001-B5A3-F393-E0A9-E50E24DCCA9E and friends, least significant byte first
static const ble_uuid128_t UUID_SVC = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t UUID_RX = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t UUID_TX = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        cmd_t cmd = {0};
        uint16_t len = 0;
        ble_hs_mbuf_to_flat(ctxt->om, cmd.text, sizeof cmd.text - 1, &len);
        cmd.text[len] = 0;
        xQueueSend(cmd_q, &cmd, 0);
    }
    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;                                   // notify only
}

static const struct ble_gatt_svc_def svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &UUID_RX.u, .access_cb = rx_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &UUID_TX.u, .access_cb = tx_access, .val_handle = &tx_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *ev, void *arg);

static void advertise(void)
{
    struct ble_hs_adv_fields f = {0};
    const char *name = ble_svc_gap_device_name();
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)name;
    f.name_len = strlen(name);
    f.name_is_complete = 1;
    f.uuids128 = (ble_uuid128_t *)&UUID_SVC;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&f);

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            conn_handle = ev->connect.conn_handle;
            ESP_LOGI(TAG, "connected");
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "disconnected");
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &own_addr_type);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_init(QueueHandle_t q)
{
    cmd_q = q;
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(svcs);
    ble_gatts_add_svcs(svcs);
    ble_svc_gap_device_name_set("Ledify");
    ble_att_set_preferred_mtu(256);
    nimble_port_freertos_init(host_task);
}

bool ble_connected(void)
{
    return conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

void ble_send(const char *s)
{
    if (!ble_connected()) {
        return;
    }
    int mtu = ble_att_mtu(conn_handle) - 3;
    if (mtu < 20) {
        mtu = 20;
    }
    size_t len = strlen(s);
    for (size_t off = 0; off < len; off += mtu) {
        size_t n = len - off < (size_t)mtu ? len - off : (size_t)mtu;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s + off, n);
        if (!om || ble_gatts_notify_custom(conn_handle, tx_handle, om) != 0) {
            break;
        }
    }
}
