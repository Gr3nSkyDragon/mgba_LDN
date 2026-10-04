/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-wire.h>

#include <string.h>

static bool _isSync(uint8_t byte) {
	return (byte & 0xF0) == 0x60;
}

static bool _hostUnit(struct UDSWire* wire, uint8_t* byte) {
	struct UDSUnitPort* port = &wire->cable.port;
	return !wire->cable.discardFirst && port->peek(port->context, byte);
}

void udsWireInit(struct UDSWire* wire, const struct UDSUnitPort* port) {
	memset(wire, 0, sizeof(*wire));
	udsCableInit(&wire->cable, port);
	wire->phase = UDS_WIRE_DOWN;
	wire->hostNybble = -1;
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
		return;
	}
	if (wire->phase == UDS_WIRE_DOWN) {
		if (!wire->begun) {
			udsCableBegin(&wire->cable);
			wire->begun = true;
			wire->phase = UDS_WIRE_ROLE;
		}
		return; // DOWN with the session up and begun: a sync that timed out; the line stays idle until the session restarts
	}

	uint8_t unused;
	udsCablePoll(&wire->cable, nowMs, &unused); // drops the 3DS's first unit and flushes what is queued

	if (wire->phase == UDS_WIRE_SYNC && !wire->syncDone) {
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
	}
}

uint8_t udsWirePreload(struct UDSWire* wire) {
	uint8_t host;
	wire->preloadHost = false;
	switch (wire->phase) {
	case UDS_WIRE_DOWN:
		return UDS_WIRE_IDLE_LINE;
	case UDS_WIRE_ROLE:
		return UDS_WIRE_ROLE_SLAVE;
	case UDS_WIRE_IDLE:
		return wire->idleZeroSent ? UDS_WIRE_NO_DATA : 0x00;
	case UDS_WIRE_SYNC:
		// Nothing until the 3DS's nybble is known (the cartridge's first loop runs until it receives a 6x); then what a slave
		// that had synced with that nybble would answer: 60|nybble while the master sends 6x, 00 while it sends 00.
		if (wire->hostNybble < 0) {
			return UDS_WIRE_NO_DATA;
		}
		if (_isSync(wire->lastMaster)) {
			return 0x60 | wire->hostNybble;
		}
		if (wire->lastMaster == 0x00) {
			return 0x00;
		}
		return UDS_WIRE_NO_DATA;
	case UDS_WIRE_PASS:
		if (_hostUnit(wire, &host)) {
			wire->preloadHost = true;
			return host;
		}
		return UDS_WIRE_NO_DATA;
	}
	return UDS_WIRE_IDLE_LINE;
}

// One unit per completed exchange. If the reply was a unit from the 3DS the exchange is complete: consume it and send the
// master's byte. If it was our FE nothing completed: the master will re-send the same byte, so its unit is sent once, now (the 3DS
// must see it to produce its own), and not again on the retry.
static void _pass(struct UDSWire* wire, uint8_t byte) {
	struct UDSUnitPort* port = &wire->cable.port;
	bool retry = wire->outstanding && byte == wire->outstandingByte;
	if (wire->preloadHost) {
		uint8_t host;
		port->pop(port->context, &host);
		if (!retry) {
			port->queue(port->context, byte);
		}
		wire->outstanding = false;
	} else if (!retry) {
		port->queue(port->context, byte);
		wire->outstanding = true;
		wire->outstandingByte = byte;
	}
	port->flush(port->context);
}

void udsWireExchanged(struct UDSWire* wire, uint32_t nowMs, uint8_t masterByte) {
	switch (wire->phase) {
	case UDS_WIRE_DOWN:
		return;
	case UDS_WIRE_ROLE:
		if (masterByte == 0x01) {
			wire->phase = UDS_WIRE_IDLE;
			wire->idleZeroSent = false;
		}
		return;
	case UDS_WIRE_IDLE:
		wire->idleZeroSent = true;
		if (_isSync(masterByte)) {
			wire->phase = UDS_WIRE_SYNC;
			wire->cartNybble = masterByte & 0x0F;
			wire->hostNybble = -1;
			wire->syncDone = false;
			wire->lastMaster = masterByte;
			udsWirePoll(wire, nowMs);
		}
		return;
	case UDS_WIRE_SYNC:
		wire->lastMaster = masterByte;
		if (wire->syncDone && !_isSync(masterByte) && masterByte != 0x00) {
			// The cartridge has left its sync loops and the 3DS has finished its own: from here on it is plain exchange. This
			// exchange got a synthetic reply (the sync rule above), so it is not paired with a unit; the next one is the first.
			wire->phase = UDS_WIRE_PASS;
			wire->outstanding = false;
			wire->preloadHost = false;
		}
		return;
	case UDS_WIRE_PASS:
		_pass(wire, masterByte);
		return;
	}
}
