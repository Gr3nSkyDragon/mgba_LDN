/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-cable.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void _trace(struct UDSCable* cable, const char* format, ...) {
	if (!cable->port.trace) {
		return;
	}
	char line[200];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	cable->port.trace(cable->port.context, line);
}

static bool _ready(struct UDSCable* cable) {
	return cable->port.ready(cable->port.context);
}

static void _queue(struct UDSCable* cable, uint8_t byte) {
	cable->port.queue(cable->port.context, byte);
}

static void _flush(struct UDSCable* cable) {
	cable->port.flush(cable->port.context);
}

static bool _pop(struct UDSCable* cable, uint8_t* byte) {
	return cable->port.pop(cable->port.context, byte);
}

static bool _peek(struct UDSCable* cable, uint8_t* byte) {
	return cable->port.peek(cable->port.context, byte);
}

void udsCableInit(struct UDSCable* cable, const struct UDSUnitPort* port) {
	memset(cable, 0, sizeof(*cable));
	cable->port = *port;
	cable->syncNybble = -1;
}

void udsCableBegin(struct UDSCable* cable) {
	cable->discardFirst = true;
	_queue(cable, UDS_CABLE_FIRST_UNIT);
	_flush(cable);
	_trace(cable, "link up: first unit %02X sent", UDS_CABLE_FIRST_UNIT);
}

bool udsCableLost(struct UDSCable* cable) {
	bool pending = cable->txPending;
	cable->echoActive = false;
	cable->syncPhase = UDS_SYNC_IDLE;
	cable->txPending = false;
	return pending;
}

bool udsCablePoll(struct UDSCable* cable, uint32_t nowMs, uint8_t* received) {
	uint8_t byte;
	bool completed = false;
	if (cable->discardFirst && cable->port.waiting(cable->port.context)) {
		_pop(cable, &byte);
		cable->discardFirst = false;
		_trace(cable, "host first unit %02X discarded", byte);
	}
	if (cable->txPending && !cable->discardFirst && !cable->busy && _pop(cable, &byte)) {
		_trace(cable, "xfer tx %02X rx %02X", cable->txByte, byte);
		cable->txPending = false;
		*received = byte;
		completed = true;
	}
	// The host pressed A before we did and the Game Boy adopted its choice. The host's own menu loop only ends when a D0-class
	// byte arrives in one of its two kept replies, and which of our units those are depends on how its retries after FE line up
	// with ours, so it can go on exchanging after our Game Boy has left the menu. Answer each of its units with the adopted
	// selection, as the Game Boy would have if it had stayed, until the host has been quiet for a moment.
	if (cable->echoActive && !cable->discardFirst) {
		while (!cable->txPending && cable->port.waiting(cable->port.context) && cable->echoCount < UDS_CABLE_ECHO_MAX_UNITS) {
			_queue(cable, cable->echoByte);
			_pop(cable, &byte);
			++cable->echoCount;
			cable->echoLastMs = nowMs;
			_trace(cable, "menu echo: host unit %02X answered with %02X (%u)", byte, cable->echoByte, cable->echoCount);
		}
		if (nowMs - cable->echoLastMs > UDS_CABLE_ECHO_QUIET_MS || nowMs - cable->echoStartMs > UDS_CABLE_ECHO_MAX_MS ||
		    cable->echoCount >= UDS_CABLE_ECHO_MAX_UNITS) {
			cable->echoActive = false;
			_trace(cable, "menu echo ended after %u units", cable->echoCount);
		}
	}
	_flush(cable);
	return completed;
}

void udsCableTransfer(struct UDSCable* cable, uint8_t byte) {
	if (cable->txPending) {
		return;
	}
	cable->txByte = byte;
	cable->txPending = true;
	_queue(cable, byte);
	_flush(cable);
}

// Serial_SyncAndExchangeNybble, as the ROM's own loops would run it against the host, one frame at a time:
//   1. exchange 60|nybble units, one for each host unit, until a unit from the host is a 6x byte (the ROM's loop1: how long
//      this lasts is how long the host takes to arrive; Azahar's host sent 164 units of 60 in one wait)
//   2. five more 60|nybble and five 00 (the ROM's loop2 and loop3, which the VC shortens: 7 + 5 in a retail pair)
//   3. keep answering the host's burst one unit at a time for as long as it is still sending sync units (00 or 6x), so that
//      both sides leave the sync having sent the same number of units, whatever length the host used.
// Every unit of the host's that is read is matched by one unit of ours, so the two streams stay paired.
static void _syncSend(struct UDSCable* cable, uint8_t byte) {
	_queue(cable, byte);
	++cable->syncSentN;
}

static bool _syncPop(struct UDSCable* cable, uint8_t* byte) {
	if (cable->syncRecvN >= cable->syncSentN || cable->discardFirst || !_pop(cable, byte)) {
		return false;
	}
	++cable->syncRecvN;
	return true;
}

static enum UDSCableStatus _syncFinish(struct UDSCable* cable, int nybble, int* result, enum UDSCableStatus status) {
	cable->syncPhase = UDS_SYNC_IDLE;
	cable->busy = false;
	*result = nybble;
	_trace(cable, "sync done: nybble %X, %u host units followed past the first 12", nybble, cable->syncTail);
	return status;
}

enum UDSCableStatus udsCableSync(struct UDSCable* cable, uint8_t nybble, uint32_t nowMs, int* result) {
	uint8_t byte;
	nybble &= 0x0F;

	if (cable->syncPhase == UDS_SYNC_IDLE) {
		cable->busy = true;
		cable->syncStartMs = nowMs;
		cable->syncSentN = cable->syncRecvN = 0;
		cable->syncTail = 0;
		cable->syncNybble = -1;
		cable->syncPhase = UDS_SYNC_WAIT;
		_trace(cable, "hook: nybble sync, link %s", _ready(cable) ? "up" : "not up yet");
	}

	if (_ready(cable)) {
		if (cable->syncPhase == UDS_SYNC_WAIT) {
			// Phase 1. One unit out for each unit in; nothing is read before it has been paired with one of ours.
			unsigned guard;
			for (guard = 0; guard < 400; ++guard) {
				if (cable->syncSentN == cable->syncRecvN) {
					_syncSend(cable, 0x60 | nybble);
				}
				if (!_syncPop(cable, &byte)) {
					break;
				}
				if ((byte & 0xF0) == 0x60) {
					cable->syncNybble = byte & 0x0F;
					break;
				}
			}
			_flush(cable);
			if (cable->syncNybble >= 0) {
				unsigned i;
				for (i = 0; i < UDS_CABLE_SYNC_SIXTIES; ++i) {
					_syncSend(cable, 0x60 | nybble);
				}
				for (i = 0; i < UDS_CABLE_SYNC_ZEROS; ++i) {
					_syncSend(cable, 0x00);
				}
				_flush(cable);
				cable->syncPhase = UDS_SYNC_AFTER;
				_trace(cable, "sync: host answered %X after %u units; sending %d more %X and %d 00", cable->syncNybble, cable->syncRecvN,
				       UDS_CABLE_SYNC_SIXTIES, nybble, UDS_CABLE_SYNC_ZEROS);
			}
		}
		if (cable->syncPhase == UDS_SYNC_AFTER) {
			while (cable->syncRecvN < cable->syncSentN && _syncPop(cable, &byte)) {
			}
			if (cable->syncRecvN >= cable->syncSentN) {
				cable->syncPhase = UDS_SYNC_TAIL;
				cable->syncTailMs = nowMs;
			}
		}
		if (cable->syncPhase == UDS_SYNC_TAIL) {
			// A unit that is not 00 or 6x belongs to what the host does next: leave it.
			while (cable->syncTail < UDS_CABLE_SYNC_TAIL_MAX && _peek(cable, &byte) && (byte == 0x00 || (byte & 0xF0) == 0x60)) {
				_syncSend(cable, 0x00);
				_syncPop(cable, &byte);
				++cable->syncTail;
				cable->syncTailMs = nowMs;
			}
			_flush(cable);
			bool another = _peek(cable, &byte);
			// Done when the host has moved on (a different unit is waiting), or has been quiet for a moment.
			if ((another && byte != 0x00 && (byte & 0xF0) != 0x60) || cable->syncTail >= UDS_CABLE_SYNC_TAIL_MAX ||
			    nowMs - cable->syncTailMs > UDS_CABLE_SYNC_TAIL_QUIET_MS) {
				return _syncFinish(cable, cable->syncNybble, result, UDS_CABLE_DONE);
			}
		}
	}

	if (nowMs - cable->syncStartMs > UDS_CABLE_SYNC_TIMEOUT_MS && cable->syncPhase == UDS_SYNC_WAIT) {
		// Nobody answered; the Game Boy's own way out is its inactivity counter.
		_trace(cable, "sync timed out");
		return _syncFinish(cable, 0xFF, result, UDS_CABLE_TIMED_OUT);
	}
	return UDS_CABLE_PENDING;
}

// Serial_ExchangeLinkMenuSelection. The ROM exchanges three bytes per call (the first is discarded, the next two are kept)
// and wants a "D0 class" byte in either kept slot. Which of the host's bytes lands in which slot depends on how the two
// loops line up, and the host leaves the menu as soon as it sees our A press, so a missed byte is never repeated. Here a
// call sends the selection three times and keeps the last D0-class byte of the three the host sent, in both slots, which
// does not depend on the alignment. The host reads the same constant selection whichever of our units it looks at.
static enum UDSCableStatus _menuFinish(struct UDSCable* cable, uint32_t nowMs, uint8_t* first, uint8_t* second) {
	int valid = -1;
	unsigned i;
	for (i = 0; i < 3; ++i) {
		if ((cable->menuBytes[i] & 0xF0) == 0xD0) {
			valid = cable->menuBytes[i];
		}
	}
	*first = valid >= 0 ? valid : cable->menuBytes[1];
	*second = valid >= 0 ? valid : cable->menuBytes[2];
	cable->menuActive = false;
	cable->busy = false;
	if ((cable->menuSelection & 0x0C) == 0 && (*first & 0xF0) == 0xD0 && (*first & 0x0C) != 0) {
		cable->echoActive = true;
		cable->echoByte = *first;
		cable->echoStartMs = cable->echoLastMs = nowMs;
		cable->echoCount = 0;
		_trace(cable, "host chose first (%02X): its further menu units will be answered with it", *first);
	}
	_trace(cable, "menu done: sent %02X, host sent %02X %02X %02X -> %02X %02X", cable->menuSelection, cable->menuBytes[0],
	       cable->menuBytes[1], cable->menuBytes[2], *first, *second);
	return UDS_CABLE_DONE;
}

enum UDSCableStatus udsCableMenu(struct UDSCable* cable, uint8_t selection, uint32_t nowMs, uint8_t* first, uint8_t* second) {
	if (!cable->menuActive) {
		cable->menuActive = true;
		cable->busy = true;
		cable->menuSent = false;
		cable->menuGot = 0;
	}

	if (_ready(cable) && !cable->menuSent) {
		unsigned i;
		cable->menuSelection = selection;
		for (i = 0; i < 3; ++i) {
			_queue(cable, selection);
		}
		_flush(cable);
		cable->menuSent = true;
	}

	if (_ready(cable) && cable->menuSent) {
		uint8_t byte;
		while (cable->menuGot < 3 && !cable->discardFirst && _pop(cable, &byte)) {
			cable->menuBytes[cable->menuGot++] = byte;
		}
		if (cable->menuGot >= 3) {
			return _menuFinish(cable, nowMs, first, second);
		}
	}

	if (!_ready(cable) && cable->menuSent) {
		// The link dropped in the middle of a call: give the Game Boy what an idle line gives.
		cable->menuBytes[0] = cable->menuBytes[1] = cable->menuBytes[2] = 0xFF;
		return _menuFinish(cable, nowMs, first, second);
	}
	return UDS_CABLE_PENDING;
}
