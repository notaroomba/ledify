#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "sched.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs.h"

static const char *TAG = "sched";
static char tz[64];
static struct { int16_t minute; char cmd[48]; } tab[SCHED_MAX];
static int ntab, last_minute = -1, scan;

static void load_str(const char *key, char *buf, size_t n)
{
    nvs_handle_t h;
    buf[0] = 0;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        size_t len = n;
        if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
            buf[0] = 0;
        }
        nvs_close(h);
    }
}

static bool save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t e = nvs_set_str(h, key, val);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e == ESP_OK;
}

static void apply_tz(void)
{
    setenv("TZ", tz[0] ? tz : "UTC0", 1);
    tzset();
}

// The table is one NVS string: "HHMM command\n" per entry.
static bool store(void)
{
    char buf[SCHED_MAX * 56];
    int w = 0;
    for (int i = 0; i < ntab; i++) {
        w += snprintf(buf + w, sizeof buf - w, "%02d%02d %s\n", tab[i].minute / 60, tab[i].minute % 60, tab[i].cmd);
    }
    return save_str("sched", buf);
}

void sched_init(void)
{
    load_str("tz", tz, sizeof tz);
    apply_tz();
    char buf[SCHED_MAX * 56];
    load_str("sched", buf, sizeof buf);
    for (char *line = strtok(buf, "\n"); line && ntab < SCHED_MAX; line = strtok(NULL, "\n")) {
        if (strlen(line) > 5 && line[4] == ' ') {
            tab[ntab].minute = (line[0] - '0') * 600 + (line[1] - '0') * 60 + (line[2] - '0') * 10 + (line[3] - '0');
            strncpy(tab[ntab].cmd, line + 5, sizeof tab[ntab].cmd - 1);
            ntab++;
        }
    }
    ESP_LOGI(TAG, "tz %s, %d scheduled", tz[0] ? tz : "UTC", ntab);
}

void sched_on_ip(void)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
}

bool sched_set_tz(const char *posix_tz)
{
    if (strlen(posix_tz) >= sizeof tz) {
        return false;
    }
    strcpy(tz, !strcmp(posix_tz, "utc") ? "" : posix_tz);
    apply_tz();
    return save_str("tz", tz);
}

const char *sched_tz(void)
{
    return tz[0] ? tz : "UTC0";
}

static bool local_now(struct tm *out)
{
    time_t now = time(NULL);
    localtime_r(&now, out);
    return out->tm_year >= 120;              // before 2020 the clock has not been set
}

bool sched_time(char *out, size_t n)
{
    struct tm t;
    if (!local_now(&t)) {
        out[0] = 0;
        return false;
    }
    snprintf(out, n, "%02d:%02d", t.tm_hour, t.tm_min);
    return true;
}

bool sched_add(const char *hhmm, const char *cmd)
{
    int h, m;
    if (ntab >= SCHED_MAX || sscanf(hhmm, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59 || !cmd[0] || strlen(cmd) >= sizeof tab[0].cmd) {
        return false;
    }
    tab[ntab].minute = h * 60 + m;
    strcpy(tab[ntab].cmd, cmd);
    ntab++;
    return store();
}

bool sched_del(int i)
{
    if (i < 1 || i > ntab) {
        return false;
    }
    memmove(&tab[i - 1], &tab[i], (ntab - i) * sizeof tab[0]);
    ntab--;
    return store();
}

void sched_clear(void)
{
    ntab = 0;
    store();
}

int sched_json(char *out, size_t n)
{
    int w = snprintf(out, n, "[");
    for (int i = 0; i < ntab; i++) {
        w += snprintf(out + (w < (int)n ? w : n), w < (int)n ? n - w : 0, "%s{\"at\":\"%02d:%02d\",\"cmd\":\"%s\"}", i ? "," : "", tab[i].minute / 60, tab[i].minute % 60, tab[i].cmd);
    }
    w += snprintf(out + (w < (int)n ? w : n), w < (int)n ? n - w : 0, "]");
    return w;
}

// Called from the main loop. When the minute changes, hand out every entry for that minute, one per call.
const char *sched_due(void)
{
    struct tm t;
    if (!ntab || !local_now(&t)) {
        return NULL;
    }
    int cur = t.tm_hour * 60 + t.tm_min;
    if (cur != last_minute) {
        last_minute = cur;
        scan = 0;
    }
    while (scan < ntab) {
        if (tab[scan].minute == cur) {
            return tab[scan++].cmd;
        }
        scan++;
    }
    return NULL;
}
