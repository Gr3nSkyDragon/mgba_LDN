/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-wire.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const unsigned kBlockSize[UDS_WIRE_BLOCKS] = {17, 424, 200};

static void _trace(struct UDSWire* wire, const char* format, ...) {
	if (!wire->cable.port.trace) {
		return;
	}
	char line[160];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	wire->cable.port.trace(wire->cable.port.context, line);
}

static bool _isSync(const struct UDSWire* wire, uint8_t byte) {
	uint8_t high = byte & 0xF0;
	return high == 0x60 || (wire->gen2 && (high == 0x70 || high == 0x80));
}

static bool _isMenu(uint8_t byte) {
	return (byte & 0xF0) == 0xD0;
}

// The 3DS's next unit, if one may be read now. While units are owed (sent, partner unread) the oldest of the 3DS's units are the
// partners of those and nothing else may read them.
static bool _hostUnit(struct UDSWire* wire, uint8_t* byte) {
	struct UDSUnitPort* port = &wire->cable.port;
	return !wire->cable.discardFirst && wire->readDebt == 0 && port->peek(port->context, byte);
}

static void _send(struct UDSWire* wire, uint8_t byte) {
	struct UDSUnitPort* port = &wire->cable.port;
	port->queue(port->context, byte);
	port->flush(port->context);
}

// A unit sent without reading its partner: the partner is read (and dropped) later, as the 3DS's units arrive.
static void _sendOwed(struct UDSWire* wire, uint8_t byte) {
	_send(wire, byte);
	++wire->readDebt;
}

static void _payDebt(struct UDSWire* wire) {
	struct UDSUnitPort* port = &wire->cable.port;
	uint8_t host;
	while (wire->readDebt && !wire->cable.discardFirst && port->peek(port->context, &host)) {
		port->pop(port->context, &host);
		--wire->readDebt;
		if (wire->menuEra && _isMenu(host)) {
			wire->menuHost = host;
			wire->menuHostKnown = true;
			if (host & 0x0C) {
				wire->hostPress = true;
			}
		}
	}
}

// PASS: read the 3DS's units into the receive buffer as they arrive (each read matches one unit sent while waiting).
static void _drain(struct UDSWire* wire) {
	struct UDSUnitPort* port = &wire->cable.port;
	uint8_t host;
	_payDebt(wire);
	while (!wire->readDebt && !wire->cable.discardFirst && wire->rxCount < UDS_WIRE_RX && port->peek(port->context, &host)) {
		port->pop(port->context, &host);
		wire->rx[(wire->rxHead + wire->rxCount) % UDS_WIRE_RX] = host;
		++wire->rxCount;
		if (wire->pending > 0) {
			--wire->pending;
		}
	}
}

static uint8_t _rxTake(struct UDSWire* wire) {
	uint8_t byte = wire->rx[wire->rxHead];
	wire->rxHead = (wire->rxHead + 1) % UDS_WIRE_RX;
	--wire->rxCount;
	return byte;
}

// The port the cable and the wire use counts every unit of the session in both directions (see _headPosition).
static bool _countReady(void* context) {
	struct UDSWire* wire = context;
	return wire->inner.ready(wire->inner.context);
}

static bool _countQueue(void* context, uint8_t byte) {
	struct UDSWire* wire = context;
	if (!wire->inner.queue(wire->inner.context, byte)) {
		return false;
	}
	++wire->sentUnits;
	return true;
}

static void _countFlush(void* context) {
	struct UDSWire* wire = context;
	wire->inner.flush(wire->inner.context);
}

static bool _countPop(void* context, uint8_t* byte) {
	struct UDSWire* wire = context;
	if (!wire->inner.pop(wire->inner.context, byte)) {
		return false;
	}
	++wire->recvUnits;
	return true;
}

static bool _countPeek(void* context, uint8_t* byte) {
	struct UDSWire* wire = context;
	return wire->inner.peek(wire->inner.context, byte);
}

static size_t _countWaiting(void* context) {
	struct UDSWire* wire = context;
	return wire->inner.waiting(wire->inner.context);
}

static void _countTrace(void* context, const char* line) {
	struct UDSWire* wire = context;
	wire->inner.trace(wire->inner.context, line);
}

void udsWireInit(struct UDSWire* wire, const struct UDSUnitPort* port) {
	memset(wire, 0, sizeof(*wire));
	wire->inner = *port;
	udsCableInit(&wire->cable, &(struct UDSUnitPort) {
		.context = wire,
		.ready = _countReady,
		.queue = _countQueue,
		.flush = _countFlush,
		.pop = _countPop,
		.peek = _countPeek,
		.waiting = _countWaiting,
		.trace = port->trace ? _countTrace : NULL,
	});
	wire->phase = UDS_WIRE_DOWN;
	wire->hostNybble = -1;
	wire->menuHost = 0xD0;
	wire->syncHigh = 0x60;
}

void udsWireSetGeneration(struct UDSWire* wire, int generation) {
	wire->gen2 = generation == 2;
}

static void _menuEnd(struct UDSWire* wire) {
	if (wire->pendingZero) {
		// Not the next cycle's stale byte after all: the transfer the cartridge makes once the choice is settled.
		_sendOwed(wire, 0x00);
		wire->pendingZero = false;
	}
	wire->echoWanted = true; // decided when the debt is paid: only then is it known what the 3DS pressed
	wire->phase = UDS_WIRE_IDLE;
	wire->strandedPending = false;
	wire->idleZeroSent = true;
	wire->outstanding = false;
	wire->preloadHost = false;
}

static void _armEcho(struct UDSWire* wire, uint32_t nowMs) {
	wire->echoWanted = false;
	wire->menuEra = false;
	if (wire->hostPress && !wire->cartPress) {
		// The 3DS chose first and the cartridge took its choice. The 3DS's menu loop goes on until a D0-class byte reaches
		// its kept replies, so answer its further units with the choice (uds-cable's echo).
		wire->cable.echoActive = true;
		wire->cable.echoByte = wire->menuHost;
		wire->cable.echoStartMs = wire->cable.echoLastMs = nowMs;
		wire->cable.echoCount = 0;
	}
}

void udsWirePoll(struct UDSWire* wire, uint32_t nowMs) {
	struct UDSUnitPort* port = &wire->cable.port;
	bool ready = port->ready(port->context);

	if (!ready) {
		// The 3DS went away: the cartridge sees an idle line until the session is back.
		if (wire->phase != UDS_WIRE_DOWN || wire->begun) {
			udsCableLost(&wire->cable);
		}
		wire->phase = UDS_WIRE_DOWN;
		wire->begun = false;
		wire->outstanding = false;
		wire->readDebt = 0;
		wire->echoWanted = false;
		return;
	}
	if (wire->phase == UDS_WIRE_DOWN) {
		if (!wire->begun) {
			udsCableBegin(&wire->cable);
			wire->sentUnits = wire->recvUnits = 0; // a new session: both streams count from their first unit
			wire->begun = true;
			wire->readDebt = 0;
			wire->phase = UDS_WIRE_ROLE;
		}
		return; // DOWN with the session up and begun: a sync that timed out; the line stays idle until the session restarts
	}

	uint8_t unused;
	udsCablePoll(&wire->cable, nowMs, &unused); // drops the 3DS's first unit, runs the echo and flushes what is queued
	_payDebt(wire);
	if (wire->echoWanted && wire->readDebt == 0) {
		_armEcho(wire, nowMs);
	}
	if (wire->phase == UDS_WIRE_PASS) {
		_drain(wire);
	}

	if (wire->phase == UDS_WIRE_SYNC && !wire->syncDone) {
		if (wire->readDebt) {
			return; // the 3DS's older units are not this sync's
		}
		int result = 0;
		enum UDSCableStatus status = udsCableSync(&wire->cable, wire->cartNybble, nowMs, &result);
		if (wire->cable.syncNybble >= 0) {
			wire->hostNybble = wire->cable.syncNybble;
		}
		if (status == UDS_CABLE_DONE) {
			wire->syncDone = true;
		} else if (status == UDS_CABLE_TIMED_OUT) {
			// Nobody answered: let the cartridge see an idle line, as its own timeout would end the sync.
			wire->phase = UDS_WIRE_DOWN;
			wire->outstanding = false;
		}
	} else if (wire->phase == UDS_WIRE_MENU && nowMs - wire->lastExchangeMs > UDS_WIRE_GAP_MS) {
		_menuEnd(wire);
	} else if (wire->phase == UDS_WIRE_PASS && nowMs - wire->lastExchangeMs > UDS_WIRE_GAP_MS) {
		wire->phase = UDS_WIRE_IDLE;
		wire->mailMode = false;
		wire->strandedPending = true;
		wire->outstanding = false;
		wire->preloadHost = false;
		wire->blockStoring = false;
		wire->rxHead = wire->rxCount = 0; // what was read for the cartridge and not delivered belongs to this phase
	}
}

uint8_t udsWirePreload(struct UDSWire* wire) {
	uint8_t host;
	wire->preloadHost = false;
	wire->preloadFill = false;
	switch (wire->phase) {
	case UDS_WIRE_DOWN:
		return UDS_WIRE_IDLE_LINE;
	case UDS_WIRE_ROLE:
		return UDS_WIRE_ROLE_SLAVE;
	case UDS_WIRE_IDLE:
		return wire->idleZeroSent ? UDS_WIRE_NO_DATA : 0x00;
	case UDS_WIRE_SYNC:
		// Nothing until the 3DS's nybble is known (the cartridge's first loop runs until it receives a 6x); then what a slave
		// that had synced with that nybble would answer: 60|nybble through the first two loops, 00 through the third.
		if (wire->hostNybble < 0) {
			return UDS_WIRE_NO_DATA;
		}
		switch (wire->syncPart) {
		case UDS_WIRE_SYNC_LOOPS12:
			return wire->syncHigh | wire->hostNybble;
		case UDS_WIRE_SYNC_LOOP3:
			return wire->zeros < UDS_WIRE_LOOP3_ZEROS ? 0x00 : UDS_WIRE_NO_DATA;
		case UDS_WIRE_SYNC_DONE:
			// Both sides are finished: what comes next is an ordinary exchange, so answer it as one.
			if (wire->syncDone && _hostUnit(wire, &host)) {
				wire->preloadHost = true;
				return host;
			}
			return UDS_WIRE_NO_DATA;
		}
		return UDS_WIRE_NO_DATA;
	case UDS_WIRE_MENU:
		// Gen 1's menu loops until someone presses A, so an assumed D0 does no harm there. Gen 2's Link_EnsureSync is a rendezvous that
		// returns as soon as a reply is in the D range: an assumed D0 ended it before the 3DS's game had reached its own menu (a Gen 1 game
		// in a Time Capsule trade), the Gen 2 game walked on and the 3DS waited at its menu for ever. Until a selection of the 3DS's has been
		// read the cartridge is told it has no data and goes on asking, as on a cable with no partner.
		if (wire->gen2 && !wire->menuHostKnown) {
			return UDS_WIRE_NO_DATA;
		}
		return wire->menuHost;
	case UDS_WIRE_PASS:
		_drain(wire); // a block cannot wait for the next poll
		while (wire->blockSkip && wire->rxCount) {
			_rxTake(wire); // the partner of an FE that was stored as data: dropped so that the block does not shift
			--wire->blockSkip;
		}
		while (wire->mailMode && !wire->mailPre && wire->rxCount && wire->rx[wire->rxHead] != UDS_WIRE_MAIL_PREAMBLE_BYTE) {
			_rxTake(wire); // what is left of the 3DS's patch block ahead of its mail: not part of the mail, which begins with its $20 run
		}
		if (!wire->rxCount) {
			if (wire->mailMode) {
				// The mail block's replies are all stored, and its first reply, whatever it is, ends the ignoring: an FE would be stored as
				// data like any other. The receiver finds the partner's mail by scanning for the first $20 and skipping
				// the whole run of $20 and FE, so a run of any length is harmless: until the first byte of real mail data is served, late
				// replies are $20 and the real units are kept and served in order (nothing is lost at the head, where the first message
				// is; the tail is cut by as many replies as were late, which is the padding of the patch set). After that a missing reply
				// is a 00 and the real unit is dropped when it arrives, so what follows stays aligned.
				wire->preloadFill = true;
				return wire->mailData ? 0x00 : UDS_WIRE_MAIL_PREAMBLE_BYTE;
			}
			if (wire->blockStoring && wire->blockRemaining == 1) {
				// The last byte the cartridge stores is the first unit of the next block (the 3DS's player block is one unit short of what
				// the cartridge stores), which the 3DS produces only after it has received our block. Stand in for it with what it will be;
				// the real unit is dropped when it arrives.
				wire->preloadFill = true;
				return wire->blockIndex % UDS_WIRE_BLOCKS == 1 ? 0xFD : 0x00;
			}
			return UDS_WIRE_NO_DATA;
		}
		host = wire->rx[wire->rxHead];
		if (!wire->blockStoring && !wire->mailMode && host == 0xFD) {
			// The cartridge is ignoring replies until an fd; once it has one it stores what follows, and a reply that is not there
			// is stored as FE. Hold the fd (the cartridge ignores FE and goes on sending fd) until the whole block is in the buffer,
			// so that every byte after it can be served.
			if (wire->rxCount < kBlockSize[wire->blockIndex % UDS_WIRE_BLOCKS]) {
				return UDS_WIRE_NO_DATA;
			}
		}
		wire->preloadHost = true;
		return host;
	}
	return UDS_WIRE_IDLE_LINE;
}

// One unit per completed exchange. If the reply was a unit from the 3DS the exchange is complete: consume it and send the
// master's byte. If it was our FE nothing completed: the master will re-send the same byte, so its unit is sent once, now (the 3DS
// must see it to produce its own), and not again on the retry.
static void _blockStored(struct UDSWire* wire) {
	if (wire->blockRemaining && --wire->blockRemaining == 0) {
		_trace(wire, "block %u done (%u replies were FE inside it)", wire->blockIndex, wire->blockUnderruns);
		wire->blockStoring = false;
		++wire->blockIndex;
		wire->blockUnderruns = 0;
		if (wire->blockIndex % UDS_WIRE_BLOCKS == 1) {
			wire->rnCovered = false; // the list of the next cycle is covered afresh
		}
		if (wire->gen2 && wire->blockIndex % UDS_WIRE_BLOCKS == 0) {
			wire->mailData = wire->mailPre = false;
				wire->mailMode = true; // the patch lists were the third block; in the Trade Center a fourth follows
		}
	}
}

// Stream positions. Both streams count from their first unit (the 3DS's EF and our first unit are both index -2001), and unit k of
// ours is paired with unit k of the 3DS's: sentUnits is the position our next unit takes, and the unit at the head of the receive
// buffer is the 3DS's unit number recvUnits - rxCount.
static unsigned _headPosition(const struct UDSWire* wire) {
	return wire->recvUnits - wire->rxCount;
}

// While the cartridge waits for a block it sends fd, and they go out only as padding the 3DS needs: never more than one unit ahead
// of what the 3DS has delivered (its exchanges are paced by ours; it never waits on one we have not sent), and once the 3DS's own
// block can be seen in the buffer, never past the position our block must start at (see _alignBlock).
static bool _padAllowed(const struct UDSWire* wire) {
	if (wire->sentUnits > wire->recvUnits) {
		return false;
	}
	if (wire->rnCovered && wire->blockIndex % UDS_WIRE_BLOCKS == 0) {
		// The 3DS's list is under way (it runs ahead of ours), and its player block comes after the list's last position: stop there.
		return wire->sentUnits < wire->rnListEnd;
	}
	if (wire->blockIndex % UDS_WIRE_BLOCKS != 0 && !wire->blockStoring && !wire->mailMode) {
		unsigned i;
		for (i = 0; i < wire->rxCount; ++i) {
			if (wire->rx[(wire->rxHead + i) % UDS_WIRE_RX] == 0xFD) {
				return wire->sentUnits + 1 < _headPosition(wire) + i;
			}
		}
	}
	return true;
}

// The cartridge's ignoring has just ended on the 3DS's first preamble fd of a block (player block or patch lists), at position T.
// The 3DS's block is preamble fd, then data: the player block 6 fd, 441 bytes (name, party, IDs, structs, OT names, nicknames) and 3 of
// padding, 450 in all; the patch lists 3 fd and 197. Its exchanges receive one unit behind what they send, so it stores our units T-1
// to T+448 (measured: with our block 11 late the nickname of the sixth Pokemon kept exactly two letters, the last two our window left
// it), skips the fd and copies 441 bytes from the first other one. The cartridge's block (the same layout) must therefore start at
// T-1, as the 3DS's own did in its stream: what we have sent up to here is padding, so fill up to T-1 with fd, or, if we are already
// past it, leave out as many of the cartridge's own preamble fd as that.
//
// The player block is placed by its data instead (the trainer name, the first byte that is not fd), because that is what the 3DS's
// unpacking finds. T is the position of the 3DS's first preamble fd; its own name is at T+6. Gen 2 stores 450 units of ours from T-1
// and searches from the window's first byte [measured]: our name at T+6 (where the nickname fix put it, live with Azahar) has all 441
// bytes inside, two to spare. Gen 1's window is not measured; the name goes at T+6 too, which needs its start at T+3 or before (its
// unpacking, cable_club.asm, skips the window's first three bytes) and at T-3 or after (415 bytes of data in 424).
//
// The cartridge's own random-number list is not sent (rnSuppress, counted to ten numbers) and must be over here: its bytes lag one
// exchange on the wire, so its tenth number can be the stale byte of the exchange that ends the ignoring. That exchange is not counted,
// and a live Gen 1 trade had the count end on the trainer name's first letter instead, which went out as fd: the 3DS read the whole
// party one byte late (a traded Rapidash arrived as a Nidoking, the next species in the list, with its struct and names shifted).
// The cartridge's first byte after its ignoring ends is sent as fd whatever it is, for the same reason.
static void _alignBlock(struct UDSWire* wire) {
	struct UDSUnitPort* port = &wire->cable.port;
	unsigned t = _headPosition(wire) - 1; // the unit just taken
	if (wire->blockIndex % UDS_WIRE_BLOCKS == 1) {
		wire->dataTarget = t + (wire->gen2 ? UDS_WIRE_DATA_AT_GEN2 : UDS_WIRE_DATA_AT_GEN1);
		wire->dataAligning = true;
		wire->dataFirst = true;
		wire->rnSuppress = false; // the cartridge's list is over (see above)
		wire->blockDrop = 0;
		_trace(wire, "block %u: the 3DS's starts at unit %u; our data will start at %u", wire->blockIndex, t, wire->dataTarget);
		return;
	}
	unsigned target = t ? t - 1 : 0;
	unsigned preamble = wire->blockIndex % UDS_WIRE_BLOCKS == 1 ? UDS_WIRE_PREAMBLE : UDS_WIRE_PATCH_PREAMBLE;
	if (wire->sentUnits <= target) {
		unsigned fill = target - wire->sentUnits;
		unsigned i;
		for (i = 0; i < fill; ++i) {
			port->queue(port->context, 0xFD);
		}
		wire->blockDrop = 0;
		_trace(wire, "block %u: the 3DS's starts at unit %u; ours starts at %u after %u fd of padding", wire->blockIndex, t, target, fill);
	} else {
		unsigned late = wire->sentUnits - target;
		wire->blockDrop = late < preamble ? late : preamble;
		_trace(wire, "block %u: the 3DS's starts at unit %u; ours would start %u late, leaving out %u of its preamble fd%s", wire->blockIndex, t, late,
		       wire->blockDrop, late > preamble ? " (STILL LATE: the end of the block is lost)" : "");
	}
}

// Units are index-paired with the 3DS's: our unit k is what the 3DS receives in its exchange k, so a block we send must begin at
// the index where the 3DS's own block begins, or its ignoring phase ends too early and it stores the wrong bytes (padding fds 25
// positions ahead of its block put our random numbers where its player block started, and the trade menu showed garbage).
// While the cartridge only waits (before a block's data) the exchanges therefore run in lockstep with the 3DS's units: a retry
// after FE is not a new exchange and sends nothing, and the fd it repeats goes out only as the reply to the previous one arrives.
// Any other byte is real data (the tail of the previous block, stale bytes) and always goes. Inside a block's data every exchange
// sends.
static void _pass(struct UDSWire* wire, uint32_t nowMs, uint8_t byte) {
	(void) nowMs;
	struct UDSUnitPort* port = &wire->cable.port;
	bool storing = wire->blockStoring;
	bool opening = false;
	bool answered = false;
	// (Not in the mail block: there an FE is stored, so every exchange is a real one and sends its unit.)
	bool retry = !wire->mailMode && wire->outstanding && wire->outstandingByte == byte && !wire->preloadHost && !wire->preloadFill;
	if (wire->preloadHost && wire->rxCount) {
		uint8_t host = _rxTake(wire);
		answered = true;
		if (wire->mailMode) {
			if (host == UDS_WIRE_MAIL_PREAMBLE_BYTE) {
				wire->mailPre = true;
			} else if (wire->mailPre) {
				wire->mailData = true; // the first byte of mail data after the run
			}
		}
		if (storing) {
			_blockStored(wire);
		} else if (host == 0xFD && !wire->mailMode) {
			// This fd ends the cartridge's ignoring: the replies after it are the block.
			opening = true;
			wire->blockStoring = true;
			wire->blockRemaining = kBlockSize[wire->blockIndex % UDS_WIRE_BLOCKS];
			wire->blockUnderruns = 0;
			wire->pending = 0;
			_trace(wire, "block %u opens, %u units buffered", wire->blockIndex, wire->rxCount);
		}
	} else if (wire->preloadFill) {
		if (!wire->mailMode || wire->mailData) {
			++wire->blockSkip; // the real unit that this stood in for
		} // (ahead of the mail data nothing is dropped: the real stream is served whole after the $20 run, see _preload)
		if (storing) {
			_trace(wire, "block %u: the last byte was not there yet, filled in", wire->blockIndex);
		}
		_blockStored(wire);
	} else if (storing) {
		// No unit in the buffer inside the data: the cartridge stores an FE. The block is damaged by that one byte; the 3DS's
		// next unit is dropped so that the rest does not shift.
		++wire->blockUnderruns;
		++wire->blockSkip;
		_blockStored(wire);
	}
	if (!storing && !wire->mailMode && !wire->rnCovered && wire->blockIndex % UDS_WIRE_BLOCKS == 0 && wire->rxCount && wire->rx[wire->rxHead] == 0xFD) {
		// The 3DS's random-number list is starting (its first fd is next in line). The list is of no consequence (the hook wrapper's
		// trades leave the 3DS's all fd), so the cartridge's own units for it are not sent: fd stand in for them, as many as the 3DS
		// needs to go on (see _padAllowed), so that the stream stays level with the 3DS's for the player block that follows.
		wire->rnCovered = true;
		wire->rnListEnd = _headPosition(wire) + UDS_WIRE_RN_POSITIONS - 1; // its 7 fd and 10 numbers
		wire->rnSuppress = true;
		wire->rnSeenFd = false;
		wire->rnNumbers = 0;
		_trace(wire, "block %u: the 3DS's list starts (units: %u received, %u sent); dropping the cartridge's own", wire->blockIndex, wire->recvUnits, wire->sentUnits);
	}
	if (opening && wire->blockIndex % UDS_WIRE_BLOCKS != 0) {
		_alignBlock(wire); // the cartridge's ignoring fd: the units in front of its block are decided there
	} else if (wire->rnSuppress) {
		// The cartridge's own list (fd preamble, then ten numbers, all below fd) is not sent.
		if (byte == 0xFD) {
			wire->rnSeenFd = true;
		} else if (wire->rnSeenFd && ++wire->rnNumbers >= UDS_WIRE_RN_NUMBERS) {
			wire->rnSuppress = false;
		}
		if (_padAllowed(wire)) {
			port->queue(port->context, 0xFD);
		}
	} else if (storing && wire->dataAligning) {
		// The player block's preamble: fd up to the data's place, then the data from there (see _alignBlock).
		uint8_t sent = wire->dataFirst ? 0xFD : byte;
		wire->dataFirst = false;
		if (sent == 0xFD) {
			if (wire->sentUnits < wire->dataTarget) {
				port->queue(port->context, 0xFD);
			}
		} else {
			unsigned fill = 0;
			while (wire->sentUnits < wire->dataTarget && fill < UDS_WIRE_RX) {
				port->queue(port->context, 0xFD);
				++fill;
			}
			if (wire->sentUnits > wire->dataTarget) {
				_trace(wire, "block %u: our data starts %u units late (STILL LATE: the end of the block is lost)", wire->blockIndex,
				       wire->sentUnits - wire->dataTarget);
			} else {
				_trace(wire, "block %u: our data starts at unit %u", wire->blockIndex, wire->sentUnits);
			}
			port->queue(port->context, sent);
			wire->dataAligning = false;
		}
	} else if (storing && wire->blockDrop && byte == 0xFD) {
		--wire->blockDrop; // a preamble fd of a block that would otherwise start late (see _alignBlock)
	} else if (storing || wire->mailMode) {
		port->queue(port->context, byte);
	} else if (byte == 0xFD) {
		// The cartridge waits for a block, sending fd: they are padding, sent only as the 3DS needs them (see _padAllowed).
		if (_padAllowed(wire)) {
			port->queue(port->context, 0xFD);
		}
	} else if (!retry) {
		port->queue(port->context, byte);
		++wire->pending;
	}
	// The exchange that got FE is retried by the cartridge a frame later with the same byte.
	wire->outstanding = !storing && !opening && !answered && !wire->preloadFill && !wire->mailMode;
	wire->outstandingByte = byte;
	port->flush(port->context);
}

static void _enterPass(struct UDSWire* wire) {
	wire->phase = UDS_WIRE_PASS;
	wire->outstanding = false;
	wire->preloadHost = false;
	wire->blockStoring = false;
	wire->blockIndex = 0;
	wire->blockRemaining = 0;
	wire->blockUnderruns = 0;
	wire->blockSkip = 0;
	wire->blockDrop = 0;
	wire->dataAligning = wire->dataFirst = false;
	wire->mailMode = false;
	wire->mailData = wire->mailPre = false;
	wire->rnCovered = wire->rnSuppress = false;
	wire->rxHead = wire->rxCount = 0;
	wire->pending = 0;
}

static void _startSync(struct UDSWire* wire, uint32_t nowMs, uint8_t byte, uint8_t stale) {
	wire->phase = UDS_WIRE_SYNC;
	wire->cartNybble = byte & 0x0F;
	wire->syncHigh = byte & 0xF0;
	udsCableSetSyncHigh(&wire->cable, wire->syncHigh);
	wire->hostNybble = -1;
	wire->syncDone = false;
	wire->syncPart = UDS_WIRE_SYNC_LOOPS12;
	wire->zeros = 0;
	wire->stale = stale;
	wire->outstanding = false;
	wire->preloadHost = false;
	udsWirePoll(wire, nowMs);
}

static void _enterMenu(struct UDSWire* wire) {
	wire->phase = UDS_WIRE_MENU;
	wire->pendingZero = false;
	wire->hostPress = false;
	wire->cartPress = false;
	wire->menuEra = true;
	wire->echoWanted = false;
	wire->menuHost = 0xD0;
	wire->menuHostKnown = false;
	wire->outstanding = false;
	wire->preloadHost = false;
}

static void _menuByte(struct UDSWire* wire, uint8_t byte) {
	wire->pendingZero = false; // the held 00 was a cycle's stale byte
	if (byte & 0x0C) {
		wire->cartPress = true;
	}
	if (wire->readDebt >= UDS_WIRE_MENU_AHEAD) {
		return; // the 3DS is behind: do not run ahead of its units (the send window is small)
	}
	// The cartridge is not shown the 3DS's unit that pairs with this one (it is told the 3DS's selection instead); the unit is
	// read and dropped as it arrives, and the selection it carries is kept.
	_sendOwed(wire, byte);
}

static void _idle(struct UDSWire* wire, uint32_t nowMs, uint8_t byte) {
	if (wire->strandedPending) {
		// The first byte after a quiet PASS is the last one of it, stranded in the serial register by the lag.
		wire->strandedPending = false;
		_sendOwed(wire, byte);
		return;
	}
	wire->idleZeroSent = true;
	if (_isSync(wire, byte)) {
		_startSync(wire, nowMs, byte, wire->lastMaster);
	} else if (_isMenu(byte)) {
		_enterMenu(wire);
		_menuByte(wire, byte);
	} else if (byte != 0x00 && byte != UDS_WIRE_NO_DATA) {
		// Raw exchange. This one got FE, so the cartridge retries it and the retry is the first.
		_enterPass(wire);
	}
}

static void _syncByte(struct UDSWire* wire, uint32_t nowMs, uint8_t byte) {
	switch (wire->syncPart) {
	case UDS_WIRE_SYNC_LOOPS12:
		if (byte == 0x00) {
			wire->syncPart = UDS_WIRE_SYNC_LOOP3;
			wire->zeros = 1;
		}
		return;
	case UDS_WIRE_SYNC_LOOP3:
		if (byte == 0x00) {
			if (++wire->zeros >= UDS_WIRE_LOOP3_ZEROS) {
				wire->syncPart = UDS_WIRE_SYNC_DONE;
			}
		} else if (_isSync(wire, byte)) {
			wire->syncPart = UDS_WIRE_SYNC_LOOPS12;
			wire->zeros = 0;
		}
		return;
	case UDS_WIRE_SYNC_DONE:
		if (!wire->syncDone) {
			return; // the 3DS side is still finishing; the cartridge keeps getting FE and retrying
		}
		if (_isSync(wire, byte)) {
			_startSync(wire, nowMs, byte, 0x00);
		} else if (_isMenu(byte)) {
			_enterMenu(wire);
			_menuByte(wire, byte);
		} else {
			// Raw exchange from here, which begins with the byte the cartridge's serial register held before the sync. This byte is
			// an exchange like any other (a plain transfer start such as a zero byte is not retried after FE, so it must not be dropped).
			_enterPass(wire);
			_sendOwed(wire, wire->stale);
			_pass(wire, nowMs, byte);
		}
		return;
	}
}

void udsWireExchanged(struct UDSWire* wire, uint32_t nowMs, uint8_t masterByte) {
	if (wire->phase == UDS_WIRE_DOWN) {
		return;
	}
	wire->lastExchangeMs = nowMs;
	switch (wire->phase) {
	case UDS_WIRE_DOWN:
		return;
	case UDS_WIRE_ROLE:
		if (masterByte == 0x01) {
			wire->phase = UDS_WIRE_IDLE;
			wire->idleZeroSent = false;
			wire->strandedPending = false;
		}
		break;
	case UDS_WIRE_IDLE:
		_idle(wire, nowMs, masterByte);
		break;
	case UDS_WIRE_SYNC:
		_syncByte(wire, nowMs, masterByte);
		break;
	case UDS_WIRE_MENU:
		if (_isMenu(masterByte)) {
			_menuByte(wire, masterByte);
		} else if (masterByte == 0x00) {
			wire->pendingZero = true;
		} else {
			_menuEnd(wire);
			_idle(wire, nowMs, masterByte);
		}
		break;
	case UDS_WIRE_PASS:
		_pass(wire, nowMs, masterByte);
		break;
	}
	wire->lastMaster = masterByte;
}
