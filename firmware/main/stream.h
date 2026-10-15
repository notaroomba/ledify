#pragma once
#include <stdbool.h>
#include <stddef.h>

// Pixel streams over the LAN: DDP on UDP 4048 (LedFx, xLights, WLED tools) and E1.31/sACN on UDP 5568
// (LedFx, Hyperion, xLights; 170 pixels per universe, universes 1 and up, unicast or multicast). Frames go to
// the LED stream buffer and the main loop draws them while they keep coming.
void stream_init(void);
void stream_on_ip(void);                 // join the sACN multicast groups once the interface has an address
bool stream_take_frame(void);            // true once per complete frame received: draw the stream buffer now
int stream_json(char *out, size_t n);    // {"src":"LedFx","fps":40} or null when nothing is streaming
