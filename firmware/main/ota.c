// Over-the-air update: "ota URL" (from BLE, the console or MQTT) fetches an app image into the spare
// OTA slot and restarts. The bootloader boots the new image once; main.c calls ota_mark_ok() after the
// board has been up for a while, otherwise the next reset falls back to the previous image.
#include <string.h>
#include "ota.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";
static char url[160];
static bool running;

static void ota_task(void *arg)
{
    esp_http_client_config_t http = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,   // https:// against the public CA bundle; http:// is allowed too
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err == ESP_OK) {
        int total = esp_https_ota_get_image_size(h), last = -1;
        while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int pct = total > 0 ? esp_https_ota_get_image_len_read(h) * 100 / total : 0;
            if (pct / 10 != last) {
                last = pct / 10;
                ESP_LOGI(TAG, "%d%%", pct);
            }
        }
        if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
            err = esp_https_ota_finish(h);                // verifies the image and sets it as the next boot
        } else {
            esp_https_ota_abort(h);
            if (err == ESP_OK) {
                err = ESP_FAIL;
            }
        }
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "update written, restarting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGE(TAG, "failed: %s", esp_err_to_name(err));
    running = false;
    vTaskDelete(NULL);
}

bool ota_start(const char *u)
{
    if (running || strlen(u) >= sizeof url || strncmp(u, "http", 4)) {
        return false;
    }
    strcpy(url, u);
    running = true;
    return xTaskCreate(ota_task, "ota", 10240, NULL, 5, NULL) == pdPASS;   // TLS handshake needs the stack
}

void ota_mark_ok(void)
{
    esp_ota_mark_app_valid_cancel_rollback();
}
