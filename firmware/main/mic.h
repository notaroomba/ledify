#pragma once
#include <stdbool.h>

bool mic_init(void);
float mic_level(void);     // RMS of the last 256 samples, 0..1 of full scale
