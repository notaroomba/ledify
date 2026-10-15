#include <string.h>
#include <stdio.h>
#include "stream.h"
#include "leds.h"
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "stream";
static int sock_ddp = -1, sock_e131 = -1;
static volatile bool frame_req;
static char src[32];
static int max_u;                        // highest sACN universe seen in this run: a frame is complete when it lands
static int64_t fps_at;
static int fps_count, fps;

#define PX_PER_UNIVERSE 170

static void got_frame(void)
{
    frame_req = true;
    int64_t now = esp_timer_get_time();
    fps_count++;
    if (now - fps_at > 1000000) {
        fps = fps_count;
        fps_count = 0;
        fps_at = now;
    }
}

// DDP: 10-byte header (flags, sequence, data type, destination, 32-bit byte offset, 16-bit length), RGB bytes after.
static void ddp(const uint8_t *b, int len)
{
    if (len < 10 || (b[0] & 0xC0) != 0x40) {
        return;
    }
    int hdr = (b[0] & 0x10) ? 14 : 10;                               // a timecode flag adds four bytes
    uint32_t off = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
    int dlen = (b[8] << 8) | b[9];
    if (dlen > len - hdr) {
        dlen = len - hdr;
    }
    if (b[3] > 1 || dlen < 3) {                                       // destination 0/1 is the default output
        return;
    }
    leds_stream_pixels(off / 3, b + hdr, dlen / 3);
    strcpy(src, "DDP");
    if (b[0] & 1) {                                                   // push: the frame is complete
        got_frame();
    }
}

// E1.31: ACN root layer, framing layer (source name, universe), DMP layer (start code + up to 512 channels).
static void e131(const uint8_t *b, int len)
{
    if (len < 126 || memcmp(b + 4, "ASC-E1.17\0\0\0", 12) || b[21] != 4 || b[43] != 2 || b[117] != 2 || b[118] != 0xa1 || b[125] != 0) {
        return;
    }
    if (b[112] & 0x40) {                                              // stream terminated
        return;
    }
    int u = (b[113] << 8) | b[114];
    int ch = ((b[123] << 8) | b[124]) - 1;
    if (ch > len - 126) {
        ch = len - 126;
    }
    if (u < 1 || ch < 3) {
        return;
    }
    strncpy(src, (const char *)b + 44, sizeof src - 1);
    src[sizeof src - 1] = 0;
    leds_stream_pixels((u - 1) * PX_PER_UNIVERSE, b + 126, ch / 3);
    if (u > max_u) {
        max_u = u;
    }
    if (u == max_u) {
        got_frame();
    }
}

static void stream_task(void *arg)
{
    static uint8_t buf[1500];
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock_ddp, &fds);
        FD_SET(sock_e131, &fds);
        struct timeval tv = { .tv_sec = 1 };
        int mx = sock_ddp > sock_e131 ? sock_ddp : sock_e131;
        if (select(mx + 1, &fds, NULL, NULL, &tv) <= 0) {
            if (!leds_stream_active()) {
                max_u = 0;
                fps = 0;
            }
            continue;
        }
        if (FD_ISSET(sock_ddp, &fds)) {
            int n = recv(sock_ddp, buf, sizeof buf, 0);
            if (n > 0) {
                ddp(buf, n);
            }
        }
        if (FD_ISSET(sock_e131, &fds)) {
            int n = recv(sock_e131, buf, sizeof buf, 0);
            if (n > 0) {
                e131(buf, n);
            }
        }
    }
}

static int udp(int port)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGW(TAG, "bind %d failed", port);
    }
    return s;
}

void stream_init(void)
{
    sock_ddp = udp(STREAM_PORT_DDP);
    sock_e131 = udp(STREAM_PORT_E131);
    xTaskCreate(stream_task, "stream", 4096, NULL, 4, NULL);
}

// sACN multicast: universe U lives on 239.255.0.U; enough groups for MAX_LEDS pixels.
void stream_on_ip(void)
{
    for (int u = 1; u <= (MAX_LEDS + PX_PER_UNIVERSE - 1) / PX_PER_UNIVERSE; u++) {
        struct ip_mreq m = { .imr_interface.s_addr = htonl(INADDR_ANY) };
        m.imr_multiaddr.s_addr = htonl(0xEFFF0000 | u);
        setsockopt(sock_e131, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);
    }
}

bool stream_take_frame(void)
{
    if (!frame_req) {
        return false;
    }
    frame_req = false;
    return true;
}

int stream_json(char *out, size_t n)
{
    if (!leds_stream_active()) {
        return snprintf(out, n, "null");
    }
    return snprintf(out, n, "{\"src\":\"%s\",\"fps\":%d}", src, fps);
}
