/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_ESP32_SERIAL_H
#define GBA_SIO_ESP32_SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A plain serial (USB CDC) port. Windows only for now; elsewhere Open just fails.
struct Esp32Serial;

// `name` is e.g. "COM4". The board's native USB Serial/JTAG port resets the chip when opened (the firmware's own
// notes say so), so callers should give it a few seconds to boot before talking.
struct Esp32Serial* Esp32SerialOpen(const char* name, unsigned baud);
void Esp32SerialClose(struct Esp32Serial*);
// Non-blocking: returns the number of bytes read (possibly 0), or -1 if the port failed.
int Esp32SerialRead(struct Esp32Serial*, uint8_t* buffer, size_t capacity);
bool Esp32SerialWrite(struct Esp32Serial*, const void* data, size_t length);

// Looks for an Espressif native USB Serial/JTAG device (VID 303A, PID 1001 - what the ESP32-S3/C3/C6 boards
// enumerate as) among the ports Windows currently knows and writes its name (e.g. "COM4") to `out`. Returns false
// if none is present.
bool Esp32SerialFindEspressif(char* out, size_t capacity);

#endif
