/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_WIRE_H
#define GB_SIO_UDS_WIRE_H

#include <mgba-util/common.h>

#include <mgba/internal/gb/sio/uds-cable.h>

CXX_GUARD_START

/*
 * The wire-level front end of the Virtual Console wrapper (doc/uds-wrapper-plan.md, section 9, step 3): a permanent slave on a
 * Game Boy link cable. The cartridge (real, or an emulated ROM treated as one) is always the master and clocks every exchange;
 * this module decides what the slave shifts out. It knows nothing of an emulator, a pin or a clock: the caller says "the master
 * clocked byte b" and asks for the byte to load for the next exchange.
 *
 * What a real master forces on the design (measured in docs/cable, and from home/serial.asm):
 *  - A slave's reply is loaded before the master clocks, so the reply to exchange i is decided before byte i is known; it can
 *    depend on bytes up to i-1 and on what the 3DS has delivered.
 *  - FE is "no data": the ROM re-sends the same byte one frame later. When no unit from the 3DS is available the reply is FE and
 *    no unit is sent; the cartridge retries, and units exist only for exchanges that completed (the emulator model's rule).
 *  - FF is an idle line: while the Pia session is not joined the cartridge sees no partner.
 *  - The cartridge's sync loops (fixed counts) and the 3DS's sync burst (another length) are not the same size, so during a sync
 *    the 3DS side runs by itself (udsCableSync) and the cartridge gets synthetic replies that make its own loops finish with the
 *    3DS's nybble.
 *
 * Phases: DOWN (no session), ROLE (offering 02 to the master's 01), IDLE (connected, nothing sent yet), SYNC, PASS (everything
 * else: one unit per completed exchange). Blocks (the fd preambles) and the link menu are still pass-through here; their own
 * phases are the next step.
 */

enum UDSWirePhase {
	UDS_WIRE_DOWN,
	UDS_WIRE_ROLE,
	UDS_WIRE_IDLE,
	UDS_WIRE_SYNC,
	UDS_WIRE_PASS,
};

struct UDSWire {
	struct UDSCable cable; // the unit port and the 3DS side of the sync

	enum UDSWirePhase phase;
	bool begun; // this session's first unit has been sent (once per link-up, so a sync timeout does not start another)
	bool idleZeroSent; // the slave's 00 after the role handshake
	bool outstanding; // a unit has been sent for the exchange the cartridge is retrying after FE ...
	uint8_t outstandingByte; // ... and this is its byte
	bool preloadHost; // the byte loaded for the next exchange is the 3DS's next unit
	uint8_t lastMaster;

	uint8_t cartNybble; // the nybble the cartridge is syncing
	int hostNybble; // the 3DS's nybble, -1 until its 6x unit has been read
	bool syncDone; // the 3DS side of the sync has finished
};

#define UDS_WIRE_IDLE_LINE 0xFF
#define UDS_WIRE_NO_DATA 0xFE
#define UDS_WIRE_ROLE_SLAVE 0x02 // what the slave offers; the master's own offer is 01

void udsWireInit(struct UDSWire* wire, const struct UDSUnitPort* port);

// Call every millisecond or so: brings the session's state in (link up, link lost) and runs the 3DS side of a sync.
void udsWirePoll(struct UDSWire* wire, uint32_t nowMs);

// The byte the slave shifts out in the next exchange. Does not change any state apart from remembering whether it came from
// the 3DS, so it may be called any number of times.
uint8_t udsWirePreload(struct UDSWire* wire);

// The master clocked an exchange and `masterByte` arrived. Call udsWirePreload afterwards for the next one.
void udsWireExchanged(struct UDSWire* wire, uint32_t nowMs, uint8_t masterByte);

CXX_GUARD_END

#endif
