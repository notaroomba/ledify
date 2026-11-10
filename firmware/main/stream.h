#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pixel streams over the LAN: DDP on UDP 4048 (LedFx, xLights, WLED tools) and E1.31/sACN on UDP 5568
// (LedFx, Hyperion, xLights; 170 pixels per universe, universes 1 and up, unicast or multicast). Frames go to
// the LED stream buffer and the main loop draws them while they keep coming.
void stream_init(void);
void stream_on_ip(void);                 // join the sACN multicast groups once the interface has an address
bool stream_take_frame(void);            // true once per complete frame received: draw the stream buffer now
int stream_json(char *out, size_t n);    // {"src":"LedFx","fps":40} or null when nothing is streaming

// Pictures from the web panel ("LM" frames, see matrix.h): over MQTT on ledify/<id>/frame/set, or as binary frames on
// the board's own WebSocket. Drawn onto the matrix set by stream_set_matrix() and shown like any other stream.
// from names the source in status ("panel", "broker"). Returns false if b is not a frame.
bool stream_image(const char *from, const uint8_t *b, int len);
void stream_set_matrix(int w, int h, int lay);   // w or h 0: no matrix, pictures run along the strip row by row
