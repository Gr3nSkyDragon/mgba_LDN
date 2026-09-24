#ifndef PROBE_H
#define PROBE_H
#include <stdbool.h>

// Runs the same session the mGBA ESP32 backend runs against the GB-Link ESP32 bridge, through whatever serial
// implementation Esp32SerialSetOps installed, and reports what it saw via `log` (one line per call).
// Returns 0 when the board answered, joined a Switch room and advertised at least one beacon; nonzero otherwise.
int probe_run(unsigned seconds, void (*log)(const char* line));
void probe_stop(void);

#endif
