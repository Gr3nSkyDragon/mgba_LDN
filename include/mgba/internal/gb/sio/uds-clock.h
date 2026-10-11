/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_CLOCK_H
#define GB_SIO_UDS_CLOCK_H

#include <mgba-util/common.h>

#include <mgba/internal/gb/sio/uds-wire.h>

CXX_GUARD_START

/*
 * The clock side of a slave cartridge: the cartridge sits behind a GB-Link (Pico) in Game Boy mode, whose SPI is master only, so we
 * drive the clock and the cartridge answers as slave. The wire (uds-wire.c) is used unchanged: the byte it would have the slave shift
 * out (udsWirePreload) is the byte we clock, and the cartridge's reply is "the master's byte" (udsWireExchanged). This module decides
 * when to clock and which exchanges the wire sees; it knows nothing of an emulator or a Pico, so the same file runs in mGBA (Local,
 * where mGBA clocks the emulated cartridge) and on the ESP32 board (which clocks a Pico, real or mGBA standing in for one).
 *
 * The Pokemon games treat a byte the same way in either role: their serial interrupt stores rSB as the byte received, loads the next
 * hSerialSendData into rSB and leaves FE (no data) behind (home/serial.asm, Serial; Gen 2 the same). Only two things differ: the
 * master starts every transfer, and in the handshake the side clocking offers 01 while the side waiting answers 02 (the status each
 * takes is the byte it receives). So only the handshake is turned round here.
 *
 * One exchange:
 *   gap = udsClockGap(clock)                     the wait to keep after it, chosen by the phase it starts in
 *   if (!udsClockPrepare(clock, now, &byte))     the mail hold: do not clock now, look again shortly
 *   reply = <clock `byte` into the cartridge>    FF when nobody shifted on the other end
 *   udsClockExchanged(clock, now, reply)
 *
 * `pico`: the Pico's rules, for a clock that cannot see whether the cartridge is armed: an FF outside a block is a clock nobody heard,
 * an FE in a sync a clock that came too early, and the Gen 2 mail is held for the 3DS's units (mailHoldAhead, 0: no hold). Without it
 * (mGBA's Local without the Pico clock) the caller skips the clocks that would find the cartridge unarmed.
 */

// A Gen 2 Trade Center's mail (pokecrystal engine/link/link.asm, ExchangeBytes of wLinkSendMail): one exchange that is ignored, then
// wLinkSendMailEnd - wLinkSendMail = 5 preamble + 6 x 33 messages + 6 x 14 metadata + 6 x 17 + 1 patch set = 390 stored. (Western
// games; the Japanese ones have shorter mail.)
#define UDS_CLOCK_MAIL_EXCHANGES 391
// The mail hold: how many units we may run ahead of the 3DS before the clock waits (32 is the value the mail tests passed with), and
// the longest the clock is held for one 3DS unit. The wire ends a PASS after UDS_WIRE_GAP_MS without an exchange, so a hold never
// comes near that; at the cap the byte is clocked as without the hold.
#define UDS_CLOCK_MAIL_HOLD_AHEAD 32
#define UDS_CLOCK_MAIL_HOLD_MAX_MS 150

// The wait after an exchange. Each side has its own durations: mGBA's in emulated cycles (the values the Local tests passed with),
// the board's in microseconds of real time.
enum UDSClockGap {
	UDS_CLOCK_GAP_LOOP, // idle, sync and menu: a little slower than the slave's frame loops (about 12.6 ms)
	UDS_CLOCK_GAP_ROLE, // between our 01 offers while the cartridge alternates its own clock (1 ms)
	UDS_CLOCK_GAP_BYTE, // after each byte in PASS: in a block the slave waits for every byte anyway (0.5 ms)
};

struct UDSClock {
	struct UDSWire* wire;
	bool pico;
	unsigned mailHoldAhead;
	void (*trace)(void* context, const char* line); // NULL: no trace
	void* traceContext;

	// The exchange in progress, from udsClockPrepare to udsClockExchanged.
	int phase; // the wire's phase when it started
	bool role;
	bool mailOpen; // inside the Gen 2 mail, for as long as it lasts
	bool inBlock; // where an FF is data: inside a block, or the mail
	bool mailFill; // the wire stood in for a 3DS unit of the mail that was not there yet
	uint8_t sent;

	unsigned exchanges; // exchanges given to the wire
	unsigned quiet; // exchanges clocked but kept from it: empty both ways, handshake leftovers, gaps, clocks nobody heard
	unsigned roleOffers; // our 01 offers that nobody took
	unsigned mailExchanges; // exchanges given to the wire since its Gen 2 mail mode began
	unsigned mailFills;
	unsigned mailHolds;
	unsigned mailHoldCapped; // holds that reached UDS_CLOCK_MAIL_HOLD_MAX_MS and clocked anyway
	bool mailHolding;
	uint32_t mailHoldSinceMs;
	uint32_t mailHoldLongestMs;
};

void udsClockInit(struct UDSClock* clock, struct UDSWire* wire, bool pico, unsigned mailHoldAhead);
void udsClockSetTrace(struct UDSClock* clock, void (*trace)(void* context, const char* line), void* context);

// The wait to keep after the exchange about to be clocked. Call it first: it goes by the phase the exchange starts in.
enum UDSClockGap udsClockGap(const struct UDSClock* clock);
// The byte to clock now into *byte; false while the mail hold waits for the 3DS (nothing is clocked: call again soon).
bool udsClockPrepare(struct UDSClock* clock, uint32_t nowMs, uint8_t* byte);
// The cartridge's reply to the byte udsClockPrepare gave (FF when nobody shifted on the other end).
void udsClockExchanged(struct UDSClock* clock, uint32_t nowMs, uint8_t reply);

CXX_GUARD_END

#endif
