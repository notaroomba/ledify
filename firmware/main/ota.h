#pragma once
#include <stdbool.h>

bool ota_start(const char *url);   // download an app image over Wi-Fi into the other slot, then restart
void ota_mark_ok(void);            // the running image works: cancel the bootloader's rollback
