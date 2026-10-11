/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_GBLINK_H
#define GB_SIO_UDS_GBLINK_H

#include <mgba-util/common.h>

CXX_GUARD_START

struct mCore;
struct mDebugger;
struct GBVCLink;

/*
 * Layer L4 of the Virtual Console wrapper (doc/uds-wrapper-plan.md): a Game Boy serial-port driver that makes a Gen 1
 * Pokemon game believe it sits on a link cable with the 3DS Virtual Console release at the other end.
 *
 * How it works, in the terms of the captures (docs/wiki/vc_link.md):
 *
 *  - The wire carries one "unit" per serial transfer: the byte the console put on the line. The VC makes both consoles the
 *    internal-clock side, so every transfer a console starts is paired with the peer's transfer of the same number. This
 *    driver does the same: a transfer started with the internal clock queues a unit and completes, after the normal
 *    8-bit shift time, once the peer's matching unit has arrived (uds-session.c does the reliable delivery).
 *  - The VC patches a few places in the ROM (pret's vc_hook points). Where the patch changes what goes on the wire, this
 *    driver patches the same place with a breakpoint:
 *      Link_fake_connection_status .. Wireless_prompt   the receptionist's connection handshake never reaches the wire:
 *                                                        the status is forced to "internal clock" and serial transfers
 *                                                        in between are not paired with the peer
 *      Serial_SyncAndExchangeNybble (Wireless_WaitLinkTransfer)
 *                                                        the whole nybble sync becomes one burst of units (see GBVC_SYNC_*)
 *  - Everything else (link menu bytes, the byte blocks, the trade selection) runs as the ROM's own code on the serial
 *    device; whether that matches the VC stream closely enough is what the first live trades are for.
 *
 * The driver owns the UDS side (udp pair, join, Pia session) and polls it from the emulation thread, so a link is
 * created on a Game Boy core that is stopped (CoreController uses an Interrupter) and destroyed the same way.
 *
 * Set MGBA_VCLINK_TRACE to a directory to get a text log of every unit and hook (vclink_<time>.txt).
 */

#define GBVC_NAME_WORDS 10

enum GBVCAir {
	GBVC_AIR_BRIDGE, // Wireless Adapter > Local with "Virtual Console (local only)": the UDP pair to Azahar's test bridge
	GBVC_AIR_RADIO, // the board as a raw radio, the wrapper here, to a retail 3DS (no longer offered in the menu: GBVC_AIR_BOARD does it)
	GBVC_AIR_BOARD, // Wireless Adapter > ESP32: the wrapper runs on the board (firmware 1.4 on); the
	                // ROM is the cartridge and only its serial transfers go to the board. The board keeps the 3DS key: if it has none,
	                // the key file (keyPath) is stored on it.
};

// Which side of the link cable the cartridge plays, in wire mode (the front end has no say):
//  - Local (GBVC_AIR_BRIDGE): the cartridge is the slave and mGBA clocks it as a GB-Link (Pico) in Game Boy mode would, whose SPI is
//    master only: the Pico clock and the mail hold (uds-gblink.c). This follows the GB-Link model, as the GBA side does.
//    MGBA_VCLINK_CART=master brings back the master cartridge below (kept in case a GB-Link firmware ever offers both);
//    MGBA_VCLINK_PICO_CLOCK=0 the slave without the Pico clock; MGBA_VCLINK_PICO_MAIL_HOLD=<units> the mail hold (0: off).
//  - ESP32 (GBVC_AIR_BOARD): the cartridge is the master and the wrapper on the board its permanent slave (uds-wire.c), the only
//    arrangement firmware 1.5 has.
// The hook mode (MGBA_VCLINK_WIRE=0) is master as before.

struct GBVCLinkConfig {
	enum GBVCAir air;
	uint16_t listenPort; // bridge: 0 takes AZAHAR_UDS_BRIDGE or the default
	uint16_t sendPort;
	const char* portName; // radio: the board's serial port, NULL or empty to find it
	const char* keyPath; // radio: the 3DS UDS key file (Settings > BIOS)
	bool wire; // unused: wire mode (the ROM is the master of a link cable and a front end is its slave, uds-wire.c, no ROM hooks) is the default; MGBA_VCLINK_WIRE=0 selects the hook mode
	const char* tracePath; // a file for the trace (the Android app's shared log); NULL or empty: MGBA_VCLINK_TRACE as before
};

struct GBVCLink* GBVCLinkCreate(struct mCore* core, struct mDebugger* debugger, const uint16_t name[GBVC_NAME_WORDS],
                                const struct GBVCLinkConfig* config);
void GBVCLinkDestroy(struct GBVCLink* link);
// A line of the front end's own in the trace (the Android app's wall-clock and frame-rate notes). Nothing without a trace.
void GBVCLinkTraceNote(struct GBVCLink* link, const char* text);

CXX_GUARD_END

#endif
