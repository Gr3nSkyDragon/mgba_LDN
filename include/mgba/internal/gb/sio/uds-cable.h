/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_CABLE_H
#define GB_SIO_UDS_CABLE_H

#include <mgba-util/common.h>

CXX_GUARD_START

/*
 * The cable side of the Virtual Console wrapper, with no emulator in it (doc/uds-wrapper-plan.md, section 9).
 *
 * It turns what a Game Boy does on its serial port into the 3DS Virtual Console's unit stream and back: the pairing of a
 * transfer with the peer's unit, the nybble sync burst and the link menu exchange. It touches neither a core nor a socket nor
 * a clock: units go through a UDSUnitPort, time is passed in, and what the Game Boy intends (the nybble it wants to sync,
 * the menu selection it made) is passed in as arguments. The mGBA driver (uds-gblink.c) feeds it from the ROM's RAM today; a
 * wire-level front end will feed it from cable bytes, and the same code is meant to run on the ESP32.
 *
 * Time is milliseconds from any fixed origin.
 */

#define UDS_CABLE_FIRST_UNIT 0x00 // the joiner's unit at index -2001; the host's is EF
#define UDS_CABLE_SYNC_SIXTIES 5 // after the host's answer: this many more 60|nybble, then UDS_CABLE_SYNC_ZEROS of 00
#define UDS_CABLE_SYNC_ZEROS 5
#define UDS_CABLE_SYNC_TIMEOUT_MS 20000
#define UDS_CABLE_SYNC_TAIL_MAX 40 // most units followed after the first 12
#define UDS_CABLE_SYNC_TAIL_QUIET_MS 250 // the host's burst is over when it has sent nothing for this long
#define UDS_CABLE_ECHO_QUIET_MS 600 // the host has left the menu when it has sent nothing for this long
#define UDS_CABLE_ECHO_MAX_MS 4000
#define UDS_CABLE_ECHO_MAX_UNITS 64

// Where the units go. All functions are called from one thread.
struct UDSUnitPort {
	void* context;
	bool (*ready)(void* context); // the Pia session is joined and the stream may be used
	bool (*queue)(void* context, uint8_t byte); // false when the send window is full
	void (*flush)(void* context);
	bool (*pop)(void* context, uint8_t* byte); // the next unit from the host, in order
	bool (*peek)(void* context, uint8_t* byte); // the same, left in place
	size_t (*waiting)(void* context);
	void (*trace)(void* context, const char* line); // may be NULL
};

enum UDSCableSyncPhase {
	UDS_SYNC_IDLE,
	UDS_SYNC_WAIT, // exchanging 60|nybble units until the host's 6x arrives
	UDS_SYNC_AFTER, // the ROM's two shorter loops after that
	UDS_SYNC_TAIL, // following the host's burst to its end
};

enum UDSCableStatus {
	UDS_CABLE_PENDING, // not finished: call again later (the Game Boy goes on running)
	UDS_CABLE_DONE,
	UDS_CABLE_TIMED_OUT, // sync only: nobody answered; the result is 0xFF
};

struct UDSCable {
	struct UDSUnitPort port;

	bool discardFirst; // the host's first unit (EF) is not a transfer
	bool busy; // a sync or menu exchange is under way: transfers are not paired

	bool txPending; // a transfer is waiting for the peer's unit
	uint8_t txByte;

	enum UDSCableSyncPhase syncPhase;
	unsigned syncSentN;
	unsigned syncRecvN;
	unsigned syncTail;
	uint32_t syncTailMs;
	uint32_t syncStartMs;
	uint8_t syncHigh; // the high nybble of the syncs' bytes: 60 in Gen 1, 60/70/80 in Gen 2 by link mode (set per sync)
	int syncNybble;

	bool menuActive;
	bool menuSent;
	unsigned menuGot;
	uint8_t menuSelection;
	uint8_t menuBytes[3];

	// The host chose first (we did not): its menu loop goes on until a D0-class byte reaches one of its two kept replies, so its
	// units are answered with the adopted selection until it goes quiet. See udsCablePoll.
	bool echoActive;
	uint8_t echoByte;
	uint32_t echoStartMs;
	uint32_t echoLastMs;
	unsigned echoCount;
};

void udsCableInit(struct UDSCable* cable, const struct UDSUnitPort* port);

// The link has just come up: sends the first unit and arranges for the host's to be discarded.
void udsCableBegin(struct UDSCable* cable);
// The high nybble of the syncs' bytes for the syncs that follow (60 by default).
void udsCableSetSyncHigh(struct UDSCable* cable, uint8_t high);

// The host went away. Returns true if a transfer was waiting; the caller should finish it with an idle line (0xFF).
bool udsCableLost(struct UDSCable* cable);

// Call every millisecond or so while the link is up. Discards the host's first unit and completes a waiting transfer:
// returns true with the byte the Game Boy receives. After a link menu call that adopted the host's choice it also answers the
// host's further units with that choice, for as long as they keep coming.
bool udsCablePoll(struct UDSCable* cable, uint32_t nowMs, uint8_t* received);

// The Game Boy started a transfer as master with `byte` in its serial register. A transfer already waiting is kept.
void udsCableTransfer(struct UDSCable* cable, uint8_t byte);

// Serial_SyncAndExchangeNybble: the Game Boy wants to exchange `nybble`. Call repeatedly (once a frame is plenty) until it
// stops returning PENDING; `result` is then the nybble the host sent (0xFF on timeout).
enum UDSCableStatus udsCableSync(struct UDSCable* cable, uint8_t nybble, uint32_t nowMs, int* result);

// Serial_ExchangeLinkMenuSelection: the Game Boy made `selection`. Call repeatedly until it returns DONE; `first` and
// `second` are then the two bytes for the ROM's receive buffer.
enum UDSCableStatus udsCableMenu(struct UDSCable* cable, uint8_t selection, uint32_t nowMs, uint8_t* first, uint8_t* second);

CXX_GUARD_END

#endif
