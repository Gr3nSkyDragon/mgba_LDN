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

static bool _isSync(uint8_t byte) {
	return (byte & 0xF0) == 0x60;
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

void udsWireInit(struct UDSWire* wire, const struct UDSUnitPort* port) {
	memset(wire, 0, sizeof(*wire));
	udsCableInit(&wire->cable, port);
	wire->phase = UDS_WIRE_DOWN;
	wire->hostNybble = -1;
	wire->menuHost = 0xD0;
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
			return 0x60 | wire->hostNybble;
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
		return wire->menuHost;
	case UDS_WIRE_PASS:
		_drain(wire); // a block cannot wait for the next poll
		while (wire->blockSkip && wire->rxCount) {
			_rxTake(wire); // the partner of an FE that was stored as data: dropped so that the block does not shift
			--wire->blockSkip;
		}
		if (!wire->rxCount) {
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
		if (!wire->blockStoring && host == 0xFD) {
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
	bool retry = wire->outstanding && wire->outstandingByte == byte && !wire->preloadHost && !wire->preloadFill;
	if (wire->preloadHost && wire->rxCount) {
		uint8_t host = _rxTake(wire);
		answered = true;
		if (storing) {
			_blockStored(wire);
		} else if (host == 0xFD) {
			// This fd ends the cartridge's ignoring: the replies after it are the block.
			opening = true;
			wire->blockStoring = true;
			wire->blockRemaining = kBlockSize[wire->blockIndex % UDS_WIRE_BLOCKS];
			wire->blockUnderruns = 0;
			wire->pending = 0;
			_trace(wire, "block %u opens, %u units buffered", wire->blockIndex, wire->rxCount);
		}
	} else if (wire->preloadFill) {
		++wire->blockSkip; // the real unit that this stood in for
		_trace(wire, "block %u: the last byte was not there yet, filled in", wire->blockIndex);
		_blockStored(wire);
	} else if (storing) {
		// No unit in the buffer inside the data: the cartridge stores an FE. The block is damaged by that one byte; the 3DS's
		// next unit is dropped so that the rest does not shift.
		++wire->blockUnderruns;
		++wire->blockSkip;
		_blockStored(wire);
	}
	if (!storing && !wire->rnCovered && wire->blockIndex % UDS_WIRE_BLOCKS == 0 && wire->rxCount && wire->rx[wire->rxHead] == 0xFD) {
		// The 3DS's random-number list is starting (its first fd is next in line). Its exchanges are paced by our units: it
		// needs one for each of the list's 18 positions (the fd that ends its ignoring, seven of preamble, ten numbers) and, with
		// the cartridge held until the list is buffered, only we can supply them. The list is of no consequence (the hook wrapper's
		// trades leave the 3DS's all fd), so cover the rest of it with fd and drop the cartridge's own units for it; the player
		// block's first unit is then the next one, which is where the 3DS's player block begins.
		unsigned i;
		for (i = 0; i < UDS_WIRE_RN_POSITIONS - 1; ++i) {
			port->queue(port->context, 0xFD);
		}
		wire->rnCovered = true;
		wire->rnSuppress = true;
		wire->rnSeenFd = false;
		wire->rnNumbers = 0;
		_trace(wire, "block %u: the 3DS's list starts; covered it with %u fd and dropping the cartridge's own", wire->blockIndex, UDS_WIRE_RN_POSITIONS - 1);
	}
	if (wire->rnSuppress) {
		// The cartridge's own list (fd preamble, then ten numbers, all below fd) has been covered: its units are not sent.
		if (byte == 0xFD) {
			wire->rnSeenFd = true;
		} else if (wire->rnSeenFd && ++wire->rnNumbers >= UDS_WIRE_RN_NUMBERS) {
			wire->rnSuppress = false;
		}
	} else if (storing) {
		port->queue(port->context, byte);
	} else if (!retry && (byte != 0xFD || wire->pending < UDS_WIRE_PENDING_MAX)) {
		port->queue(port->context, byte);
		++wire->pending;
	}
	// The exchange that got FE is retried by the cartridge a frame later with the same byte.
	wire->outstanding = !storing && !opening && !answered && !wire->preloadFill;
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
	wire->rnCovered = wire->rnSuppress = false;
	wire->rxHead = wire->rxCount = 0;
	wire->pending = 0;
}

static void _startSync(struct UDSWire* wire, uint32_t nowMs, uint8_t byte, uint8_t stale) {
	wire->phase = UDS_WIRE_SYNC;
	wire->cartNybble = byte & 0x0F;
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
	if (_isSync(byte)) {
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
		} else if (_isSync(byte)) {
			wire->syncPart = UDS_WIRE_SYNC_LOOPS12;
			wire->zeros = 0;
		}
		return;
	case UDS_WIRE_SYNC_DONE:
		if (!wire->syncDone) {
			return; // the 3DS side is still finishing; the cartridge keeps getting FE and retrying
		}
		if (_isSync(byte)) {
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
