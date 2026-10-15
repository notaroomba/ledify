#pragma once
#include <stdbool.h>
#include <stddef.h>

// Clock and schedules. The time comes from SNTP once Wi-Fi is up; the timezone is a POSIX TZ string in NVS
// ("EST5EDT,M3.2.0,M11.1.0"); entries are "HH:MM command" and run every day at that minute.
#define SCHED_MAX 8
void sched_init(void);
void sched_on_ip(void);                      // start SNTP
bool sched_set_tz(const char *posix_tz);     // saved; "" = UTC
const char *sched_tz(void);
bool sched_time(char *out, size_t n);        // "HH:MM" local time; false until the clock is set
bool sched_add(const char *hhmm, const char *cmd);
bool sched_del(int i);                       // 1-based, as listed
void sched_clear(void);
int sched_json(char *out, size_t n);         // [{"at":"06:30","cmd":"rise 20"},...], returns the length like snprintf
const char *sched_due(void);                 // a command whose minute has just come, else NULL; call often
