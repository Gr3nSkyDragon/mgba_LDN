/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-clock.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char* const kPhases[] = {"down", "role", "idle", "sync", "menu", "pass"};

static void _trace(struct UDSClock* clock, const char* format, ...) {
	if (!clock->trace) {
		return;
	}
	char line[256];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	clock->trace(clock->traceContext, line);
}

void udsClockInit(struct UDSClock* clock, struct UDSWire* wire, bool pico, unsigned mailHoldAhead) {
	memset(clock, 0, sizeof(*clock));
	clock->wire = wire;
	clock->pico = pico;
	clock->mailHoldAhead = pico ? mailHoldAhead : 0; // the hold is a Pico rule
}

void udsClockSetTrace(struct UDSClock* clock, void (*trace)(void* context, const char* line), void* context) {
	clock->trace = trace;
	clock->traceContext = context;
}

// A master Pokemon clocks once a frame in its sync, menu and idle loops and byte after byte in a block (Serial_ExchangeBytes). We
// stay a little slower than the slave's frame loops; in a block it waits for every byte anyway.
enum UDSClockGap udsClockGap(const struct UDSClock* clock) {
	switch (clock->wire->phase) {
	case UDS_WIRE_ROLE:
		return UDS_CLOCK_GAP_ROLE;
	case UDS_WIRE_PASS:
		return UDS_CLOCK_GAP_BYTE;
	default:
		return UDS_CLOCK_GAP_LOOP;
	}
}

bool udsClockPrepare(struct UDSClock* clock, uint32_t nowMs, uint8_t* byte) {
	struct UDSWire* wire = clock->wire;
	const int phase = wire->phase;
	// Where FF is data (Pico clock): inside a block, and inside the Gen 2 mail for as long as it lasts. The wire's mail mode ends
	// only on a quiet gap, but the game's mail is exactly UDS_CLOCK_MAIL_EXCHANGES exchanges; after them the cartridge is back in
	// its menu, unarmed, and an FF is a clock nobody heard.
	// Only a Trade Center has mail (link.asm: LINK_TRADECENTER), and the wire knows the room by its sync's range (SERIAL_TIMECAPSULE
	// 60, SERIAL_TRADECENTER 70, SERIAL_BATTLE 80). After a Time Capsule's blocks the wire still waits in mail mode, but the cartridge
	// is in its trade menu: counting its FF (unarmed) as mail kept the wire busy for seconds, the gap that ends PASS never came, and
	// the selection's sync (62...) was taken for data (the board's slower clock catches the cartridge unarmed far more often).
	const bool tradeCenter = wire->syncHigh == 0x70;
	if (!wire->mailMode || !tradeCenter) {
		clock->mailExchanges = 0;
		clock->mailFills = clock->mailHolds = clock->mailHoldCapped = 0;
		clock->mailHoldLongestMs = 0;
		clock->mailHolding = false;
	}
	clock->phase = phase;
	clock->mailOpen = wire->mailMode && tradeCenter && clock->mailExchanges < UDS_CLOCK_MAIL_EXCHANGES;
	clock->inBlock = phase == UDS_WIRE_PASS && (wire->blockStoring || clock->mailOpen);
	clock->role = phase == UDS_WIRE_ROLE;
	clock->sent = clock->role ? 0x01 : udsWirePreload(wire);
	clock->mailFill = clock->mailOpen && wire->preloadFill;
	if (clock->mailFill && clock->mailHoldAhead) {
		// Mail hold. The 3DS sends its unit k only once our unit k has reached it, so a clock that runs on while its units are on
		// the way gives the cartridge a stand-in for each ($20 ahead of the mail data, which costs as many bytes at the end of the
		// mail; 00 after it, a damaged byte). A board decides when the Pico clocks, so it can wait instead: once we are
		// mailHoldAhead units ahead (enough for the 3DS to go on with), and from the first byte of mail data on whenever we are
		// ahead at all, the clock waits for the 3DS's unit. The cartridge waits armed for as long as it takes (its mail is a plain
		// ExchangeBytes and the Trade Center's wLinkTimeoutFrames is 0).
		const int ahead = (int) (wire->sentUnits - wire->recvUnits);
		const int need = wire->mailData ? 1 : (int) clock->mailHoldAhead;
		if (ahead >= need) {
			if (!clock->mailHolding) {
				clock->mailHolding = true;
				clock->mailHoldSinceMs = nowMs;
				++clock->mailHolds;
			}
			if (nowMs - clock->mailHoldSinceMs < UDS_CLOCK_MAIL_HOLD_MAX_MS) {
				return false;
			}
			++clock->mailHoldCapped;
		}
	}
	if (clock->mailHolding) {
		clock->mailHolding = false;
		if (nowMs - clock->mailHoldSinceMs > clock->mailHoldLongestMs) {
			clock->mailHoldLongestMs = nowMs - clock->mailHoldSinceMs;
		}
	}
	*byte = clock->sent;
	return true;
}

void udsClockExchanged(struct UDSClock* clock, uint32_t nowMs, uint8_t reply) {
	struct UDSWire* wire = clock->wire;
	const int phase = clock->phase;
	const uint8_t sent = clock->sent;
	if (!clock->role) {
		// A master cartridge clocks only when its code has a byte to exchange, and the wire reads its silences: a quiet gap is what
		// ends a MENU or a PASS. We clock at a steady rate instead, so most clocks find the game outside its exchange routines and
		// get FE back. When neither side had anything (our FE, its FE), a master would not have clocked at all: the wire does not
		// see it, and its gap timers see the silence a master leaves. The handshake's own bytes (01, 02), which the cartridge's
		// Cable Club code keeps loading while it waits, are stale in IDLE as 00 and FE already are.
		// In MENU an FE from the cartridge is a clock that caught it between selections, not the end of the menu (a master never sends
		// one there, which is why the wire reads it as the end). Both sides repeat their selection, so a lost or repeated one is harmless.
		// In PASS likewise: the game is between exchanges (in a Time Capsule trade's menu, for one, while the wire still waits for a
		// Gen 2 mail block that a Gen 1 partner never sends). A master would have stopped clocking, the wire's gap would end PASS and
		// the selection's nybble sync would be seen. Our byte is not taken from the wire, so whatever it had is offered again.
		const bool empty = sent == UDS_WIRE_NO_DATA && reply == UDS_WIRE_NO_DATA;
		const bool handshake = phase == UDS_WIRE_IDLE && (reply == 0x01 || reply == 0x02);
		const bool menuGap = phase == UDS_WIRE_MENU && reply == UDS_WIRE_NO_DATA;
		const bool passGap = phase == UDS_WIRE_PASS && reply == UDS_WIRE_NO_DATA;
		// Pico clock: an idle line reads FF, and the board cannot tell a cartridge that was not listening (Gen 2 is armed only
		// inside an exchange call) from one that sent FF. Inside a block (and the Gen 2 mail) FF is data, a patched FE or a patch
		// list's end, and the slave waits armed for every byte; anywhere else it is a clock nobody heard. Our byte was not taken.
		const bool absent = clock->pico && reply == 0xFF && !clock->inBlock;
		// Pico clock, in SYNC: an FE is a clock that caught the cartridge between its sync and the link menu (the byte it loaded for
		// the menu not yet in rSB). The wire takes the first byte after the sync to decide what follows, and an FE there sent it to
		// PASS instead of MENU: the 3DS never got our selection and stayed at "Please wait". The wire ignores FE everywhere else in
		// SYNC, so only that decision changes; it waits for the cartridge's real byte.
		const bool syncGap = clock->pico && phase == UDS_WIRE_SYNC && reply == UDS_WIRE_NO_DATA;
		if (empty || handshake || menuGap || passGap || absent || syncGap) {
			++clock->quiet;
			return;
		}
		udsWireExchanged(wire, nowMs, reply);
		if (wire->mailMode && wire->syncHigh == 0x70) { // a Trade Center's mail (see udsClockPrepare)
			if (clock->mailFill) {
				++clock->mailFills;
			}
			if (++clock->mailExchanges == UDS_CLOCK_MAIL_EXCHANGES && clock->pico) {
				if (clock->mailHoldAhead) {
					_trace(clock, "mail done: %u of its %u exchanges got a stand-in for a 3DS unit not there yet; %u holds, the longest "
					       "%u ms, %u of them clocked anyway at %u ms", clock->mailFills, UDS_CLOCK_MAIL_EXCHANGES, clock->mailHolds,
					       (unsigned) clock->mailHoldLongestMs, clock->mailHoldCapped, UDS_CLOCK_MAIL_HOLD_MAX_MS);
				} else {
					_trace(clock, "mail done: %u of its %u exchanges got a stand-in for a 3DS unit not there yet (the end of the 3DS's "
					       "mail is cut by as many)", clock->mailFills, UDS_CLOCK_MAIL_EXCHANGES);
				}
			}
		}
	} else if (reply == 0x02) {
		// ESTABLISH_CONNECTION_WITH_EXTERNAL_CLOCK: the cartridge takes the slave's part. The wire leaves ROLE as if a master
		// cartridge had offered 01 to it.
		udsWireExchanged(wire, nowMs, 0x01);
		_trace(clock, "clock: the cartridge answered 02, it is the slave");
	} else {
		// An offer nobody took (the cartridge is clocking its own try, or not armed): not an exchange, and kept out of the trace's
		// budget after the first few so that what follows the handshake is recorded.
		if (clock->trace && ++clock->roleOffers <= 16) {
			_trace(clock, "clock %02X -> %02X [role]", sent, reply);
		}
		return;
	}
	++clock->exchanges;
	if (clock->trace && clock->exchanges <= 8000) {
		_trace(clock, "clock %02X -> %02X [%s]", sent, reply, wire->phase <= UDS_WIRE_PASS ? kPhases[wire->phase] : "?");
	}
}
