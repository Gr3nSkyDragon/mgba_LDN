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
 *  - The master's data lags one exchange: the byte set for exchange n is shifted out in exchange n+1, so every phase starts
 *    with a stale byte (the previous byte left in the serial register) and ends with one stranded in it.
 *  - FE is "no data": the ROM re-sends the same byte one frame later. When no unit from the 3DS is available the reply is FE and
 *    no unit is sent; the cartridge retries, and units exist only for exchanges that completed (the emulator model's rule).
 *  - FF is an idle line: while the Pia session is not joined the cartridge sees no partner.
 *  - The cartridge's sync loops (fixed counts) and the 3DS's sync burst (another length) are not the same size, so during a sync
 *    the 3DS side runs by itself (udsCableSync) and the cartridge gets synthetic replies that make its own loops finish with the
 *    3DS's nybble.
 *
 * Phases, and what each does with the cartridge's bytes (the unit stream is the one the hook wrapper sends, see uds-gblink.c):
 *  DOWN   no session (or a sync that timed out): FF, nothing is sent.
 *  ROLE   offers 02 to the master's 01; no unit (the VC has no role bytes).
 *  IDLE   between phases. 00 and FE are stale bytes and are dropped; a 6x starts a SYNC, a Dx a MENU, anything else PASS. The
 *         first byte after a quiet gap that ended a PASS is the byte stranded by the lag: it is sent as a unit.
 *  SYNC   starts at the first 6x. The cartridge's loop 1 (6x until a 6x reply), loop 2 (ten more) and loop 3 (ten 00; the tenth is
 *         stranded into the next burst) are counted but produce no units: the 3DS side runs by itself. When both are finished
 *         the next byte decides: a 6x is the next sync, a Dx the menu, anything else raw exchange, in which case the stale
 *         byte that preceded the sync is sent first (the hook wrapper's raw exchange after a sync starts with it). That byte is an
 *         exchange like any other and goes out as a unit: plain transfer starts (zero bytes) are not retried after FE, and a
 *         dropped unit leaves the 3DS waiting, where one unit too many does no harm.
 *  MENU   one unit per Dx byte (three per call, as the hook wrapper), replies are the 3DS's selection (last Dx unit seen,
 *         D0 until one). A 00 is held: the next Dx shows it was a cycle's stale byte; if the menu ends it is the final transfer and
 *         is sent. If the 3DS pressed A and we did not, its further units are answered with its choice (uds-cable's echo).
 *  PASS   one unit per completed exchange (a unit from the 3DS is the reply; with none, FE, and the byte is sent once).
 * A quiet gap (UDS_WIRE_GAP_MS) ends MENU and PASS; every sync in a recorded trade follows one.
 *
 * Blocks (the fd preambles) are PASS here; bulk buffering is the next step.
 */

#define UDS_WIRE_RX 1024 // more than the largest block and its preamble
#define UDS_WIRE_PENDING_MAX 12 // most units sent ahead of the 3DS's while the cartridge waits (the send window is 25)

enum UDSWirePhase {
	UDS_WIRE_DOWN,
	UDS_WIRE_ROLE,
	UDS_WIRE_IDLE,
	UDS_WIRE_SYNC,
	UDS_WIRE_MENU,
	UDS_WIRE_PASS,
};

enum UDSWireSyncPart {
	UDS_WIRE_SYNC_LOOPS12, // 6x bytes: the cartridge's first two loops
	UDS_WIRE_SYNC_LOOP3, // 00 bytes, counted to ten
	UDS_WIRE_SYNC_DONE, // the cartridge's loops are over
};

struct UDSWire {
	struct UDSCable cable; // the unit port, the 3DS side of the sync and the menu echo

	enum UDSWirePhase phase;
	bool begun; // this session's first unit has been sent (once per link-up, so a sync timeout does not start another)
	bool idleZeroSent; // the slave's 00 after the role handshake
	bool strandedPending; // the next byte is the one stranded by the lag at the end of a PASS
	bool outstanding; // a unit has been sent for the exchange the cartridge is retrying after FE ...
	uint8_t outstandingByte; // ... and this is its byte
	bool preloadHost; // the byte loaded for the next exchange is the 3DS's next unit
	uint8_t lastMaster;
	uint32_t lastExchangeMs;

	// sync
	uint8_t cartNybble; // the nybble the cartridge is syncing
	uint8_t syncHigh; // and its high nybble: 60 in Gen 1; Gen 2 uses 60, 70 (Trade Center) or 80 (Colosseum) by link mode
	bool gen2; // Gen 2 syncs are recognised in all three ranges
	int hostNybble; // the 3DS's nybble, -1 until its 6x unit has been read
	bool syncDone; // the 3DS side of the sync has finished
	enum UDSWireSyncPart syncPart;
	unsigned zeros; // 00 bytes seen in loop 3
	uint8_t stale; // the byte that preceded the sync's first 6x

	// menu
	uint8_t menuHost; // the 3DS's selection, as the cartridge is told it
	bool menuHostKnown; // a selection of the 3DS's has been read (Gen 2: until then the cartridge is told FE, see udsWirePreload)
	bool pendingZero; // a 00 is held: stale byte of the next cycle, or the final transfer
	bool hostPress; // the 3DS has pressed A or B (and the cartridge has been told)
	bool cartPress;
	bool menuEra; // from the menu's start until its leftovers have been read: the 3DS's Dx units are tracked
	bool echoWanted; // the menu ended and the echo is armed once the debt is paid, if the 3DS pressed and the cartridge did not

	// Units sent whose partner (the 3DS's unit with the same index) has not been read yet. Index pairing is what the 3DS
	// relies on and what keeps a sync from being answered by an old unit: every unit sent without a read adds to the debt,
	// and the debt is paid by reading (and dropping) the 3DS's units as they arrive, before anything else reads.
	unsigned readDebt;

	// Blocks (PASS). Serial_ExchangeBytes ignores replies until an fd, then stores the next `size` replies whatever they are: with
	// only the serial interrupt enabled an FE reply is not retried but returned as data. So once a block's data has started the
	// replies must never run dry, and the first fd of a block is held until the whole block is queued.
	bool blockStoring;
	unsigned blockIndex; // which block of the cycle (random numbers, player data, patch lists) the cartridge is receiving
	unsigned blockRemaining;
	unsigned blockUnderruns; // replies that had to be FE inside a block's data (the block is damaged)
	bool mailMode; // Gen 2 after the patch lists: the Trade Center's mail block, a plain ExchangeBytes (see _pass)
	bool rnCovered; // the 3DS's random-number list has been covered with fd (see _pass)
	bool rnSuppress; // the cartridge's own list's units are being dropped
	bool rnSeenFd;
	unsigned rnNumbers; // its numbers seen so far
	unsigned blockSkip; // units to drop from the buffer so that an FE stored as data does not shift the block
	bool preloadFill; // the reply loaded is a stand-in for the block's last byte, which the 3DS has not produced yet

	// PASS has its own receive buffer. The 3DS only produces its next unit once it has ours (its exchanges block), so every
	// exchange of the cartridge sends a unit (up to UDS_WIRE_PENDING_MAX ahead of what has been read) and the 3DS's units are read
	// into the buffer as they arrive; the cartridge is answered from the buffer, at wire speed, whenever it has what is needed.
	uint8_t rx[UDS_WIRE_RX];
	unsigned rxHead;
	unsigned rxCount;
	int pending; // units sent while ignoring/waiting that have not been matched by a read yet
};

// Replies the cartridge stores after the fd that ends its ignoring, for the three blocks of an exchange (random numbers with their
// preamble, the 424-byte player block, the 200-byte patch lists) in the order they come.
#define UDS_WIRE_BLOCKS 3
#define UDS_WIRE_RN_POSITIONS 18 // the list's exchanges on the 3DS: the fd that ends its ignoring, seven of preamble, ten numbers
#define UDS_WIRE_RN_NUMBERS 10

#define UDS_WIRE_IDLE_LINE 0xFF
#define UDS_WIRE_NO_DATA 0xFE
#define UDS_WIRE_ROLE_SLAVE 0x02 // what the slave offers; the master's own offer is 01
#define UDS_WIRE_GAP_MS 400 // no exchange for this long ends a MENU or a PASS
#define UDS_WIRE_LOOP3_ZEROS 10
#define UDS_WIRE_MENU_AHEAD 12 // most units owed (sent, partner unread) before the menu stops sending, so the send window cannot fill

void udsWireInit(struct UDSWire* wire, const struct UDSUnitPort* port);
// 2 makes syncs in the 70 and 80 ranges count as syncs too (Gen 2 picks the range by link mode); 1 (the default) only 60.
void udsWireSetGeneration(struct UDSWire* wire, int generation);

// Call every millisecond or so: brings the session's state in (link up, link lost), runs the 3DS side of a sync and ends a
// MENU or PASS that has gone quiet.
void udsWirePoll(struct UDSWire* wire, uint32_t nowMs);

// The byte the slave shifts out in the next exchange. Does not change any state apart from remembering whether it came from
// the 3DS, so it may be called any number of times.
uint8_t udsWirePreload(struct UDSWire* wire);

// The master clocked an exchange and `masterByte` arrived. Call udsWirePreload afterwards for the next one.
void udsWireExchanged(struct UDSWire* wire, uint32_t nowMs, uint8_t masterByte);

CXX_GUARD_END

#endif
