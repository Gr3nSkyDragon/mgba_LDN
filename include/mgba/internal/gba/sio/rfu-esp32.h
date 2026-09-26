/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_RFU_ESP32_H
#define GBA_SIO_RFU_ESP32_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gba/sio/rfu.h>

/*
 * The "ESP32" wireless adapter backend: GB-Link's ESP32 LDN bridge board (an ESP32-S3/C3/C6/ESP32 running its
 * firmware) over USB serial. The board joins the Switch's FireRed/LeafGreen room, runs the Pia session and the
 * Switch-vs-cartridge link-layer fixes itself, and in its "adapter host" mode stands in for the Wireless Adapter's
 * cable side to whatever the host computer plays - here, this emulated adapter: it advertises the room as RFU1
 * broadcast frames, accepts an RFU1 connect request, and relays the game's child slots and the Switch's parent
 * slots. Only the board's documented serial protocol is used (no firmware code); see src/gba/sio/esp32/.
 *
 * The board's COM port is found automatically (Espressif native USB Serial/JTAG, else a CP210x/CH340/FTDI UART bridge) unless one is named.
 */

struct GBASIORFUBackend* GBASIORFUESP32Create(void);

// Names the serial port (e.g. "COM4") instead of auto-detecting; NULL or "" goes back to auto-detect. Call before
// the backend is attached.
void GBASIORFUESP32SetPort(struct GBASIORFUBackend*, const char* port);

CXX_GUARD_END

#endif
