#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "web.h"
#include "cmd.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "mdns.h"
#include "nvs.h"
#include "lwip/sockets.h"

static const char *TAG = "web";
static QueueHandle_t cmd_q;
static httpd_handle_t hd;
static char name[32] = "ledify", id[16];
#define MAX_WS 4
static int clients[MAX_WS];
static int nclients;

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[] asm("_binary_app_js_gz_end");
extern const uint8_t app_css_gz_start[] asm("_binary_app_css_gz_start");
extern const uint8_t app_css_gz_end[] asm("_binary_app_css_gz_end");
extern const uint8_t icon_svg_start[] asm("_binary_icon_svg_start");
extern const uint8_t icon_svg_end[] asm("_binary_icon_svg_end");
extern const uint8_t manifest_start[] asm("_binary_manifest_webmanifest_start");
extern const uint8_t manifest_end[] asm("_binary_manifest_webmanifest_end");

static esp_err_t send_blob(httpd_req_t *req, const char *type, const uint8_t *start, const uint8_t *end, bool gz)
{
    httpd_resp_set_type(req, type);
    if (gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t h_index(httpd_req_t *r) { return send_blob(r, "text/html", index_html_gz_start, index_html_gz_end, true); }
static esp_err_t h_js(httpd_req_t *r) { return send_blob(r, "application/javascript", app_js_gz_start, app_js_gz_end, true); }
static esp_err_t h_css(httpd_req_t *r) { return send_blob(r, "text/css", app_css_gz_start, app_css_gz_end, true); }
static esp_err_t h_icon(httpd_req_t *r) { return send_blob(r, "image/svg+xml", icon_svg_start, icon_svg_end, false); }
static esp_err_t h_manifest(httpd_req_t *r) { return send_blob(r, "application/manifest+json", manifest_start, manifest_end, false); }

static esp_err_t h_config(httpd_req_t *r)
{
    char buf[128];
    snprintf(buf, sizeof buf, "window.LEDIFY_CONFIG={local:true,id:\"%s\",name:\"%s\"};", id, name);
    httpd_resp_set_type(r, "application/javascript");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    return httpd_resp_send(r, buf, HTTPD_RESP_USE_STRLEN);
}

static void drop(int fd)
{
    for (int i = 0; i < nclients; i++) {
        if (clients[i] == fd) {
            clients[i] = clients[--nclients];
            return;
        }
    }
}

static esp_err_t h_ws(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {                                   // handshake done: remember the socket
        if (nclients < MAX_WS) {
            clients[nclients++] = fd;
        }
        ESP_LOGI(TAG, "ws client %d connected (%d)", fd, nclients);
        return ESP_OK;
    }
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT };
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    if (f.type == HTTPD_WS_TYPE_CLOSE) {
        drop(fd);
        return ESP_OK;
    }
    if (f.type != HTTPD_WS_TYPE_TEXT || !f.len) {
        return ESP_OK;
    }
    cmd_t c = {0};
    uint8_t buf[sizeof c.text];
    if (f.len >= sizeof buf) {
        return ESP_OK;
    }
    f.payload = buf;
    if (httpd_ws_recv_frame(req, &f, f.len) != ESP_OK) {
        return ESP_FAIL;
    }
    memcpy(c.text, buf, f.len);
    xQueueSend(cmd_q, &c, 0);
    return ESP_OK;
}

static void on_close(httpd_handle_t h, int fd)
{
    drop(fd);
    close(fd);
}

static void send_all(httpd_ws_frame_t *f)
{
    for (int i = nclients - 1; i >= 0; i--) {
        if (httpd_ws_send_data(hd, clients[i], f) != ESP_OK) {
            drop(clients[i]);
        }
    }
}

static void send_text(const char *what, const char *payload)
{
    if (!hd || !nclients) {
        return;
    }
    static char buf[2600];
    int n = snprintf(buf, sizeof buf, "%s %s", what, payload);
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)buf, .len = n < (int)sizeof buf ? n : sizeof buf - 1 };
    send_all(&f);
}

void web_publish(const char *light_json, const char *status_json, const char *sensors_json)
{
    send_text("light/state", light_json);
    send_text("state", status_json);
    send_text("sensors", sensors_json);
}

void web_publish_live(const char *json)
{
    send_text("live", json);
}

void web_publish_reply(const char *text)
{
    send_text("reply", text);
}

void web_publish_audio(const void *pcm, int len)
{
    if (!hd || !nclients) {
        return;
    }
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_BINARY, .payload = (uint8_t *)pcm, .len = len };
    send_all(&f);
}

int web_clients(void)
{
    return nclients;
}

static bool valid_name(const char *s)
{
    size_t l = strlen(s);
    if (l < 1 || l >= sizeof name) {
        return false;
    }
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '-') {
            return false;
        }
    }
    return true;
}

bool web_set_name(const char *n)
{
    if (!valid_name(n)) {
        return false;
    }
    strcpy(name, n);
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_set_str(h, "name", name);
    nvs_commit(h);
    nvs_close(h);
    if (hd) {
        mdns_hostname_set(name);
    }
    return true;
}

const char *web_name(void)
{
    return name;
}

void web_init(QueueHandle_t q)
{
    cmd_q = q;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(id, sizeof id, "ledify_%02x%02x%02x", mac[3], mac[4], mac[5]);
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof name;
        char n[32];
        if (nvs_get_str(h, "name", n, &len) == ESP_OK && valid_name(n)) {
            strcpy(name, n);
        }
        nvs_close(h);
    }
}

void web_on_ip(void)
{
    if (hd) {
        return;
    }
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(name);
        mdns_instance_name_set("Ledify");
        mdns_txt_item_t txt[] = { { "id", id } };
        mdns_service_add(NULL, "_http", "_tcp", 80, txt, 1);
        mdns_service_add(NULL, "_ledify", "_tcp", 80, txt, 1);
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.close_fn = on_close;
    if (httpd_start(&hd, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "server failed to start");
        hd = NULL;
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_index },
        { .uri = "/app.js", .method = HTTP_GET, .handler = h_js },
        { .uri = "/app.css", .method = HTTP_GET, .handler = h_css },
        { .uri = "/config.js", .method = HTTP_GET, .handler = h_config },
        { .uri = "/icon.svg", .method = HTTP_GET, .handler = h_icon },
        { .uri = "/manifest.webmanifest", .method = HTTP_GET, .handler = h_manifest },
        { .uri = "/ws", .method = HTTP_GET, .handler = h_ws, .is_websocket = true },
    };
    for (int i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        httpd_register_uri_handler(hd, &uris[i]);
    }
    ESP_LOGI(TAG, "panel at http://%s.local/", name);
}
