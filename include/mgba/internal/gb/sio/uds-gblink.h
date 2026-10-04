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
	GBVC_AIR_BRIDGE, // Wireless Adapter > Local: the UDP pair to Azahar's test bridge
	GBVC_AIR_RADIO, // Wireless Adapter > ESP32: the real radio through the ESP32 board, to a retail 3DS
};

struct GBVCLinkConfig {
	enum GBVCAir air;
	uint16_t listenPort; // bridge: 0 takes AZAHAR_UDS_BRIDGE or the default
	uint16_t sendPort;
	const char* portName; // radio: the board's serial port, NULL or empty to find it
	const char* keyPath; // radio: the 3DS UDS key file (Settings > BIOS)
};

struct GBVCLink* GBVCLinkCreate(struct mCore* core, struct mDebugger* debugger, const uint16_t name[GBVC_NAME_WORDS],
                                const struct GBVCLinkConfig* config);
void GBVCLinkDestroy(struct GBVCLink* link);

CXX_GUARD_END

#endif
