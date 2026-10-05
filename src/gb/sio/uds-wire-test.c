/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Test of uds-wire.c with no emulator and no hardware: a model of the cartridge as the master (Serial_ExchangeByte's FE retry one
 * frame later, the three loops of Serial_SyncAndExchangeNybble and the link menu's four-byte cycle, from home/serial.asm), a
 * scripted 3DS behind a UnitPort, and a replay of the first 99 exchanges of a real two-window cable recording
 * (uds-wire-test-vectors.h) for the phase boundaries. Build target: uds-wire-test. Exit status 0 when every check passes.
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

#define MAX_UNITS 2048

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
	// A 3DS whose exchanges block: it releases its next scripted unit only when it receives one of ours.
	bool reactive;
	uint8_t script[MAX_UNITS];
	unsigned releaseAt[MAX_UNITS]; // a scripted unit is released once the 3DS has received this many of ours
	unsigned scriptLen, scriptPos;
};

static bool hostReady(void* context) {
	return ((struct Host*) context)->ready;
}

static void hostRelease(struct Host* host) {
	while (host->reactive && host->scriptPos < host->scriptLen && host->sentCount >= host->releaseAt[host->scriptPos]) {
		host->queue[host->tail++] = host->script[host->scriptPos++];
	}
}

static bool hostQueue(void* context, uint8_t byte) {
	struct Host* host = context;
	host->sent[host->sentCount++] = byte;
	hostRelease(host);
	return true;
}

static void hostFlush(void* context) {
	UNUSED(context);
}

static bool hostPop(void* context, uint8_t* byte) {
	struct Host* host = context;
	hostRelease(host);
	if (host->head == host->tail) {
		return false;
	}
	*byte = host->queue[host->head++];
	return true;
}

static bool hostPeek(void* context, uint8_t* byte) {
	struct Host* host = context;
	hostRelease(host);
	if (host->head == host->tail) {
		return false;
	}
	*byte = host->queue[host->head];
	return true;
}

static size_t hostWaiting(void* context) {
	struct Host* host = context;
	hostRelease(host);
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

// Serial_SyncAndExchangeNybble: loop 1 sends 60|nybble until a 6x comes back, loop 2 and loop 3 are ten exchanges each (60|nybble,
// then 00), a frame apart. The result is the nybble of the last reply of loop 2.
static int cartSyncHigh(struct Sim* sim, uint8_t high, uint8_t nybble, unsigned* loop1) {
	unsigned i, n = 0;
	uint8_t reply;
	do {
		reply = exchange(sim, high | nybble);
		wait(sim, 16);
		++n;
	} while ((reply & 0xF0) != high && n < 2000);
	if (loop1) {
		*loop1 = n;
	}
	int result = -1;
	for (i = 0; i < 10; ++i) {
		reply = exchange(sim, high | nybble);
		wait(sim, 16);
		result = (reply & 0xF0) == high ? reply & 0x0F : -1;
	}
	for (i = 0; i < 10; ++i) {
		exchange(sim, 0x00);
		wait(sim, 16);
	}
	return result;
}

static int cartSync(struct Sim* sim, uint8_t nybble, unsigned* loop1) {
	return cartSyncHigh(sim, 0x60, nybble, loop1);
}

// The role handshake and the idle exchanges before the first sync, as recorded (01 00 00 from the master).
static void handshake(struct Sim* sim, uint8_t replies[3]) {
	replies[0] = exchange(sim, 0x01);
	replies[1] = exchange(sim, 0x00);
	replies[2] = exchange(sim, 0x00);
}

// The link menu's cycle as recorded: a stale 00, then the selection three times.
static void menuCycle(struct Sim* sim, uint8_t selection, uint8_t replies[4]) {
	replies[0] = exchange(sim, 0x00);
	replies[1] = exchange(sim, selection);
	replies[2] = exchange(sim, selection);
	replies[3] = exchange(sim, selection);
	wait(sim, 60);
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

// A 3DS with a sync ready from the start: its first unit (EF), four 6x with nybble 5, then its zeros.
static unsigned hostSyncUnits(uint8_t* units, uint8_t nybble) {
	unsigned count = 0, i;
	units[count++] = 0xEF;
	for (i = 0; i < 4; ++i) {
		units[count++] = 0x60 | nybble;
	}
	for (i = 0; i < 40; ++i) {
		units[count++] = 0x00;
	}
	return count;
}

static void bringToSyncDone(struct Sim* sim, uint8_t staleBeforeSync) {
	uint8_t replies[3], hostUnits[64];
	unsigned count = hostSyncUnits(hostUnits, 5), i;
	simInit(sim);
	for (i = 0; i < count; ++i) {
		hostPush(&sim->host, hostUnits[i]);
	}
	tick(sim);
	handshake(sim, replies);
	if (staleBeforeSync != 0x00) {
		exchange(sim, staleBeforeSync);
	}
	cartSync(sim, 3, NULL);
	wait(sim, 600);
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
	struct Sim sim;
	uint8_t replies[3], hostUnits[64];
	unsigned count = hostSyncUnits(hostUnits, 5), i, loop1 = 0;

	// The 3DS is there from the start.
	bringToSyncDone(&sim, 0x00);
	CHECK(sim.wire.syncDone, "the 3DS side of the sync has finished by itself");
	CHECK(sim.wire.phase == UDS_WIRE_SYNC && sim.wire.syncPart == UDS_WIRE_SYNC_DONE, "the cartridge's own loops are counted to the end");
	unsigned unitsBefore = sim.host.sentCount;
	exchange(&sim, 0xD0);
	CHECK(sim.wire.phase == UDS_WIRE_MENU && sim.host.sentCount == unitsBefore + 1 && sim.host.sent[unitsBefore] == 0xD0,
	      "a Dx byte after the sync starts the menu and is the first menu unit");
	uint8_t reference[MAX_UNITS];
	unsigned referenceCount = 0;
	referenceSync(hostUnits, count, 3, reference, &referenceCount);
	CHECK(referenceCount == unitsBefore && !memcmp(reference, sim.host.sent, referenceCount),
	      "the units the 3DS gets are exactly what its side of the sync produces alone (%u vs %u units)", unitsBefore, referenceCount);
	int result = cartSync(&sim, 3, NULL); // (a second sync from here, with nothing queued, must not confuse anything)
	CHECK(result == -1 || result == 5, "(sanity)");

	// Same, leaving by a raw byte: the byte that preceded the sync is sent first; the cartridge's cycle of stale 00 is that byte here.
	bringToSyncDone(&sim, 0x00);
	unitsBefore = sim.host.sentCount;
	exchange(&sim, 0xFD);
	CHECK(sim.wire.phase == UDS_WIRE_PASS && sim.host.sentCount == unitsBefore + 2 && sim.host.sent[unitsBefore] == 0x00 &&
	          sim.host.sent[unitsBefore + 1] == 0xFD,
	      "a raw byte after the sync: the stale byte from before it goes out as the first unit, then the byte itself (%02X %02X)",
	      sim.host.sent[unitsBefore], sim.host.sent[unitsBefore + 1]);
	bringToSyncDone(&sim, 0xFE);
	unitsBefore = sim.host.sentCount;
	exchange(&sim, 0xFD);
	CHECK(sim.wire.phase == UDS_WIRE_PASS && sim.host.sentCount == unitsBefore + 2 && sim.host.sent[unitsBefore] == 0xFE,
	      "and when it was FE (the first room sync) it is FE (%02X)", sim.host.sent[unitsBefore]);

	// A second sync right after the first, as in the trade (nybble 0, then 2): the 3DS's second burst arrives a second later.
	simInit(&sim);
	for (i = 0; i < count; ++i) {
		hostPush(&sim.host, hostUnits[i]);
	}
	hostAt(&sim.host, 3000, 0x67);
	hostAt(&sim.host, 3016, 0x67);
	hostAt(&sim.host, 3032, 0x67);
	for (i = 0; i < 40; ++i) {
		hostAt(&sim.host, 3050 + 16 * i, 0x00);
	}
	tick(&sim);
	handshake(&sim, replies);
	cartSync(&sim, 3, NULL);
	wait(&sim, 600);
	exchange(&sim, 0x62); // the next sync starts at once
	CHECK(sim.wire.phase == UDS_WIRE_SYNC && sim.wire.cartNybble == 2 && !sim.wire.syncDone && sim.wire.hostNybble == -1, "a 6x after a finished sync starts the next one");
	wait(&sim, 3000);
	result = cartSync(&sim, 2, &loop1);
	CHECK(result == 7, "the second sync ends with the 3DS's second nybble (%d)", result);

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
	CHECK(sim.wire.phase == UDS_WIRE_MENU, "and the sync still ends cleanly");
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
	CHECK(sim.host.sentCount < 40, "and no second session starts (%u units)", sim.host.sentCount);
}

static void bringToMenu(struct Sim* sim) {
	bringToSyncDone(sim, 0x00);
	exchange(sim, 0xD0);
	sim->host.head = sim->host.tail; // whatever the 3DS had left over from the sync
	sim->wire.readDebt = 0; // and start the menu with nothing owed, so the counts below are the menu's alone
}

// Gen 2 syncs by link mode: 70 in the Trade Center, 80 in the Colosseum. They are syncs only when the wire is told it is Gen 2 (a 7x in Gen 1
// is data), the 3DS's nybble comes back in the same range, and a 3DS still in another range is ignored, as the ROM's own code does.
static void testGen2Sync(void) {
	unsigned high;
	for (high = 0x70; high <= 0x80; high += 0x10) {
		struct Sim sim;
		simInit(&sim);
		udsWireSetGeneration(&sim.wire, 2);
		unsigned i;
		hostPush(&sim.host, 0xEF);
		for (i = 0; i < 4; ++i) {
			hostPush(&sim.host, high | 5);
		}
		for (i = 0; i < 40; ++i) {
			hostPush(&sim.host, 0x00);
		}
		uint8_t replies[3];
		tick(&sim);
		handshake(&sim, replies);
		wait(&sim, 300);
		unsigned loop1 = 0;
		int result = cartSyncHigh(&sim, high, 3, &loop1);
		CHECK(sim.wire.syncHigh == high && sim.wire.phase == UDS_WIRE_SYNC, "a %02Xx byte starts a sync in Gen 2 (phase %d)", high, sim.wire.phase);
		CHECK(result == 5, "the cartridge's loop gets the 3DS's nybble back in its own range (%d)", result);
		CHECK(sim.wire.syncDone && sim.wire.syncPart == UDS_WIRE_SYNC_DONE, "and its loops and the 3DS's side both finish");
		bool allInRange = true;
		for (i = 1; i < 6 && i < sim.host.sentCount; ++i) {
			allInRange = allInRange && (sim.host.sent[i] & 0xF0) == high;
		}
		CHECK(allInRange && sim.host.sent[1] == (high | 3), "the 3DS is sent %02X|nybble units (%02X)", high, sim.host.sent[1]);
	}

	// Gen 1: a 7x is not a sync.
	struct Sim sim;
	simInit(&sim);
	hostPush(&sim.host, 0xEF);
	uint8_t replies[3];
	tick(&sim);
	handshake(&sim, replies);
	wait(&sim, 300);
	exchange(&sim, 0x73);
	CHECK(sim.wire.phase != UDS_WIRE_SYNC, "in Gen 1 a 7x does not start a sync");

	// Gen 2: a 3DS still in the 60 range does not answer a 70 sync (the ROM ignores a reply in the wrong range, and so does the wire).
	simInit(&sim);
	udsWireSetGeneration(&sim.wire, 2);
	unsigned i;
	hostPush(&sim.host, 0xEF);
	for (i = 0; i < 6; ++i) {
		hostPush(&sim.host, 0x65);
	}
	tick(&sim);
	handshake(&sim, replies);
	wait(&sim, 300);
	uint8_t reply = 0;
	for (i = 0; i < 20; ++i) {
		reply = exchange(&sim, 0x73);
		wait(&sim, 16);
	}
	CHECK(sim.wire.hostNybble < 0 && (reply & 0xF0) != 0x70 && sim.wire.phase == UDS_WIRE_SYNC,
	      "a 3DS answering in another range is not taken for the sync's partner (reply %02X)", reply);
}

static void testMenu(void) {
	struct Sim sim;
	uint8_t r[4];
	bringToMenu(&sim);
	CHECK(sim.wire.phase == UDS_WIRE_MENU, "in the menu phase");
	unsigned base = sim.host.sentCount;
	// The 3DS's menu units, as recorded from its side: FE sel FE per call.
	hostPush(&sim.host, 0xFE);
	hostPush(&sim.host, 0xD0);
	hostPush(&sim.host, 0xFE);
	menuCycle(&sim, 0xD0, r);
	CHECK(sim.host.sentCount == base + 3, "three units per cycle, for the three selection bytes and not the stale 00 (%u)", sim.host.sentCount - base);
	CHECK(sim.host.sent[base] == 0xD0 && sim.host.sent[base + 1] == 0xD0 && sim.host.sent[base + 2] == 0xD0, "they are the selection");
	CHECK(r[0] == 0xD0 && r[1] == 0xD0 && r[2] == 0xD0 && r[3] == 0xD0, "the cartridge is told the 3DS's selection, D0 (%02X %02X %02X %02X)", r[0], r[1], r[2], r[3]);
	hostPush(&sim.host, 0xFE);
	hostPush(&sim.host, 0xD0);
	hostPush(&sim.host, 0xFE);
	menuCycle(&sim, 0xD0, r);
	CHECK(sim.host.sentCount == base + 6, "and again");
	CHECK(sim.host.head == sim.host.tail, "each unit of ours consumed one of the 3DS's (the queue is empty)");

	// The 3DS moves its cursor (D1) and then presses A (D4): the cartridge is told, and does not press.
	hostPush(&sim.host, 0xFE);
	hostPush(&sim.host, 0xD1);
	hostPush(&sim.host, 0xFE);
	menuCycle(&sim, 0xD0, r);
	CHECK(sim.wire.menuHost == 0xD1 && !sim.wire.hostPress, "the 3DS's cursor move is passed on (%02X)", sim.wire.menuHost);
	hostPush(&sim.host, 0xFE);
	hostPush(&sim.host, 0xD4);
	hostPush(&sim.host, 0xFE);
	menuCycle(&sim, 0xD0, r);
	CHECK(sim.wire.menuHost == 0xD4 && sim.wire.hostPress && !sim.wire.cartPress, "the 3DS pressed A first (%02X)", sim.wire.menuHost);
	hostPush(&sim.host, 0xFE);
	hostPush(&sim.host, 0xD4);
	hostPush(&sim.host, 0xFE);
	menuCycle(&sim, 0xD0, r);
	CHECK(r[1] == 0xD4 && r[2] == 0xD4, "the cartridge's kept replies carry the press (%02X %02X)", r[1], r[2]);

	// The cartridge leaves: its final stale 00, then it is quiet. The 00 goes out as the final transfer, and the echo is armed.
	unsigned before = sim.host.sentCount;
	exchange(&sim, 0x00);
	CHECK(sim.host.sentCount == before, "a trailing 00 is held");
	hostPush(&sim.host, 0xFE); // the partner of the final 00
	wait(&sim, UDS_WIRE_GAP_MS + 50);
	CHECK(sim.wire.phase == UDS_WIRE_IDLE, "a quiet gap ends the menu");
	CHECK(sim.wire.readDebt == 0 && sim.wire.cable.echoActive, "the debt is paid and the echo armed");
	CHECK(sim.host.sentCount >= before + 1 && sim.host.sent[before] == 0x00, "the final 00 is sent (%u units, first %02X)", sim.host.sentCount - before, sim.host.sent[before]);
	// The 3DS's menu loop keeps exchanging: its units are answered with its own choice.
	unsigned afterGap = sim.host.sentCount;
	hostPush(&sim.host, 0xFE);
	hostPush(&sim.host, 0xFE);
	wait(&sim, 20);
	CHECK(sim.host.sentCount == afterGap + 2 && sim.host.sent[afterGap] == 0xD4 && sim.host.sent[afterGap + 1] == 0xD4,
	      "the 3DS's further menu units are answered with D4 (%u more)", sim.host.sentCount - afterGap);
	wait(&sim, UDS_CABLE_ECHO_QUIET_MS + 100);
	CHECK(!sim.wire.cable.echoActive, "and the echo stops when the 3DS is quiet");

	// The cartridge presses first: no echo.
	bringToMenu(&sim);
	hostPushN(&sim.host, 0xD0, 20);
	menuCycle(&sim, 0xD0, r);
	menuCycle(&sim, 0xD4, r);
	CHECK(sim.wire.cartPress && !sim.wire.hostPress, "the cartridge pressed first");
	exchange(&sim, 0x00);
	wait(&sim, UDS_WIRE_GAP_MS + 50);
	CHECK(sim.wire.phase == UDS_WIRE_IDLE && !sim.wire.cable.echoActive, "so there is no echo");

	// A 3DS that is slow does not let us run ahead of it: the send window is small.
	bringToMenu(&sim);
	base = sim.host.sentCount;
	unsigned i;
	for (i = 0; i < 10; ++i) {
		menuCycle(&sim, 0xD0, r);
	}
	CHECK(sim.host.sentCount - base == UDS_WIRE_MENU_AHEAD, "with nothing from the 3DS only %u units go out ahead of it (%u)", UDS_WIRE_MENU_AHEAD,
	      sim.host.sentCount - base);
	// The debt is paid as the 3DS's units arrive, and sending resumes.
	hostPushN(&sim.host, 0xFE, UDS_WIRE_MENU_AHEAD);
	wait(&sim, 5);
	CHECK(sim.wire.readDebt == 0 && sim.host.head == sim.host.tail, "late units from the 3DS pay the debt (%u owed)", sim.wire.readDebt);
	menuCycle(&sim, 0xD0, r);
	CHECK(sim.host.sentCount - base == UDS_WIRE_MENU_AHEAD + 3, "and the menu sends again");
}

static void testPass(void) {
	struct Sim sim;
	bringToSyncDone(&sim, 0x00);
	exchange(&sim, 0xFD); // leaves the sync for raw exchange; the stale 00 goes out
	CHECK(sim.wire.phase == UDS_WIRE_PASS, "in the pass-through phase");
	CHECK(sim.wire.readDebt == 1, "the stale unit's partner is owed (%u)", sim.wire.readDebt);
	sim.host.head = sim.host.tail;
	hostPush(&sim.host, 0xE0);
	wait(&sim, 5);
	CHECK(sim.wire.readDebt == 0 && sim.host.head == sim.host.tail, "and read and dropped when the 3DS's unit arrives, not shown to the cartridge");
	// Host units are there: each exchange takes one and sends one.
	hostPush(&sim.host, 0xA1);
	hostPush(&sim.host, 0xA2);
	unsigned base = sim.host.sentCount;
	uint8_t reply = exchange(&sim, 0x11);
	CHECK(reply == 0xA1 && sim.host.sentCount == base + 1 && sim.host.sent[base] == 0x11, "a unit from the 3DS is the reply and the master's byte goes out (%02X)", reply);
	reply = exchange(&sim, 0x22);
	CHECK(reply == 0xA2 && sim.host.sentCount == base + 2 && sim.host.sent[base + 1] == 0x22, "and the next");
	CHECK(sim.host.head == sim.host.tail, "both of the 3DS's units were consumed");

	// Nothing from the 3DS: FE. The first exchange sends its unit; the cartridge's retries (the same byte after FE) are not new
	// exchanges and send nothing, so the 3DS is not fed padding that would move our block away from where its own begins.
	sim.wire.pending = 0;
	sim.wire.outstanding = false;
	unsigned base2 = sim.host.sentCount;
	bool allFe = true;
	unsigned i;
	for (i = 0; i < UDS_WIRE_PENDING_MAX + 4; ++i) {
		allFe = allFe && exchange(&sim, 0x33) == 0xFE;
	}
	CHECK(allFe, "no unit from the 3DS: every reply is FE");
	CHECK(sim.host.sentCount == base2 + 1, "and one unit goes out, not one per retry (%u)", sim.host.sentCount - base2);
	exchange(&sim, 0x44);
	CHECK(sim.host.sentCount == base2 + 2 && sim.host.sent[base2 + 1] == 0x44, "a different byte after FE is a new exchange and is sent");
	exchange(&sim, 0xFD);
	exchange(&sim, 0xFD);
	CHECK(sim.host.sentCount == base2 + 3, "an fd repeated while waiting is sent once");

	// The 3DS's units arrive: they are read into the buffer and delivered in order.
	hostPush(&sim.host, 0xB1);
	hostPush(&sim.host, 0xB2);
	hostPush(&sim.host, 0xB3);
	reply = exchange(&sim, 0x33);
	CHECK(reply == 0xB1 && sim.host.head == sim.host.tail && sim.wire.rxCount == 2 && sim.host.sentCount == base2 + 4,
	      "when the 3DS's units arrive they are read at once, the first is the reply and the exchange is sent (%02X)", reply);
	reply = exchange(&sim, 0x33);
	CHECK(reply == 0xB2, "in order");
	reply = exchange(&sim, 0x33);
	CHECK(reply == 0xB3, "all of them");
	reply = exchange(&sim, 0x33);
	CHECK(reply == 0xFE && sim.wire.rxCount == 0, "and FE again when the buffer is empty");

	// An FE that is the 3DS's own unit is delivered like any other.
	hostPush(&sim.host, 0xFE);
	hostPush(&sim.host, 0xD1);
	reply = exchange(&sim, 0x55);
	CHECK(reply == 0xFE && sim.wire.rxCount == 1, "an FE unit from the 3DS is delivered as what it is");
	reply = exchange(&sim, 0x55);
	CHECK(reply == 0xD1, "and the next follows it");

	// A quiet gap ends the pass-through; the first byte after it is the one stranded by the lag, and a 6x after that is a sync.
	wait(&sim, UDS_WIRE_GAP_MS + 50);
	CHECK(sim.wire.phase == UDS_WIRE_IDLE && sim.wire.strandedPending, "a quiet gap ends the pass-through");
	unsigned beforeStale = sim.host.sentCount;
	reply = exchange(&sim, 0x00);
	CHECK(reply == 0xFE && sim.host.sentCount == beforeStale + 1 && sim.host.sent[beforeStale] == 0x00, "the stranded byte is sent as a unit");
	exchange(&sim, 0x00);
	CHECK(sim.host.sentCount == beforeStale + 1, "later stale bytes are not");
	exchange(&sim, 0x60);
	CHECK(sim.wire.phase == UDS_WIRE_SYNC && sim.wire.stale == 0x00, "and a 6x starts the next sync");

	// The session drops in the middle: an idle line, nothing sent.
	sim.host.ready = false;
	tick(&sim);
	unsigned sentBefore = sim.host.sentCount;
	reply = exchange(&sim, 0x66);
	CHECK(reply == 0xFF && sim.host.sentCount == sentBefore, "a dropped session: FF and no units");
}

static void bringToPassClean(struct Sim* sim) {
	bringToSyncDone(sim, 0x00);
	exchange(sim, 0xFD);
	sim->host.head = sim->host.tail;
	sim->wire.readDebt = 0;
}

// In a block the cartridge stores whatever it is answered, FE included, so once a block has started the replies must not run dry:
// the first fd is held until the whole block is queued.
static void testBlocks(struct Sim* simp) {
	struct Sim sim;
	(void) simp;
	bringToPassClean(&sim);
	CHECK(sim.wire.phase == UDS_WIRE_PASS && !sim.wire.blockStoring && sim.wire.blockIndex == 0, "a pass-through starts waiting for the first block");

	// The 3DS's leftovers from its sync, which the cartridge ignores, and the start of its random-number block, not all there yet.
	hostPush(&sim.host, 0x60);
	hostPushN(&sim.host, 0x00, 3);
	hostPushN(&sim.host, 0xFD, 8);
	unsigned i;
	for (i = 0; i < 5; ++i) {
		hostPush(&sim.host, 0x20 + i);
	}
	uint8_t reply;
	for (i = 0; i < 4; ++i) {
		reply = exchange(&sim, 0xFD);
		CHECK(reply == (i == 0 ? 0x60 : 0x00), "leftover units are delivered as they come (%02X)", reply);
	}
	reply = exchange(&sim, 0xFD);
	CHECK(reply == 0xFE && !sim.wire.blockStoring, "with only part of the block queued the first fd is held (%02X)", reply);
	reply = exchange(&sim, 0xFD);
	CHECK(reply == 0xFE && sim.wire.rxCount == 13, "and stays held, the units buffered meanwhile (%u)", sim.wire.rxCount);
	for (i = 5; i < 10; ++i) {
		hostPush(&sim.host, 0x20 + i);
	}
	reply = exchange(&sim, 0xFD);
	CHECK(reply == 0xFD && sim.wire.blockStoring && sim.wire.blockRemaining == 17, "with the whole block queued the fd is delivered and the block opens (%02X, %u left)", reply,
	      sim.wire.blockRemaining);

	// Seventeen replies follow, none of them FE, even though the 3DS has nothing more to send after them. The block is complete
	// with the seventeenth and not before.
	bool noFe = true;
	for (i = 0; i < 17; ++i) {
		CHECK(sim.wire.blockStoring, "still storing before reply %u of 17", i + 1);
		reply = exchange(&sim, 0xAA);
		noFe = noFe && reply != 0xFE;
	}
	CHECK(noFe, "every reply inside the block comes from the queue");
	CHECK(!sim.wire.blockStoring && sim.wire.blockIndex == 1 && sim.wire.blockUnderruns == 0, "the block is complete and the next one is waited for (%u)", sim.wire.blockIndex);
	CHECK(sim.host.head == sim.host.tail, "everything the 3DS sent was read");

	// If the 3DS's unit is missing inside a block the cartridge gets an FE as data; the stream does not shift.
	sim.wire.blockStoring = true;
	sim.wire.blockRemaining = 4;
	unsigned sentBefore = sim.host.sentCount;
	reply = exchange(&sim, 0xB1);
	CHECK(reply == 0xFE && sim.wire.blockUnderruns == 1 && sim.host.sentCount == sentBefore + 1 && sim.wire.blockSkip == 1, "an empty buffer inside a block: FE as data, counted, the unit goes out and the 3DS's next unit will be dropped");
	hostPush(&sim.host, 0x77); // the unit that FE stood in for, then the one after it
	hostPush(&sim.host, 0x78);
	reply = exchange(&sim, 0xB2);
	CHECK(reply == 0x78 && sim.host.head == sim.host.tail && sim.wire.blockSkip == 0,
	      "the late unit is the owed partner and is dropped, not shifted into the block; the next is delivered in place (%02X)", reply);
}

// The 3DS's player block is one unit short of what the cartridge stores: the last byte is the first unit of the next block.
static void testLastByte(void) {
	struct Sim sim;
	bringToPassClean(&sim);
	sim.wire.blockIndex = 1;
	hostPush(&sim.host, 0xFD);
	unsigned i;
	for (i = 0; i < 423; ++i) {
		hostPush(&sim.host, i < 5 ? 0xFD : (uint8_t) (0x20 + i % 0x80));
	}
	uint8_t reply = exchange(&sim, 0xFD);
	CHECK(reply == 0xFD && sim.wire.blockStoring && sim.wire.blockRemaining == 424, "the block opens with exactly its own units queued (%u left)", sim.wire.blockRemaining);
	bool noFe = true;
	for (i = 0; i < 423; ++i) {
		reply = exchange(&sim, 0xA0);
		noFe = noFe && reply != 0xFE;
	}
	CHECK(noFe && sim.wire.blockStoring && sim.wire.blockRemaining == 1, "423 replies come from the queue and one is left");
	reply = exchange(&sim, 0xA0);
	CHECK(reply == 0xFD && !sim.wire.blockStoring && sim.wire.blockIndex == 2 && sim.wire.blockSkip == 1 && sim.wire.blockUnderruns == 0,
	      "the last byte is filled in with an fd, not FE, and is not counted as damage (%02X)", reply);
	// The real unit arrives, then the next block's fd: the first is dropped, the second is held for its block.
	hostPush(&sim.host, 0xFD);
	hostPush(&sim.host, 0xFD);
	reply = exchange(&sim, 0xA0);
	CHECK(reply == 0xFE && sim.wire.blockSkip == 0 && sim.wire.rxCount == 1, "the unit that was stood in for is dropped, and the next block's fd is held (%u buffered)", sim.wire.rxCount);
}

// The whole block exchange against a 3DS that only answers units with units, and a cartridge that runs Serial_ExchangeBytes:
// ignore replies until an fd, then store the next `size` replies, whatever they are, sending its own block meanwhile.
static void testBlockLoop(void) {
	struct Sim sim;
	bringToPassClean(&sim);
	// The 3DS's stream: its sync leftovers, then the three blocks as the recorded trades show them (fd run, then data).
	static const unsigned kSizes[3] = {17, 424, 200};
	static const unsigned kLeadFd[3] = {7, 5, 2}; // fd the cartridge stores before the data (the run is one longer: the first ends the ignoring)
	unsigned n = 0, b, i;
	// The 3DS creates its random-number and player blocks at once (bulk, as soon as it has our first unit) but produces what comes
	// after them, the patch lists' preamble, only once it has received our whole player block, as in the live run.
	static const unsigned kLate = 470; // units of ours by then: past what the cartridge has sent when it needs the next unit
	// Its exchanges are paced by ours until the player block: each unit up to the end of the random-number list is released by
	// one more unit of ours (as in the live run that stalled with the cartridge waiting for the list to be buffered).
	unsigned base = sim.host.sentCount - 1; // the exchange that entered the pass has sent its unit: the first of theirs exists
	sim.host.script[n] = 0x60;
	sim.host.releaseAt[n] = base + 1 + n;
	++n;
	for (i = 0; i < 4; ++i) {
		sim.host.script[n] = 0x00;
		sim.host.releaseAt[n] = base + 1 + n;
		++n;
	}
	uint8_t expected[3][424];
	for (b = 0; b < 3; ++b) {
		sim.host.script[n] = 0xFD; // the one that ends the cartridge's ignoring
		sim.host.releaseAt[n] = b == 0 ? base + 1 + n : b == 2 ? kLate : base + 1 + 5 + UDS_WIRE_RN_POSITIONS;
		++n;
		// The cartridge stores `size` replies after it. The 3DS's player block is one unit short of that: the last one stored is the
		// first fd of the patch lists' preamble.
		unsigned own = b == 1 ? kSizes[b] - 1 : kSizes[b];
		for (i = 0; i < own; ++i) {
			uint8_t v = i < kLeadFd[b] ? 0xFD : (uint8_t) (0x10 + (i * 7 + b) % 0xB0);
			sim.host.script[n] = v;
			sim.host.releaseAt[n] = b == 0 ? base + 1 + n : b == 2 ? kLate : base + 1 + 5 + UDS_WIRE_RN_POSITIONS;
			++n;
			expected[b][i] = v;
		}
		if (b == 1) {
			expected[b][kSizes[b] - 1] = 0xFD; // the first fd of the next preamble
			sim.host.script[n] = 0xFD;
			sim.host.releaseAt[n++] = kLate;
		}
	}
	sim.host.scriptLen = n;
	sim.host.scriptPos = 0;
	sim.host.reactive = true;

	uint8_t stored[3][424];
	unsigned storedCount[3] = {0, 0, 0};
	unsigned block = 0, exchanges = 0;
	bool storing = false;
	while (block < 3 && exchanges < 20000) {
		// The cartridge sends fd while it ignores replies, and its block's bytes while it stores them.
		uint8_t send = storing ? (uint8_t) (0xA0 + storedCount[block]) : 0xFD;
		uint8_t reply = exchange(&sim, send);
		++exchanges;
		if (!storing) {
			if (reply == 0xFD) {
				storing = true;
			}
		} else {
			stored[block][storedCount[block]++] = reply;
			if (storedCount[block] == kSizes[block]) {
				++block;
				storing = false;
			}
		}
	}
	CHECK(block == 3, "all three blocks complete against a 3DS that only answers units (%u blocks, %u exchanges)", block, exchanges);
	bool intact = true;
	for (b = 0; b < 3 && b < block; ++b) {
		intact = intact && storedCount[b] == kSizes[b] && !memcmp(stored[b], expected[b], kSizes[b]);
	}
	CHECK(intact, "and what the cartridge stored is what the 3DS sent, byte for byte");
	CHECK(sim.wire.blockUnderruns == 0, "with no FE stored inside a block");
	// The 3DS received every byte of the cartridge's blocks, in order.
	CHECK(sim.host.sentCount >= 17 + 424 + 200, "and received the cartridge's blocks (%u units)", sim.host.sentCount);
	// The 3DS stores 424 bytes after the first fd it receives, so the fd run in front of the player and patch blocks must stay short
	// (the test cartridge sends no preamble of its own: the run is the waiting fd plus the one that opened the block).
	unsigned run = 0, runs = 0, runLen[8] = {0};
	for (i = 0; i < sim.host.sentCount; ++i) {
		if (sim.host.sent[i] == 0xFD) {
			++run;
		} else {
			if (run && runs < 8 && sim.host.sent[i] == 0xA0) {
				runLen[runs++] = run;
			}
			run = 0;
		}
	}
	// The random-number list is covered by fd and the cartridge's own units for it dropped, so the player and patch blocks are the
	// two runs that end in the test cartridge's first data byte.
	CHECK(runs == 2 && runLen[0] <= 2 && runLen[1] <= 2, "and sent few fd in front of the player and patch blocks (%u runs: %u, %u)", runs, runLen[0], runLen[1]);
}

// Gen 2's Link_EnsureSync (the room confirm, $D0+room, and the Time Capsule's $D4) ends as soon as a reply is in the D range, so the
// cartridge must not be handed an assumed D0 while the 3DS has not selected anything: it is told FE and keeps asking until a real selection
// of the 3DS's arrives (a Gen 2 game in a Time Capsule trade walked on at once and left a Gen 1 game stuck at its menu).
static void testGen2Menu(void) {
	struct Sim sim;
	bringToSyncDone(&sim, 0x00);
	sim.host.head = sim.host.tail;
	sim.wire.readDebt = 0;
	udsWireSetGeneration(&sim.wire, 2);
	unsigned i;
	bool allFe = true;
	exchange(&sim, 0xD4);
	CHECK(sim.wire.phase == UDS_WIRE_MENU && !sim.wire.menuHostKnown, "a Dx byte starts the menu with no selection of the 3DS's known");
	for (i = 0; i < 12; ++i) {
		allFe = allFe && exchange(&sim, 0xD4) == 0xFE;
		wait(&sim, 16);
	}
	CHECK(allFe, "the Gen 2 cartridge is told FE while the 3DS has selected nothing");
	hostPush(&sim.host, 0xFE);
	hostPush(&sim.host, 0xD0);
	hostPush(&sim.host, 0xFE);
	hostPush(&sim.host, 0xFE);
	hostPush(&sim.host, 0xD4);
	hostPush(&sim.host, 0xFE);
	uint8_t reply = 0xFE;
	for (i = 0; i < 30 && reply == 0xFE; ++i) {
		reply = exchange(&sim, 0xD4);
		wait(&sim, 16);
	}
	CHECK(sim.wire.menuHostKnown && (reply & 0xF0) == 0xD0, "and when a selection of the 3DS's is read it is passed on (%02X)", reply);

	// Gen 1 is unchanged: the menu answers D0 from the start.
	bringToSyncDone(&sim, 0x00);
	exchange(&sim, 0xD0);
	CHECK(exchange(&sim, 0xD0) == 0xD0, "in Gen 1 the cartridge is told D0 at once");
}

// Gen 2's fourth block, the Trade Center's mail: a plain ExchangeBytes, so the first reply of any value ends the ignoring and every reply after
// it is stored, FE included. Every exchange is a real one (none is a retry) and must send its unit, or the 3DS's own mail exchange is left
// short of units and never gets to its trade menu (a live run lost 29 and stalled both games); a reply that is not there yet is a stand-in
// 00 and the real unit is dropped when it comes.
static void testMail(void) {
	struct Sim sim;
	bringToPassClean(&sim);
	udsWireSetGeneration(&sim.wire, 2);
	sim.wire.mailMode = true;
	unsigned base = sim.host.sentCount, i;
	uint8_t unit[391];
	for (i = 0; i < 11; ++i) {
		hostAt(&sim.host, sim.now + 400, 0x00); // what is left of the 3DS's patch block ahead of its mail: not mail
	}
	for (i = 0; i < 391; ++i) {
		unit[i] = i < 5 ? 0x20 : (uint8_t) (0x30 + i % 0x40);
		hostAt(&sim.host, sim.now + 400, unit[i]);
	}
	uint8_t stored[390];
	unsigned exchanges = 0, storedCount = 0;
	bool ignoring = true;
	while (storedCount < 390 && exchanges < 5000) {
		uint8_t reply = exchange(&sim, 0x00);
		wait(&sim, 16);
		++exchanges;
		if (ignoring) {
			ignoring = false; // the first reply, whatever it is
		} else {
			stored[storedCount++] = reply;
		}
	}
	CHECK(storedCount == 390 && exchanges == 391, "the cartridge stores 390 replies after the first (%u stored, %u exchanges)", storedCount, exchanges);
	CHECK(sim.host.sentCount - base == exchanges, "and every exchange sent its unit (%u units for %u exchanges)", sim.host.sentCount - base, exchanges);
	bool noFe = true;
	unsigned lead = 0;
	while (lead < 390 && stored[lead] == 0x20) {
		++lead;
	}
	// The receiver scans for the first $20 and skips the run: the late replies are $20, so the run is longer than five and the mail data
	// follows it whole (the head of the first message intact, the tail cut by the replies that were late).
	for (i = 0; i < 390; ++i) {
		noFe = noFe && stored[i] != 0xFE;
	}
	CHECK(noFe, "with no FE stored as data");
	CHECK(lead > 5 && lead < 60, "the replies that came before the 3DS's units were $20 stand-ins (%u)", lead);
	bool aligned = true;
	for (i = lead; i < 390; ++i) {
		aligned = aligned && stored[i] == unit[5 + i - lead];
	}
	CHECK(aligned, "and the 3DS's mail data follows the run from its first byte, in order, nothing dropped");
}

// The first 99 recorded exchanges: role handshake, the first sync (slave late by 39 exchanges), the menu up to the slave's d4.
static void testReplay(void) {
	struct Sim sim;
	simInit(&sim);
	uint8_t hostUnits[128];
	unsigned count = 0, i;
	hostUnits[count++] = 0xEF;
	for (i = 0; i < 4; ++i) {
		hostUnits[count++] = 0x65;
	}
	for (i = 0; i < 13; ++i) {
		hostUnits[count++] = 0x00;
	}
	unsigned syncUnits = count;
	for (i = 0; i < 50; ++i) {
		hostUnits[count++] = 0xD0;
	}
	for (i = 0; i < count; ++i) {
		hostPush(&sim.host, hostUnits[i]);
	}
	tick(&sim);
	int syncAt = -1, menuAt = -1, idleAt = -1;
	unsigned sentAtMenu = 0, dBytes = 0, sentBefore = 0;
	bool classOk = true, hostKnownReplies = true;
	for (i = 0; i < sizeof(kRecordedMaster); ++i) {
		sim.now = kRecordedTimeMs[i] + 1000;
		tick(&sim);
		uint8_t reply = udsWirePreload(&sim.wire);
		if (i < 3 && reply != kRecordedSlave[i]) {
			classOk = false;
		}
		if (sim.wire.phase == UDS_WIRE_SYNC && sim.wire.hostNybble >= 0) {
			if (sim.wire.syncPart != UDS_WIRE_SYNC_DONE) {
				uint8_t want = sim.wire.syncPart == UDS_WIRE_SYNC_LOOPS12 ? 0x65 : (sim.wire.zeros < 10 ? 0x00 : 0xFE);
				hostKnownReplies = hostKnownReplies && reply == want;
			}
		} else if (sim.wire.phase == UDS_WIRE_SYNC) {
			hostKnownReplies = hostKnownReplies && reply == 0xFE;
		}
		sentBefore = sim.host.sentCount;
		udsWireExchanged(&sim.wire, sim.now, kRecordedMaster[i]);
		if (idleAt < 0 && sim.wire.phase == UDS_WIRE_IDLE) {
			idleAt = (int) i;
		}
		if (syncAt < 0 && sim.wire.phase == UDS_WIRE_SYNC) {
			syncAt = (int) i;
		}
		if (menuAt < 0 && sim.wire.phase == UDS_WIRE_MENU) {
			menuAt = (int) i;
			sentAtMenu = sentBefore;
		}
		if (menuAt >= 0 && i >= (unsigned) menuAt && (kRecordedMaster[i] & 0xF0) == 0xD0) {
			++dBytes;
		}
	}
	CHECK(classOk, "the first three replies are the real slave's (02 00 FE)");
	CHECK(idleAt == 0, "connected after the first exchange (%d)", idleAt);
	CHECK(syncAt == 4, "the sync starts at the first 6x, exchange 4 (%d)", syncAt);
	CHECK(menuAt == 67, "it ends and the menu starts at the first d0, exchange 67 (%d)", menuAt);
	CHECK(hostKnownReplies, "during the sync the replies follow the rule: FE, then 65 through loops 1 and 2, 00 through loop 3, then FE");

	uint8_t reference[MAX_UNITS];
	unsigned referenceCount = 0;
	referenceSync(hostUnits, syncUnits, 0, reference, &referenceCount);
	CHECK(referenceCount == sentAtMenu && !memcmp(reference, sim.host.sent, referenceCount),
	      "on the real master's bytes the 3DS gets the same units as its side of the sync produces alone, up to the end of the sync (%u vs %u)",
	      sentAtMenu, referenceCount);
	CHECK(sim.host.sentCount - sentAtMenu == dBytes, "in the menu one unit goes out per Dx byte, the one that ended the sync included (%u for %u)",
	      sim.host.sentCount - sentAtMenu, dBytes);
}

int main(void) {
	testRole();
	testSync();
	testGen2Sync();
	testGen2Menu();
	testMail();
	testMenu();
	testPass();
	testBlocks(NULL);
	testLastByte();
	testBlockLoop();
	testReplay();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
