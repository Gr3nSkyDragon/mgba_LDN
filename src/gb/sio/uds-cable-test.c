/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Test of uds-cable.c with a scripted host and no emulator, no socket and no clock: the transfer pairing, the nybble sync burst
 * and the link menu exchange. Build target: uds-cable-test. Exit status 0 when every check passes.
 */
#include <mgba/internal/gb/sio/uds-cable.h>

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

// A fake Pia stream: what the cable sent, and what the "host" has queued for it.
struct Fake {
	bool ready;
	uint8_t sent[512];
	unsigned sentCount;
	uint8_t host[512];
	unsigned hostHead, hostTail;
	unsigned flushes;
};

static bool fakeReady(void* context) {
	return ((struct Fake*) context)->ready;
}

static bool fakeQueue(void* context, uint8_t byte) {
	struct Fake* fake = context;
	fake->sent[fake->sentCount++] = byte;
	return true;
}

static void fakeFlush(void* context) {
	++((struct Fake*) context)->flushes;
}

static bool fakePop(void* context, uint8_t* byte) {
	struct Fake* fake = context;
	if (fake->hostHead == fake->hostTail) {
		return false;
	}
	*byte = fake->host[fake->hostHead++];
	return true;
}

static bool fakePeek(void* context, uint8_t* byte) {
	struct Fake* fake = context;
	if (fake->hostHead == fake->hostTail) {
		return false;
	}
	*byte = fake->host[fake->hostHead];
	return true;
}

static size_t fakeWaiting(void* context) {
	struct Fake* fake = context;
	return fake->hostTail - fake->hostHead;
}

static void hostSends(struct Fake* fake, const uint8_t* bytes, size_t count) {
	memcpy(&fake->host[fake->hostTail], bytes, count);
	fake->hostTail += count;
}

#define HOST(fake, ...) \
	do { \
		static const uint8_t bytes_[] = {__VA_ARGS__}; \
		hostSends(fake, bytes_, sizeof(bytes_)); \
	} while (0)

static void setup(struct UDSCable* cable, struct Fake* fake) {
	memset(fake, 0, sizeof(*fake));
	fake->ready = true;
	struct UDSUnitPort port = {
		.context = fake,
		.ready = fakeReady,
		.queue = fakeQueue,
		.flush = fakeFlush,
		.pop = fakePop,
		.peek = fakePeek,
		.waiting = fakeWaiting,
		.trace = NULL,
	};
	udsCableInit(cable, &port);
}

static void testTransfer(void) {
	struct UDSCable cable;
	struct Fake fake;
	uint8_t rx = 0;
	setup(&cable, &fake);

	udsCableBegin(&cable);
	CHECK(fake.sentCount == 1 && fake.sent[0] == UDS_CABLE_FIRST_UNIT, "the first unit is sent");
	udsCableTransfer(&cable, 0x55);
	CHECK(fake.sentCount == 2 && fake.sent[1] == 0x55, "a transfer queues its byte");
	udsCableTransfer(&cable, 0x66);
	CHECK(fake.sentCount == 2, "a transfer already waiting is kept, not doubled");

	CHECK(!udsCablePoll(&cable, 0, &rx), "nothing completes before the host's unit");
	HOST(&fake, 0xEF);
	CHECK(!udsCablePoll(&cable, 0, &rx) && !cable.discardFirst, "the host's first unit is discarded, not paired");
	CHECK(!udsCablePoll(&cable, 0, &rx), "still nothing to pair");
	HOST(&fake, 0xA1, 0xA2);
	CHECK(udsCablePoll(&cable, 0, &rx) && rx == 0xA1, "the next unit completes the transfer (got %02X)", rx);
	CHECK(!udsCablePoll(&cable, 0, &rx), "one unit completes one transfer");
	udsCableTransfer(&cable, 0x77);
	CHECK(udsCablePoll(&cable, 0, &rx) && rx == 0xA2, "the following transfer takes the following unit");

	// While a sync or menu exchange runs, transfers are not paired.
	udsCableTransfer(&cable, 0x01);
	cable.busy = true;
	HOST(&fake, 0xB0);
	CHECK(!udsCablePoll(&cable, 0, &rx), "no pairing while an exchange is under way");
	cable.busy = false;
	CHECK(udsCablePoll(&cable, 0, &rx) && rx == 0xB0, "pairing resumes afterwards");

	// The host goes away mid-transfer
	udsCableTransfer(&cable, 0x02);
	CHECK(udsCableLost(&cable), "a lost link reports the waiting transfer");
	CHECK(!udsCableLost(&cable), "and only once");
}

static void testSync(void) {
	struct UDSCable cable;
	struct Fake fake;
	int result = -1;
	setup(&cable, &fake);
	udsCableBegin(&cable);
	HOST(&fake, 0xEF);
	uint8_t rx;
	udsCablePoll(&cable, 0, &rx);
	fake.sentCount = 0;

	// The host is late: two idle units, then its own 6x.
	HOST(&fake, 0xFE, 0xFE, 0x65);
	CHECK(udsCableSync(&cable, 3, 1000, &result) == UDS_CABLE_PENDING, "the sync is not finished after the host's answer");
	CHECK(cable.syncNybble == 5 && cable.syncPhase == UDS_SYNC_AFTER, "the host's nybble is read from its 6x unit (phase %d)", cable.syncPhase);
	CHECK(cable.busy, "transfers are not paired during the sync");
	CHECK(fake.sentCount == 3 + UDS_CABLE_SYNC_SIXTIES + UDS_CABLE_SYNC_ZEROS, "one unit per host unit, then 5 more and 5 zeros (%u)",
	      fake.sentCount);
	bool allSixty = true;
	unsigned i;
	for (i = 0; i < 3 + UDS_CABLE_SYNC_SIXTIES; ++i) {
		allSixty = allSixty && fake.sent[i] == 0x63;
	}
	for (; i < fake.sentCount; ++i) {
		allSixty = allSixty && fake.sent[i] == 0x00;
	}
	CHECK(allSixty, "the units are 60|nybble, then 00");

	// The host's matching ten, then a tail of three sync units, each answered with a 00.
	HOST(&fake, 0x60, 0x60, 0x60, 0x60, 0x60, 0, 0, 0, 0, 0);
	CHECK(udsCableSync(&cable, 3, 1010, &result) == UDS_CABLE_PENDING && cable.syncPhase == UDS_SYNC_TAIL, "the tail starts");
	unsigned before = fake.sentCount;
	HOST(&fake, 0x00, 0x00, 0x61);
	CHECK(udsCableSync(&cable, 3, 1020, &result) == UDS_CABLE_PENDING, "the host may still be sending");
	CHECK(fake.sentCount == before + 3 && cable.syncTail == 3, "each tail unit is answered with one 00 (%u)", fake.sentCount - before);
	CHECK(udsCableSync(&cable, 3, 1020 + UDS_CABLE_SYNC_TAIL_QUIET_MS + 1, &result) == UDS_CABLE_DONE && result == 5,
	      "a quiet host ends the sync with its nybble (%d)", result);
	CHECK(!cable.busy && cable.syncPhase == UDS_SYNC_IDLE, "the cable is free again");

	// The host moves on to something else: a unit that is not a sync unit ends the tail at once.
	setup(&cable, &fake);
	udsCableBegin(&cable);
	cable.discardFirst = false;
	HOST(&fake, 0x60);
	udsCableSync(&cable, 7, 0, &result);
	HOST(&fake, 0x60, 0x60, 0x60, 0x60, 0x60, 0, 0, 0, 0, 0, 0xD4);
	CHECK(udsCableSync(&cable, 7, 10, &result) == UDS_CABLE_DONE && result == 0, "a unit that is not 00 or 6x ends the tail (%d)", result);
	uint8_t next = 0;
	CHECK(fakePeek(&fake, &next) && next == 0xD4, "and is left for whoever reads it next");

	// Nobody answers
	setup(&cable, &fake);
	udsCableBegin(&cable);
	cable.discardFirst = false;
	CHECK(udsCableSync(&cable, 1, 0, &result) == UDS_CABLE_PENDING, "waiting");
	CHECK(udsCableSync(&cable, 1, UDS_CABLE_SYNC_TIMEOUT_MS, &result) == UDS_CABLE_PENDING, "not yet timed out");
	CHECK(udsCableSync(&cable, 1, UDS_CABLE_SYNC_TIMEOUT_MS + 1, &result) == UDS_CABLE_TIMED_OUT && result == 0xFF,
	      "a sync nobody answers times out with FF");
	CHECK(!cable.busy, "and frees the cable");

	// The link is not up yet: the call waits and sends nothing.
	setup(&cable, &fake);
	fake.ready = false;
	CHECK(udsCableSync(&cable, 1, 0, &result) == UDS_CABLE_PENDING && fake.sentCount == 0, "nothing is sent before the link is up");
}

static void testMenu(void) {
	struct UDSCable cable;
	struct Fake fake;
	uint8_t first = 0, second = 0;
	setup(&cable, &fake);
	udsCableBegin(&cable);
	cable.discardFirst = false;
	fake.sentCount = 0;

	CHECK(udsCableMenu(&cable, 0xD4, 0, &first, &second) == UDS_CABLE_PENDING, "the menu waits for the host");
	CHECK(fake.sentCount == 3 && fake.sent[0] == 0xD4 && fake.sent[1] == 0xD4 && fake.sent[2] == 0xD4, "the selection is sent three times");
	CHECK(cable.busy, "transfers are not paired during the menu call");
	HOST(&fake, 0xFE, 0xD0);
	CHECK(udsCableMenu(&cable, 0xD4, 1, &first, &second) == UDS_CABLE_PENDING, "two of three bytes are not enough");
	CHECK(fake.sentCount == 3, "the selection is sent once per call");
	HOST(&fake, 0xD1);
	CHECK(udsCableMenu(&cable, 0xD4, 2, &first, &second) == UDS_CABLE_DONE && first == 0xD1 && second == 0xD1,
	      "the last D0-class byte goes in both slots (%02X %02X)", first, second);
	CHECK(!cable.busy && !cable.menuActive, "the cable is free again");

	// No D0-class byte at all: the ROM gets the second and third bytes as they came.
	setup(&cable, &fake);
	udsCableBegin(&cable);
	cable.discardFirst = false;
	HOST(&fake, 0xFE, 0x12, 0x34);
	CHECK(udsCableMenu(&cable, 0xD0, 0, &first, &second) == UDS_CABLE_DONE && first == 0x12 && second == 0x34, "without a D0-class byte: %02X %02X",
	      first, second);

	// The host pressed A first and we did not: its further units are answered with its choice until it goes quiet.
	setup(&cable, &fake);
	udsCableBegin(&cable);
	cable.discardFirst = false;
	HOST(&fake, 0xFE, 0xFE, 0xD4);
	CHECK(udsCableMenu(&cable, 0xD0, 100, &first, &second) == UDS_CABLE_DONE && first == 0xD4 && second == 0xD4, "host-first: adopted");
	CHECK(cable.echoActive && cable.echoByte == 0xD4, "the echo is armed with the host's choice");
	unsigned sentBefore = fake.sentCount;
	uint8_t rx;
	HOST(&fake, 0xFE, 0xFE);
	CHECK(!udsCablePoll(&cable, 150, &rx), "an echo is not a completed transfer");
	CHECK(fake.sentCount == sentBefore + 2 && fake.sent[sentBefore] == 0xD4 && fake.sent[sentBefore + 1] == 0xD4 && cable.echoCount == 2,
	      "each further host unit is answered with D4 (%u)", fake.sentCount - sentBefore);
	CHECK(cable.echoActive, "still listening while the host is active");
	HOST(&fake, 0xFE);
	udsCablePoll(&cable, 300, &rx);
	CHECK(fake.sentCount == sentBefore + 3 && cable.echoCount == 3, "a late unit is answered too");
	udsCablePoll(&cable, 300 + UDS_CABLE_ECHO_QUIET_MS + 1, &rx);
	CHECK(!cable.echoActive, "the echo ends when the host has been quiet");
	HOST(&fake, 0x60);
	udsCablePoll(&cable, 2000, &rx);
	CHECK(fake.sentCount == sentBefore + 3, "nothing is echoed afterwards");

	// A waiting transfer of the Game Boy's own takes the host's next unit first.
	setup(&cable, &fake);
	udsCableBegin(&cable);
	cable.discardFirst = false;
	HOST(&fake, 0xFE, 0xFE, 0xD4);
	udsCableMenu(&cable, 0xD0, 0, &first, &second);
	udsCableTransfer(&cable, 0x00);
	sentBefore = fake.sentCount;
	HOST(&fake, 0xFE, 0xFE);
	CHECK(udsCablePoll(&cable, 10, &rx) && rx == 0xFE, "the Game Boy's own transfer completes first");
	CHECK(fake.sentCount == sentBefore + 1 && cable.echoCount == 1, "and the host's next unit is then echoed (%u)", fake.sentCount - sentBefore);

	// We pressed first, or the host did not press: no echo.
	setup(&cable, &fake);
	udsCableBegin(&cable);
	cable.discardFirst = false;
	HOST(&fake, 0xFE, 0xD4, 0xD4);
	udsCableMenu(&cable, 0xD4, 0, &first, &second);
	CHECK(!cable.echoActive, "we pressed first: no echo");
	setup(&cable, &fake);
	udsCableBegin(&cable);
	cable.discardFirst = false;
	HOST(&fake, 0xFE, 0xD0, 0xD0);
	udsCableMenu(&cable, 0xD0, 0, &first, &second);
	CHECK(!cable.echoActive, "nobody pressed: no echo");

	// The link drops after the selection went out.
	setup(&cable, &fake);
	udsCableBegin(&cable);
	cable.discardFirst = false;
	udsCableMenu(&cable, 0xD0, 0, &first, &second);
	fake.ready = false;
	CHECK(udsCableMenu(&cable, 0xD0, 1, &first, &second) == UDS_CABLE_DONE && first == 0xFF && second == 0xFF,
	      "a dropped link gives the Game Boy an idle line");
}

int main(void) {
	testTransfer();
	testSync();
	testMenu();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
