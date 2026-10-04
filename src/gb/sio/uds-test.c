/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Unit test for the UDS wrapper's Pia codec and joiner session (uds-pia.c, uds-session.c), run against real frames from
 * the retail-to-retail capture of 2026-10-04 (uds-test-vectors.h). It needs no radio and no game. Build target: uds-test.
 * Exit status 0 when every check passes.
 */
#include <mgba/internal/gb/sio/uds-pia.h>
#include <mgba/internal/gb/sio/uds-session.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uds-test-vectors.h"

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

#define VECTOR_COUNT (sizeof(vectors) / sizeof(vectors[0]))

// One message pulled out of a frame, copied so it outlives the frame buffer.
struct Sig {
	uint8_t sender;
	uint32_t destination;
	uint8_t protocol;
	uint8_t subtype;
	uint8_t reliable;
	uint16_t length;
	uint8_t payload[160];
	uint8_t connectionId;
	uint16_t packetId;
};

static int sigsOf(const uint8_t* frame, size_t size, struct Sig* out, int max) {
	struct UDSFrame f;
	if (!udsFrameParse(frame, size, &f) || f.kind != UDS_FRAME_PIA) {
		return 0;
	}
	int n = 0;
	size_t pos = 0;
	struct UDSMessage m;
	while (n < max && udsMessageNext(&f, &pos, &m)) {
		out[n].sender = m.sender;
		out[n].destination = m.destination;
		out[n].protocol = m.protocol;
		out[n].subtype = m.subtype;
		out[n].reliable = m.reliable;
		out[n].length = m.length;
		memset(out[n].payload, 0, sizeof(out[n].payload));
		memcpy(out[n].payload, m.payload, m.length < sizeof(out[n].payload) ? m.length : sizeof(out[n].payload));
		out[n].connectionId = f.pia.connectionId;
		out[n].packetId = f.pia.packetId;
		++n;
	}
	return n;
}

// Everything the session sends is recorded here.
struct Sent {
	uint8_t frame[64][UDS_MAX_FRAME_SIZE];
	size_t size[64];
	uint32_t ms[64];
	int count;
};

static struct Sent sent;
static uint32_t nowMs;

static void onSend(void* context, const uint8_t* frame, size_t size) {
	(void) context;
	if (sent.count < 64) {
		memcpy(sent.frame[sent.count], frame, size);
		sent.size[sent.count] = size;
		sent.ms[sent.count] = nowMs;
		++sent.count;
	}
}

static uint32_t be32(const uint8_t* p) {
	return ((uint32_t) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

// Codec

static void testCodec(void) {
	size_t i;
	for (i = 0; i < VECTOR_COUNT; ++i) {
		struct UDSFrame f;
		CHECK(udsFrameParse(vectors[i].data, vectors[i].size, &f), "%s: parse", vectors[i].name);
		CHECK(f.crcOk, "%s: header CRC", vectors[i].name);
		if (f.kind != UDS_FRAME_PIA) {
			continue;
		}
		CHECK(f.tailOk, "%s: HMAC tail", vectors[i].name);
		// Rebuilding the frame from its parts has to give back the same bytes.
		uint8_t out[UDS_MAX_FRAME_SIZE];
		size_t pos = udsFrameBegin(out, &f.pia);
		size_t at = 0;
		struct UDSMessage m;
		int messages = 0;
		while (udsMessageNext(&f, &at, &m)) {
			pos = udsFrameAppend(out, sizeof(out), pos, &m);
			CHECK(pos, "%s: append", vectors[i].name);
			++messages;
		}
		size_t size = pos ? udsFrameEnd(out, sizeof(out), pos) : 0;
		CHECK(messages > 0, "%s: no messages", vectors[i].name);
		CHECK(size == vectors[i].size && !memcmp(out, vectors[i].data, size), "%s: rebuilt frame differs (%zu vs %zu)",
		      vectors[i].name, size, vectors[i].size);
	}
	uint8_t reply[UDS_HELLO_REPLY_SIZE];
	CHECK(udsBuildHelloReply(reply) == vectors[0].size && !memcmp(reply, vectors[0].data, sizeof(reply)),
	      "hello reply differs from the capture");
	// The hello and bye frames come from earlier Azahar captures (docs/wiki/vc_link.md).
	uint8_t hello[UDS_HELLO_SIZE];
	udsBuildHello(hello, 0x79CD0B19);
	static const uint8_t expectHello[24] = { 0x01, 0x11, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x91, 0x3F,
		                                     0x02, 0x00, 0x00, 0x00, 0x79, 0xCD, 0x0B, 0x19, 0x01, 0x00, 0x00, 0x00 };
	CHECK(!memcmp(hello, expectHello, sizeof(expectHello)), "hello header differs from the capture");
	uint8_t bye[UDS_BYE_SIZE];
	udsBuildBye(bye);
	static const uint8_t expectBye[16] = { 0x01, 0x12, 0, 0, 0, 0, 0, 0, 0, 0, 0x85, 0x65, 0, 0, 0, 0 };
	CHECK(!memcmp(bye, expectBye, sizeof(expectBye)), "bye differs from the capture");
	CHECK(udsProfileValue(vectorAppData) == 0x8C4ED336, "profile value from the beacon application data");
}

// The unit layout against the captured unit frames, and the 25-unit window.

static void testUnits(void) {
	size_t i;
	for (i = 0; i < VECTOR_COUNT; ++i) {
		if (strcmp(vectors[i].name, "client unit") && strcmp(vectors[i].name, "host unit") &&
		    strcmp(vectors[i].name, "host window")) {
			continue;
		}
		struct Sig sigs[32];
		int n = sigsOf(vectors[i].data, vectors[i].size, sigs, 32);
		CHECK(n > 0, "%s: no messages", vectors[i].name);
		int j;
		for (j = 0; j < n; ++j) {
			struct UDSUnit unit;
			CHECK(sigs[j].protocol == UDS_PROTOCOL_GAME && udsUnitDecode(sigs[j].payload, sigs[j].length, &unit),
			      "%s: message %d is not a unit", vectors[i].name, j);
			uint8_t again[36];
			udsUnitEncode(again, &unit);
			CHECK(!memcmp(again, sigs[j].payload, 36), "%s: unit %d does not re-encode (index %d)", vectors[i].name, j,
			      (int) unit.index);
			if (j > 0) {
				struct UDSUnit previous;
				udsUnitDecode(sigs[j - 1].payload, 36, &previous);
				CHECK(unit.index == previous.index + 1, "%s: indexes not consecutive", vectors[i].name);
			}
		}
		if (!strcmp(vectors[i].name, "host window")) {
			CHECK(n == UDS_UNIT_WINDOW, "host window holds %d units", n);
		}
	}
}

// The joiner session against the capture: the same session values in, the same messages out.

static void expectSame(const struct Sig* want, const struct Sig* got, const char* what, bool payload) {
	CHECK(want->sender == got->sender && want->destination == got->destination && want->protocol == got->protocol &&
	          want->subtype == got->subtype && want->reliable == got->reliable && want->length == got->length,
	      "%s: header differs (want sender %02X dest %u proto %02X/%02X rel %d len %u; got %02X %u %02X/%02X %d %u)", what,
	      want->sender, want->destination, want->protocol, want->subtype, want->reliable, want->length, got->sender,
	      got->destination, got->protocol, got->subtype, got->reliable, got->length);
	if (payload) {
		CHECK(!memcmp(want->payload, got->payload, want->length < sizeof(want->payload) ? want->length : sizeof(want->payload)),
		      "%s: payload differs", what);
	}
}

static void configFromCapture(struct UDSSessionConfig* config) {
	memset(config, 0, sizeof(*config));
	struct UDSFrame f;
	struct UDSMessage m;
	// vector 1: the joiner's station info; vector 9 carries the station constant id in its Pia header.
	udsFrameParse(vectors[1].data, vectors[1].size, &f);
	size_t pos = 0;
	udsMessageNext(&f, &pos, &m);
	config->sequenceBase = be32(&m.payload[m.length - 4]);
	udsFrameParse(vectors[9].data, vectors[9].size, &f);
	config->connectionId = f.pia.connectionId;
	static const char name[] = "BBGAME";
	size_t i;
	for (i = 0; i < sizeof(name) - 1; ++i) {
		config->name[i] = name[i];
	}
	memcpy(config->appData, vectorAppData, 16);
	config->profileRole = 0x06;
	config->gameRoleFlags[0] = 0x01;
	config->gameRoleFlags[1] = 0x01;
	config->tickBase = 0x25C3B5D2F0ULL;
}

static struct UDSSession session;

static void deliver(const uint8_t* frame, size_t size, uint32_t at) {
	nowMs = at;
	udsSessionReceive(&session, at, frame, size);
	udsSessionPoll(&session, at);
}

static void testJoin(void) {
	struct UDSSessionConfig config;
	configFromCapture(&config);
	memset(&sent, 0, sizeof(sent));
	udsSessionInit(&session, &config, onSend, NULL);
	nowMs = 0;
	udsSessionStart(&session, 0);

	// The capture starts after the host's hello (its sniffer missed it), so one is made up here.
	uint8_t hello[UDS_HELLO_SIZE];
	udsBuildHello(hello, 0x79CD0B19);
	deliver(hello, sizeof(hello), 0);
	CHECK(sent.count == 2, "the hello should bring a reply and the station info, got %d frames", sent.count);
	CHECK(sent.size[0] == UDS_HELLO_REPLY_SIZE && !memcmp(sent.frame[0], vectors[0].data, vectors[0].size),
	      "the hello reply differs from the capture");

	// The host's side of the join, at the times it happened.
	size_t i;
	for (i = 2; i < 14; ++i) {
		if (vectors[i].fromHost) {
			deliver(vectors[i].data, vectors[i].size, (uint32_t) (vectors[i].seconds * 1000.0));
		}
	}
	CHECK(session.state == UDS_STATE_JOINED, "not joined after the mesh state");
	CHECK(session.stationIndex == UDS_STATION_JOINER, "station index not assigned");

	// What the 2DS sent, message by message.
	struct Sig want[16];
	int wantCount = 0;
	static const int clientVectors[] = { 1, 3, 4, 6, 7, 9, 10, 13 };
	for (i = 0; i < sizeof(clientVectors) / sizeof(clientVectors[0]); ++i) {
		wantCount += sigsOf(vectors[clientVectors[i]].data, vectors[clientVectors[i]].size, &want[wantCount],
		                    16 - wantCount);
	}
	struct Sig got[32];
	int gotCount = 0;
	int f;
	for (f = 1; f < sent.count; ++f) { // frame 0 is the hello reply
		gotCount += sigsOf(sent.frame[f], sent.size[f], &got[gotCount], 32 - gotCount);
	}
	CHECK(gotCount == wantCount, "message count: capture %d, ours %d", wantCount, gotCount);

	// The setup messages come in the same order with the same bytes: station info, ack, profile, ack, join request.
	static const char* const names[] = { "station info", "ack of the host's station info", "profile",
		                                 "ack of the host's profile", "join request" };
	for (i = 0; i < 5 && i < (size_t) gotCount; ++i) {
		expectSame(&want[i], &got[i], names[i], true);
		CHECK(want[i].connectionId == got[i].connectionId && want[i].packetId == got[i].packetId,
		      "%s: Pia header ids (capture %02X/%u, ours %02X/%u)", names[i], want[i].connectionId, want[i].packetId,
		      got[i].connectionId, got[i].packetId);
	}
	// After the mesh state: ping, the ack of the mesh state, pong, clock sync request and the system ack. The order and
	// the framing differ a little (the 2DS packs several into one frame); every one of them must be there.
	bool used[32] = { false };
	for (i = 5; i < (size_t) wantCount; ++i) {
		bool found = false;
		int k;
		for (k = 5; k < gotCount && !found; ++k) {
			if (used[k]) {
				continue;
			}
			bool ticks = (want[i].protocol == UDS_PROTOCOL_PING && !want[i].payload[3]) ||
			             (want[i].protocol == UDS_PROTOCOL_SYSTEM && want[i].subtype == UDS_SUBTYPE_CLOCK_SYNC);
			if (want[i].sender == got[k].sender && want[i].destination == got[k].destination &&
			    want[i].protocol == got[k].protocol && want[i].subtype == got[k].subtype &&
			    want[i].reliable == got[k].reliable && want[i].length == got[k].length &&
			    (want[i].protocol != UDS_PROTOCOL_PING || want[i].payload[3] == got[k].payload[3]) &&
			    (ticks || !memcmp(want[i].payload, got[k].payload, want[i].length))) {
				used[k] = true;
				found = true;
				if (want[i].protocol == UDS_PROTOCOL_SYSTEM && want[i].subtype == UDS_SUBTYPE_CLOCK_SYNC) {
					CHECK(got[k].connectionId == 0 && got[k].packetId == 0, "clock sync request is a direct frame");
				}
			}
		}
		CHECK(found, "post-join message %zu (protocol %02X/%02X length %u) was not sent", i, want[i].protocol,
		      want[i].subtype, want[i].length);
		if (!found) {
			int k;
			for (k = 0; k < gotCount; ++k) {
				printf("    ours %d: sender %02X dest %u protocol %02X/%02X reliable %d length %u cid %02X pid %u first %02X %02X %02X %02X\n",
				       k, got[k].sender, got[k].destination, got[k].protocol, got[k].subtype, got[k].reliable,
				       got[k].length, got[k].connectionId, got[k].packetId, got[k].payload[0], got[k].payload[1],
				       got[k].payload[2], got[k].payload[3]);
			}
			printf("    want %zu: sender %02X dest %u protocol %02X/%02X reliable %d length %u first %02X %02X %02X %02X\n", i,
			       want[i].sender, want[i].destination, want[i].protocol, want[i].subtype, want[i].reliable,
			       want[i].length, want[i].payload[0], want[i].payload[1], want[i].payload[2], want[i].payload[3]);
		}
	}
	CHECK(session.hostConstantId == 0x90, "host constant id %02X", session.hostConstantId);

	// The sniffer missed the host's acknowledgements of the three setup messages; make them up, so the session stops
	// re-sending them.
	for (i = 0; i < 3; ++i) {
		uint8_t payload[8] = { 0x05, 0, 0, 0 };
		uint32_t sequence = session.setup[i].sequence;
		payload[4] = sequence >> 24;
		payload[5] = sequence >> 16;
		payload[6] = sequence >> 8;
		payload[7] = sequence;
		struct UDSPiaHeader pia = { .connectionId = 0x90, .packetId = 30 + (uint16_t) i, .clock = 7000, .peerClock = 6500 };
		uint8_t frame[UDS_MAX_FRAME_SIZE];
		size_t pos = udsFrameBegin(frame, &pia);
		struct UDSMessage m = { .sender = UDS_STATION_HOST, .destination = UDS_ID_JOINER, .protocol = UDS_PROTOCOL_SETUP,
			                    .length = 8, .payload = payload };
		pos = udsFrameEnd(frame, sizeof(frame), udsFrameAppend(frame, sizeof(frame), pos, &m));
		deliver(frame, pos, 340);
		CHECK(session.setup[i].acked, "setup message %zu not marked acknowledged", i);
	}
}

// Units, acknowledgements, windows and re-sends against a made-up host.

static size_t hostFrame(uint8_t* out, const struct UDSUnit* units, int count, const struct UDSMessage* extra, uint16_t packetId) {
	struct UDSPiaHeader pia = { .connectionId = 0x90, .packetId = packetId, .clock = 9000, .peerClock = 8000 };
	size_t pos = udsFrameBegin(out, &pia);
	int i;
	for (i = 0; i < count; ++i) {
		uint8_t payload[36];
		udsUnitEncode(payload, &units[i]);
		struct UDSMessage m = { .sender = UDS_STATION_HOST, .destination = UDS_ID_JOINER, .protocol = UDS_PROTOCOL_GAME,
			                    .length = 36, .payload = payload };
		pos = udsFrameAppend(out, UDS_MAX_FRAME_SIZE, pos, &m);
	}
	if (extra) {
		pos = udsFrameAppend(out, UDS_MAX_FRAME_SIZE, pos, extra);
	}
	return udsFrameEnd(out, UDS_MAX_FRAME_SIZE, pos);
}

static void testUnitStream(void) {
	uint8_t frame[UDS_MAX_FRAME_SIZE];
	uint8_t byte;
	nowMs = 1000;
	sent.count = 0;

	// The host's first unit is EF at index -2001; the joiner answers with an ack of -2000.
	struct UDSUnit ef = { .index = -2001, .ack = -2001, .byte = 0xEF };
	size_t size = hostFrame(frame, &ef, 1, NULL, 20);
	deliver(frame, size, 1000);
	CHECK(udsSessionPopUnit(&session, &byte) && byte == 0xEF, "EF not delivered");
	CHECK(!udsSessionPopUnit(&session, &byte), "an extra unit appeared");
	CHECK(sent.count == 1, "expected one ack frame, got %d", sent.count);
	struct Sig sig[4];
	CHECK(sigsOf(sent.frame[0], sent.size[0], sig, 4) == 1 && sig[0].protocol == UDS_PROTOCOL_GAME && sig[0].length == 24 &&
	          (int32_t) be32(&sig[0].payload[12]) == -2000,
	      "ack of the first unit");

	// Three units out, in one frame; the first is what the 2DS sent first (index -2001, byte 00).
	sent.count = 0;
	CHECK(udsSessionQueueUnit(&session, 0x00) && udsSessionQueueUnit(&session, 0x60) && udsSessionQueueUnit(&session, 0x60),
	      "queue");
	udsSessionFlush(&session, 1010);
	CHECK(sent.count == 1 && sent.size[0] == UDS_FRAME_HEADER_SIZE + 3 * UDS_UNIT_SIZE + UDS_TAIL_SIZE,
	      "one 3-unit frame, got %d frames of %zu", sent.count, sent.count ? sent.size[0] : 0);
	struct Sig units[8];
	int n = sigsOf(sent.frame[0], sent.size[0], units, 8);
	CHECK(n == 3, "units in the frame: %d", n);
	struct UDSUnit unit;
	CHECK(udsUnitDecode(units[0].payload, 36, &unit) && unit.index == -2001 && unit.ack == -2000 && unit.byte == 0x00 &&
	          unit.roleFlags[0] == 1 && unit.roleFlags[1] == 1 && unit.roleFlags[2] == 0,
	      "first unit: index %d ack %d byte %02X", (int) unit.index, (int) unit.ack, unit.byte);
	CHECK(units[0].sender == UDS_STATION_JOINER && units[0].destination == UDS_ID_HOST, "unit addressing");

	// No ack for 500 ms: the three go again, from the first.
	sent.count = 0;
	udsSessionPoll(&session, 1400);
	CHECK(sent.count == 0, "re-sent too early (%d frames)", sent.count);
	sent.count = 0;
	udsSessionPoll(&session, 1600);
	bool resent = false;
	int f;
	for (f = 0; f < sent.count; ++f) {
		n = sigsOf(sent.frame[f], sent.size[f], units, 8);
		if (n == 3 && udsUnitDecode(units[0].payload, 36, &unit) && unit.index == -2001) {
			resent = true;
		}
	}
	CHECK(resent, "the unacknowledged units were not re-sent");

	// The host acks two of them; only the third is re-sent after that.
	struct UDSUnit ack0 = { .index = -2000, .ack = -1999, .byte = 0x00 };
	size = hostFrame(frame, &ack0, 1, NULL, 21);
	deliver(frame, size, 1700);
	CHECK(session.sendAcked == -1999, "send ack at %d", (int) session.sendAcked);
	CHECK(udsSessionPopUnit(&session, &byte) && byte == 0x00, "unit -2000 delivered");

	// Units out of order are delivered in order: -1998 arrives before -1999.
	struct UDSUnit late = { .index = -1998, .ack = -1999, .byte = 0x62 };
	size = hostFrame(frame, &late, 1, NULL, 22);
	deliver(frame, size, 1710);
	CHECK(!udsSessionPopUnit(&session, &byte), "an out-of-order unit was delivered early");
	struct UDSUnit gap = { .index = -1999, .ack = -1999, .byte = 0x61 };
	size = hostFrame(frame, &gap, 1, NULL, 23);
	deliver(frame, size, 1720);
	CHECK(udsSessionPopUnit(&session, &byte) && byte == 0x61 && udsSessionPopUnit(&session, &byte) && byte == 0x62,
	      "the pair did not come out in order");
	// A repeat is dropped.
	deliver(frame, size, 1730);
	CHECK(!udsSessionPopUnit(&session, &byte), "a repeated unit was delivered twice");
	CHECK(session.duplicateUnits == 1, "duplicate count %u", session.duplicateUnits);

	// A block: 60 units go out as 25 + 25 + 10.
	sent.count = 0;
	int i;
	for (i = 0; i < 60; ++i) {
		CHECK(udsSessionQueueUnit(&session, (uint8_t) i), "queue %d", i);
	}
	udsSessionFlush(&session, 1800);
	CHECK(sent.count == 3 && sent.size[0] == UDS_MAX_FRAME_SIZE && sent.size[1] == UDS_MAX_FRAME_SIZE &&
	          sent.size[2] == UDS_FRAME_HEADER_SIZE + 10 * UDS_UNIT_SIZE + UDS_TAIL_SIZE,
	      "60 units went out as %d frames (%zu, %zu, %zu)", sent.count, sent.size[0], sent.size[1], sent.size[2]);
	struct Sig window[UDS_UNIT_WINDOW];
	n = sigsOf(sent.frame[0], sent.size[0], window, UDS_UNIT_WINDOW);
	CHECK(n == UDS_UNIT_WINDOW, "a full frame holds %d units", n);
	for (i = 0; i < n; ++i) {
		CHECK(udsUnitDecode(window[i].payload, 36, &unit) && unit.byte == (uint8_t) i &&
		          (uint16_t) (window[i].payload[32] | (window[i].payload[33] << 8)) == (uint16_t) (unit.index + 2001),
		      "unit %d of the block", i);
	}
}

static void testClose(void) {
	nowMs = 20000;
	udsSessionPoll(&session, 20000);
	CHECK(session.state == UDS_STATE_CLOSED, "ten seconds of silence should close the session");

	struct UDSSessionConfig config;
	configFromCapture(&config);
	struct UDSSession other;
	udsSessionInit(&other, &config, onSend, NULL);
	udsSessionStart(&other, 0);
	uint8_t bye[UDS_BYE_SIZE];
	udsBuildBye(bye);
	udsSessionReceive(&other, 0, bye, sizeof(bye));
	CHECK(other.state == UDS_STATE_CLOSED, "a bye should close the session");
}

int main(void) {
	testCodec();
	testUnits();
	testJoin();
	testUnitStream();
	testClose();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
