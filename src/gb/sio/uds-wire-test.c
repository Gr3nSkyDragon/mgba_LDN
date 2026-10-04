/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Test of uds-wire.c with no emulator and no hardware: a model of the cartridge as the master (Serial_ExchangeByte's FE retry one
 * frame later and the three loops of Serial_SyncAndExchangeNybble, from home/serial.asm), a scripted 3DS behind a UnitPort, and a
 * replay of the first 99 exchanges of a real two-window cable recording (uds-wire-test-vectors.h) for the phase boundaries.
 * Build target: uds-wire-test. Exit status 0 when every check passes.
 */
#include <mgba/internal/gb/sio/uds-wire.h>

#include "uds-wire-test-vectors.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond, ...) \
	do { \
		++checks; \
		if (!(cond)) { \
			++failures; \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

#define MAX_UNITS 1024

// The 3DS: what we sent it, and what it has delivered. Units can be scheduled to arrive at a time.
struct Host {
	bool ready;
	uint8_t sent[MAX_UNITS];
	unsigned sentCount;
	uint8_t queue[MAX_UNITS];
	unsigned head, tail;
	struct {
		uint32_t at;
		uint8_t byte;
		bool done;
	} schedule[MAX_UNITS];
	unsigned scheduled;
};

static bool hostReady(void* context) {
	return ((struct Host*) context)->ready;
}

static bool hostQueue(void* context, uint8_t byte) {
	struct Host* host = context;
	host->sent[host->sentCount++] = byte;
	return true;
}

static void hostFlush(void* context) {
	UNUSED(context);
}

static bool hostPop(void* context, uint8_t* byte) {
	struct Host* host = context;
	if (host->head == host->tail) {
		return false;
	}
	*byte = host->queue[host->head++];
	return true;
}

static bool hostPeek(void* context, uint8_t* byte) {
	struct Host* host = context;
	if (host->head == host->tail) {
		return false;
	}
	*byte = host->queue[host->head];
	return true;
}

static size_t hostWaiting(void* context) {
	struct Host* host = context;
	return host->tail - host->head;
}

static const struct UDSUnitPort kPortTemplate = {
	.ready = hostReady,
	.queue = hostQueue,
	.flush = hostFlush,
	.pop = hostPop,
	.peek = hostPeek,
	.waiting = hostWaiting,
	.trace = NULL,
};

static void hostPush(struct Host* host, uint8_t byte) {
	host->queue[host->tail++] = byte;
}

static void hostPushN(struct Host* host, uint8_t byte, unsigned count) {
	while (count--) {
		hostPush(host, byte);
	}
}

static void hostAt(struct Host* host, uint32_t at, uint8_t byte) {
	host->schedule[host->scheduled].at = at;
	host->schedule[host->scheduled].byte = byte;
	host->schedule[host->scheduled].done = false;
	++host->scheduled;
}

struct Sim {
	struct UDSWire wire;
	struct Host host;
	uint32_t now;
};

static void simInit(struct Sim* sim) {
	memset(sim, 0, sizeof(*sim));
	sim->host.ready = true;
	struct UDSUnitPort port = kPortTemplate;
	port.context = &sim->host;
	udsWireInit(&sim->wire, &port);
}

static void tick(struct Sim* sim) {
	unsigned i;
	for (i = 0; i < sim->host.scheduled; ++i) {
		if (!sim->host.schedule[i].done && sim->host.schedule[i].at <= sim->now) {
			sim->host.schedule[i].done = true;
			hostPush(&sim->host, sim->host.schedule[i].byte);
		}
	}
	udsWirePoll(&sim->wire, sim->now);
}

static void wait(struct Sim* sim, unsigned ms) {
	while (ms--) {
		++sim->now;
		tick(sim);
	}
}

// One exchange as the wire sees it: the slave's byte is decided first, then the master clocks (2 ms later).
static uint8_t exchange(struct Sim* sim, uint8_t byte) {
	tick(sim);
	uint8_t reply = udsWirePreload(&sim->wire);
	sim->now += 2;
	udsWireExchanged(&sim->wire, sim->now, byte);
	return reply;
}

// Serial_ExchangeByte: a reply of FE is "no data", and the same byte is sent again one frame later.
static uint8_t exchangeRetrying(struct Sim* sim, uint8_t byte, unsigned* attempts) {
	unsigned n = 0;
	uint8_t reply;
	do {
		reply = exchange(sim, byte);
		++n;
		if (reply == 0xFE) {
			wait(sim, 16);
		}
	} while (reply == 0xFE && n < 2000);
	if (attempts) {
		*attempts = n;
	}
	return reply;
}

// Serial_SyncAndExchangeNybble: loop 1 sends 60|nybble until a 6x comes back, loop 2 and loop 3 are ten exchanges each (60|nybble,
// then 00), a frame apart. The result is the nybble of the last reply of loop 2.
static int cartSync(struct Sim* sim, uint8_t nybble, unsigned* loop1) {
	unsigned i, n = 0;
	uint8_t reply;
	do {
		reply = exchange(sim, 0x60 | nybble);
		wait(sim, 16);
		++n;
	} while ((reply & 0xF0) != 0x60 && n < 2000);
	if (loop1) {
		*loop1 = n;
	}
	int result = -1;
	for (i = 0; i < 10; ++i) {
		reply = exchange(sim, 0x60 | nybble);
		wait(sim, 16);
		result = (reply & 0xF0) == 0x60 ? reply & 0x0F : -1;
	}
	for (i = 0; i < 10; ++i) {
		exchange(sim, 0x00);
		wait(sim, 16);
	}
	return result;
}

// The role handshake and the idle exchanges before the first sync, as recorded (01 00 00 from the master).
static void handshake(struct Sim* sim, uint8_t replies[3]) {
	replies[0] = exchange(sim, 0x01);
	replies[1] = exchange(sim, 0x00);
	replies[2] = exchange(sim, 0x00);
}

// The 3DS side of a sync on its own, to compare the units sent: what udsCableSync does when nothing else is going on.
static void referenceSync(const uint8_t* hostUnits, unsigned count, uint8_t nybble, uint8_t* sent, unsigned* sentCount) {
	struct Host host;
	memset(&host, 0, sizeof(host));
	host.ready = true;
	struct UDSUnitPort port = kPortTemplate;
	port.context = &host;
	struct UDSCable cable;
	udsCableInit(&cable, &port);
	unsigned i;
	for (i = 0; i < count; ++i) {
		hostPush(&host, hostUnits[i]);
	}
	udsCableBegin(&cable);
	uint32_t now = 0;
	int result;
	uint8_t unused;
	for (; now < 20000; ++now) {
		udsCablePoll(&cable, now, &unused);
		if (udsCableSync(&cable, nybble, now, &result) == UDS_CABLE_DONE) {
			break;
		}
	}
	memcpy(sent, host.sent, host.sentCount);
	*sentCount = host.sentCount;
}

static void testRole(void) {
	struct Sim sim;
	simInit(&sim);
	sim.host.ready = false;
	tick(&sim);
	CHECK(udsWirePreload(&sim.wire) == 0xFF && sim.wire.phase == UDS_WIRE_DOWN, "no session: the cartridge sees an idle line");
	udsWireExchanged(&sim.wire, 1, 0x01);
	CHECK(sim.wire.phase == UDS_WIRE_DOWN && sim.host.sentCount == 0, "exchanges before the session are ignored");

	sim.host.ready = true;
	tick(&sim);
	CHECK(sim.wire.phase == UDS_WIRE_ROLE && sim.host.sentCount == 1 && sim.host.sent[0] == UDS_CABLE_FIRST_UNIT,
	      "the session comes up: first unit sent, role handshake offered");
	uint8_t replies[3];
	handshake(&sim, replies);
	CHECK(replies[0] == 0x02 && replies[1] == 0x00 && replies[2] == 0xFE, "replies to 01 00 00 are 02 00 FE as the real slave's (%02X %02X %02X)",
	      replies[0], replies[1], replies[2]);
	CHECK(sim.wire.phase == UDS_WIRE_IDLE && sim.host.sentCount == 1, "no unit is sent for the role handshake or the idle exchanges");
	CHECK(udsWirePreload(&sim.wire) == 0xFE, "and FE goes on until something happens");

	// The session drops: back to an idle line.
	sim.host.ready = false;
	tick(&sim);
	CHECK(sim.wire.phase == UDS_WIRE_DOWN && udsWirePreload(&sim.wire) == 0xFF, "a lost session shows the cartridge an idle line again");
}

static void testSync(void) {
	// The 3DS is there from the start: its first unit (EF), four 6x with nybble 5, then its zeros.
	struct Sim sim;
	simInit(&sim);
	uint8_t hostUnits[64];
	unsigned count = 0;
	hostUnits[count++] = 0xEF;
	unsigned i;
	for (i = 0; i < 4; ++i) {
		hostUnits[count++] = 0x65;
	}
	for (i = 0; i < 40; ++i) {
		hostUnits[count++] = 0x00;
	}
	for (i = 0; i < count; ++i) {
		hostPush(&sim.host, hostUnits[i]);
	}
	tick(&sim);
	uint8_t replies[3];
	handshake(&sim, replies);
	unsigned loop1 = 0;
	int result = cartSync(&sim, 3, &loop1);
	CHECK(result == 5, "the cartridge's own loops end with the 3DS's nybble (%d)", result);
	CHECK(sim.wire.phase == UDS_WIRE_SYNC, "still in the sync until the cartridge does something else");
	wait(&sim, 600);
	CHECK(sim.wire.syncDone, "the 3DS side of the sync has finished by itself");
	unsigned unitsBefore = sim.host.sentCount;
	uint8_t reply = exchange(&sim, 0xD0);
	CHECK(sim.wire.phase == UDS_WIRE_PASS && sim.host.sentCount == unitsBefore, "the first byte that is not sync ends it, without a unit of its own");
	CHECK(reply == 0x00, "(the reply to that exchange was still the sync rule's)");

	uint8_t reference[MAX_UNITS];
	unsigned referenceCount = 0;
	referenceSync(hostUnits, count, 3, reference, &referenceCount);
	CHECK(referenceCount == sim.host.sentCount && !memcmp(reference, sim.host.sent, referenceCount),
	      "the units the 3DS gets are exactly what its side of the sync produces alone (%u vs %u units)", sim.host.sentCount, referenceCount);

	// The 3DS is late: its 6x arrives 300 ms after the cartridge begins; until then the cartridge gets FE and retries.
	simInit(&sim);
	hostPush(&sim.host, 0xEF);
	hostAt(&sim.host, 300, 0x65);
	hostAt(&sim.host, 316, 0x65);
	hostAt(&sim.host, 332, 0x65);
	for (i = 0; i < 40; ++i) {
		hostAt(&sim.host, 340 + 16 * i, 0x00);
	}
	tick(&sim);
	handshake(&sim, replies);
	result = cartSync(&sim, 3, &loop1);
	CHECK(result == 5, "a late 3DS: the same nybble (%d)", result);
	CHECK(loop1 > 10, "the cartridge's first loop kept retrying while the 3DS had nothing (%u attempts)", loop1);
	wait(&sim, 600);
	exchange(&sim, 0xD0);
	CHECK(sim.wire.phase == UDS_WIRE_PASS, "and the sync still ends cleanly");
	CHECK(sim.host.sent[1] == 0x63, "our first sync unit went out at once, before the 3DS's reply (unit %02X)", sim.host.sent[1]);

	// Nobody answers: the 3DS side times out and the cartridge sees an idle line.
	simInit(&sim);
	hostPush(&sim.host, 0xEF);
	tick(&sim);
	handshake(&sim, replies);
	exchange(&sim, 0x63);
	wait(&sim, UDS_CABLE_SYNC_TIMEOUT_MS + 100);
	CHECK(sim.wire.phase == UDS_WIRE_DOWN, "a sync nobody answers ends in an idle line");
	CHECK(udsWirePreload(&sim.wire) == 0xFF, "FF");
}

static void bringToPass(struct Sim* sim) {
	uint8_t replies[3];
	simInit(sim);
	hostPush(&sim->host, 0xEF);
	hostPushN(&sim->host, 0x60, 3);
	hostPushN(&sim->host, 0x00, 30);
	tick(sim);
	handshake(sim, replies);
	cartSync(sim, 0, NULL);
	wait(sim, 600);
	exchange(sim, 0xD0); // leaves the sync
}

static void testPass(void) {
	struct Sim sim;
	bringToPass(&sim);
	CHECK(sim.wire.phase == UDS_WIRE_PASS, "in the pass-through phase");
	// Host units are there: each exchange takes one and sends one.
	hostPush(&sim.host, 0xA1);
	hostPush(&sim.host, 0xA2);
	unsigned base = sim.host.sentCount;
	uint8_t reply = exchange(&sim, 0x11);
	CHECK(reply == 0xA1 && sim.host.sentCount == base + 1 && sim.host.sent[base] == 0x11, "a unit from the 3DS is the reply and the master's byte goes out (%02X)", reply);
	reply = exchange(&sim, 0x22);
	CHECK(reply == 0xA2 && sim.host.sentCount == base + 2 && sim.host.sent[base + 1] == 0x22, "and the next");
	CHECK(sim.host.head == sim.host.tail, "both of the 3DS's units were consumed");

	// Nothing from the 3DS: FE, and the unit goes out once even though the cartridge retries.
	reply = exchange(&sim, 0x33);
	CHECK(reply == 0xFE && sim.host.sentCount == base + 3 && sim.host.sent[base + 2] == 0x33, "no unit from the 3DS: FE, and the master's byte is sent at once");
	reply = exchange(&sim, 0x33);
	CHECK(reply == 0xFE && sim.host.sentCount == base + 3, "the cartridge's retry does not send it again");
	hostPush(&sim.host, 0xB1);
	reply = exchange(&sim, 0x33);
	CHECK(reply == 0xB1 && sim.host.sentCount == base + 3 && sim.host.head == sim.host.tail, "when the 3DS's unit arrives it is the reply, and nothing is sent twice");
	reply = exchange(&sim, 0x44);
	CHECK(reply == 0xFE && sim.host.sentCount == base + 4 && sim.host.sent[base + 3] == 0x44, "the next exchange is a new one");

	// The same byte twice in a row is two exchanges when the first completed.
	hostPush(&sim.host, 0xC1);
	hostPush(&sim.host, 0xC2);
	reply = exchange(&sim, 0x44);
	CHECK(reply == 0xC1 && sim.host.sentCount == base + 4, "a retry of the byte that was waiting completes it (%02X)", reply);
	reply = exchange(&sim, 0x44);
	CHECK(reply == 0xC2 && sim.host.sentCount == base + 5, "and the same byte again is a new exchange");

	// An FE that is the 3DS's own unit is data: the master retries, and that is a new exchange.
	hostPush(&sim.host, 0xFE);
	reply = exchange(&sim, 0x55);
	CHECK(reply == 0xFE && sim.host.head == sim.host.tail && sim.host.sentCount == base + 6, "an FE unit from the 3DS completes the exchange (%u)", sim.host.sentCount - base);
	hostPush(&sim.host, 0xD1);
	reply = exchange(&sim, 0x55);
	CHECK(reply == 0xD1 && sim.host.sentCount == base + 7, "so the master's retry is a new unit");

	// The session drops in the middle: an idle line, nothing sent.
	sim.host.ready = false;
	tick(&sim);
	unsigned sentBefore = sim.host.sentCount;
	reply = exchange(&sim, 0x66);
	CHECK(reply == 0xFF && sim.host.sentCount == sentBefore, "a dropped session: FF and no units");
}

// The first 99 recorded exchanges: role handshake, the first sync (slave late by 39 exchanges), the menu up to the slave's d4.
static void testReplay(void) {
	struct Sim sim;
	simInit(&sim);
	uint8_t hostUnits[64];
	unsigned count = 0, i;
	hostUnits[count++] = 0xEF;
	for (i = 0; i < 4; ++i) {
		hostUnits[count++] = 0x65;
	}
	for (i = 0; i < 40; ++i) {
		hostUnits[count++] = 0x00;
	}
	for (i = 0; i < count; ++i) {
		hostPush(&sim.host, hostUnits[i]);
	}
	sim.now = 0;
	tick(&sim);
	int syncAt = -1, passAt = -1, idleAt = -1;
	unsigned sentAtPass = 0;
	bool classOk = true, nothingEarly = true, hostKnownReplies = true;
	for (i = 0; i < sizeof(kRecordedMaster); ++i) {
		sim.now = kRecordedTimeMs[i] + 1000;
		tick(&sim);
		uint8_t reply = udsWirePreload(&sim.wire);
		if (i < 3 && reply != kRecordedSlave[i]) {
			classOk = false;
		}
		if (sim.wire.phase == UDS_WIRE_SYNC && sim.wire.hostNybble >= 0) {
			uint8_t last = sim.wire.lastMaster;
			uint8_t want = ((last & 0xF0) == 0x60) ? (0x60 | 5) : (last == 0 ? 0 : 0xFE);
			hostKnownReplies = hostKnownReplies && reply == want;
		} else if (sim.wire.phase == UDS_WIRE_SYNC) {
			hostKnownReplies = hostKnownReplies && reply == 0xFE;
		}
		udsWireExchanged(&sim.wire, sim.now, kRecordedMaster[i]);
		if (idleAt < 0 && sim.wire.phase == UDS_WIRE_IDLE) {
			idleAt = (int) i;
		}
		if (syncAt < 0 && sim.wire.phase == UDS_WIRE_SYNC) {
			syncAt = (int) i;
			nothingEarly = sim.host.sentCount >= 1;
		}
		if (passAt < 0 && sim.wire.phase == UDS_WIRE_PASS) {
			passAt = (int) i;
			sentAtPass = sim.host.sentCount;
		}
	}
	CHECK(classOk, "the first three replies are the real slave's (02 00 FE)");
	CHECK(idleAt == 0, "connected after the first exchange (%d)", idleAt);
	CHECK(syncAt == 4, "the sync starts at the first 6x, exchange 4 (%d)", syncAt);
	CHECK(passAt == 67, "it ends at the first d0, exchange 67 (%d)", passAt);
	CHECK(nothingEarly, "(sanity)");
	CHECK(hostKnownReplies, "during the sync the replies follow the rule: FE, then 65 while the master sends 6x, 00 while it sends 00");

	uint8_t reference[MAX_UNITS];
	unsigned referenceCount = 0;
	referenceSync(hostUnits, count, 0, reference, &referenceCount);
	CHECK(referenceCount == sentAtPass && !memcmp(reference, sim.host.sent, referenceCount),
	      "on the real master's bytes the 3DS gets the same units as its side of the sync produces alone, up to the end of the sync (%u vs %u)",
	      sentAtPass, referenceCount);
}

int main(void) {
	testRole();
	testSync();
	testPass();
	testReplay();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
