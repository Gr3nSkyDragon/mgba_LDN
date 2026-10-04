/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Test of the joiner's side of the Azahar test bridge (uds-room.c, uds-udp.c, uds-joiner.c). An in-process fake Azahar host
 * talks to a UDSJoiner over real UDP sockets on 127.0.0.1 and plays the packets Azahar's nwm::UDS sends when it hosts
 * (beacon, authentication SEQ2, association response, EAPoL logoff, SecureData). The packet bodies are built by hand from
 * Azahar's generators (uds_beacon.cpp, uds_data.cpp, uds_connection.cpp), so a mismatch with the real Azahar would show up
 * only in a live run. Needs no game and no radio. Build target: uds-bridge-test. Exit status 0 when every check passes.
 */
#include <mgba/internal/gb/sio/uds-joiner.h>
#include <mgba/internal/gb/sio/uds-pia.h>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP_MS(ms) usleep((ms) *1000)
#endif

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

// The fake host is another socket pair with the ports swapped.
#define JOINER_LISTEN 45791
#define JOINER_SEND 45790

static struct UDSUdp host;
static struct UDSJoiner joiner;
static uint32_t nowMs;
static const uint8_t hostMac[6] = {0x40, 0xF4, 0x07, 0x11, 0x22, 0x33};

static void hostSend(uint8_t type, const uint8_t* dest, const uint8_t* data, size_t size) {
	struct UDSRoomPacket packet;
	memset(&packet, 0, sizeof(packet));
	packet.type = type;
	packet.channel = 6;
	memcpy(packet.transmitter, hostMac, 6);
	memcpy(packet.destination, dest, 6);
	packet.data = data;
	packet.size = size;
	uint8_t datagram[UDS_BRIDGE_MAX_DATAGRAM];
	size_t length = udsRoomEncode(datagram, sizeof(datagram), &packet);
	udsUdpSend(&host, datagram, length);
}

// Lets the joiner run for a few milliseconds of real time (and of simulated time, one for one).
static void run(int ms) {
	int i;
	for (i = 0; i < ms; ++i) {
		SLEEP_MS(1);
		++nowMs;
		udsJoinerPoll(&joiner, nowMs);
	}
}

// The next datagram the fake host received, decoded; false when none arrived.
static uint8_t hostBuffer[UDS_BRIDGE_MAX_DATAGRAM];
static bool hostReceive(struct UDSRoomPacket* packet) {
	size_t size = udsUdpReceive(&host, hostBuffer, sizeof(hostBuffer));
	return size && udsRoomDecode(hostBuffer, size, packet);
}

static size_t beaconBody(uint8_t* out) {
	size_t pos = 0;
	memset(out, 0, 12); // timestamp, interval, capabilities: not read
	pos = 12;
	// SSID: eight zero bytes, as a retail host sends
	out[pos++] = 0;
	out[pos++] = 8;
	memset(&out[pos], 0, 8);
	pos += 8;
	// the Nintendo "dummy" tag (type 20): must be skipped
	static const uint8_t dummy[] = {221, 7, 0x00, 0x1F, 0x32, 20, 0x0A, 0x00, 0x00};
	memcpy(&out[pos], dummy, sizeof(dummy));
	pos += sizeof(dummy);
	// the network info tag (type 21)
	size_t tag = pos;
	out[pos++] = 221;
	out[pos++] = 52 + 16;
	uint8_t* body = &out[pos];
	memset(body, 0, 52 + 16);
	body[0] = 0x00;
	body[1] = 0x1F;
	body[2] = 0x32;
	body[3] = 21;
	body[4] = 0x00;
	body[5] = 0x17;
	body[6] = 0x10;
	body[7] = 0x10; // comm id 0x00171010
	body[8] = 1; // id
	body[9] = 1; // update counter
	body[12] = 0x12;
	body[13] = 0x34;
	body[14] = 0x56;
	body[15] = 0x78; // network id
	body[16] = 1;
	body[17] = 2;
	body[51] = 16;
	memcpy(&body[52], vectorAppData, 16);
	pos += 52 + 16;
	(void) tag;
	return pos;
}

static void testCodec(void) {
	// The EAPoL start, byte for byte as Azahar's GenerateEAPoLStartFrame writes it for the name "BB".
	uint16_t name[UDS_NAME_WORDS] = {'B', 'B'};
	uint8_t start[UDS_LLC_SIZE + 0x30];
	CHECK(udsBuildEapolStart(start, sizeof(start), 1, 0x0000A5A5A5A5A5A5ULL, name) == sizeof(start), "EAPoL start size");
	static const uint8_t expect[] = {
		0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E, // LLC, EAPoL
		0x02, 0x01, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, // magic, association id, connection type, 01 00
		0x00, 0x00, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, // friend code seed
		0x00, 0x42, 0x00, 0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // name, big-endian
		0, 0, 0, 0, // padding
		0x00, 0x00, // node id, assigned by the host
		0x15, 0x00, 0x04, 0xE9, 0x13, 0x00, // fixed trailer
	};
	CHECK(!memcmp(start, expect, sizeof(expect)), "EAPoL start differs from the Azahar layout");

	uint8_t auth[6];
	udsBuildAuth(auth, 1);
	static const uint8_t wantAuth[6] = {0, 0, 1, 0, 0, 0};
	CHECK(!memcmp(auth, wantAuth, 6), "authentication SEQ1 body");

	// SecureData header: protocol size 14 + 3, count 1, securedata size 13, not management, channel 243, sequence 7,
	// destination 1, source 2.
	uint8_t secure[64];
	const uint8_t payload[3] = {1, 2, 3};
	size_t size = udsBuildSecureData(secure, sizeof(secure), payload, 3, 243, 1, 2, 7, false);
	static const uint8_t wantSecure[] = {0xAA, 0xAA, 0x03, 0, 0, 0, 0x87, 0x6D, 0x00, 0x11, 0x00, 0x01, 0x00, 0x0D,
	                                     0x00, 0xF3, 0x00, 0x07, 0x00, 0x01, 0x00, 0x02, 1, 2, 3};
	CHECK(size == sizeof(wantSecure) && !memcmp(secure, wantSecure, size), "SecureData frame");

	struct UDSRoomPacket p = {.type = 1, .channel = 6, .data = payload, .size = 3};
	uint8_t datagram[64];
	size = udsRoomEncode(datagram, sizeof(datagram), &p);
	struct UDSRoomPacket q;
	CHECK(size == UDS_BRIDGE_HEADER_SIZE + 3 && udsRoomDecode(datagram, size, &q) && q.type == 1 && q.channel == 6 &&
	          q.size == 3 && !memcmp(q.data, payload, 3),
	      "datagram round trip");
	datagram[0] = 'X';
	CHECK(!udsRoomDecode(datagram, size, &q), "a datagram with the wrong magic was accepted");
}

static void testJoin(void) {
	uint16_t name[UDS_NAME_WORDS] = {'B', 'B', 'G', 'A', 'M', 'E'};
	CHECK(udsUdpOpen(&host, JOINER_SEND, JOINER_LISTEN), "fake host cannot open its sockets");
	CHECK(udsJoinerOpen(&joiner, name, JOINER_LISTEN, JOINER_SEND), "joiner cannot open its sockets");
	if (!host.open || !joiner.udp.open) {
		return;
	}
	struct UDSRoomPacket got;
	nowMs = 0;

	// A beacon for another game is ignored.
	uint8_t beacon[512];
	size_t size = beaconBody(beacon);
	beacon[12 + 10 + 9 + 2 + 7] = 0x99; // the comm id's low byte (body[7]) of the network info tag
	hostSend(UDS_PACKET_BEACON, (const uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, beacon, size);
	run(30);
	CHECK(!hostReceive(&got), "the joiner answered a beacon of another game");
	CHECK(joiner.room.state == UDS_ROOM_SCAN && joiner.room.beaconsSeen == 1, "state after a foreign beacon");

	// The right beacon: the joiner authenticates.
	size = beaconBody(beacon);
	hostSend(UDS_PACKET_BEACON, (const uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, beacon, size);
	run(30);
	CHECK(hostReceive(&got) && got.type == UDS_PACKET_AUTH && got.size == 6 && got.data[2] == 1 &&
	          !memcmp(got.destination, hostMac, 6) && !memcmp(got.transmitter, joiner.room.mac, 6) && got.channel == 6,
	      "authentication SEQ1 not sent as expected");
	CHECK(joiner.room.state == UDS_ROOM_AUTH, "state after the beacon: %d", joiner.room.state);
	CHECK(joiner.room.host.networkId == 0x12345678 && joiner.room.host.commId == 0x00171010 &&
	          !memcmp(joiner.room.host.appData, vectorAppData, 16),
	      "beacon fields: network id %08X comm id %08X", joiner.room.host.networkId, joiner.room.host.commId);

	// Silence: the request is repeated after a second.
	run(1100);
	CHECK(hostReceive(&got) && got.type == UDS_PACKET_AUTH, "no repeat of the authentication request");
	while (hostReceive(&got)) {
	}

	// Authentication SEQ2 and the association response (status 0, association id 0xC001, then an SSID tag).
	const uint8_t seq2[6] = {0, 0, 2, 0, 0, 0};
	hostSend(UDS_PACKET_AUTH, joiner.room.mac, seq2, sizeof(seq2));
	const uint8_t assoc[] = {0x31, 0x04, 0x00, 0x00, 0x01, 0xC0, 0x00, 0x08, '1', '2', '3', '4', '5', '6', '7', '8'};
	hostSend(UDS_PACKET_ASSOC_RESPONSE, joiner.room.mac, assoc, sizeof(assoc));
	run(30);
	CHECK(hostReceive(&got) && got.type == UDS_PACKET_DATA && got.size == UDS_LLC_SIZE + 0x30 && got.data[6] == 0x88 &&
	          got.data[7] == 0x8E && got.data[8] == 0x02 && got.data[9] == 0x01 && got.data[10] == 0x00 && got.data[11] == 0x01,
	      "EAPoL start not sent after the association response");
	CHECK(joiner.room.state == UDS_ROOM_EAPOL && joiner.room.associationId == 1, "state after association: %d id %u",
	      joiner.room.state, joiner.room.associationId);

	// The host's EAPoL logoff: node 2 assigned, two of two nodes connected.
	uint8_t logoff[UDS_LLC_SIZE + 0x298];
	memset(logoff, 0, sizeof(logoff));
	memcpy(logoff, (const uint8_t[]){0xAA, 0xAA, 0x03, 0, 0, 0, 0x88, 0x8E}, 8);
	logoff[8] = 0x02;
	logoff[9] = 0x02;
	logoff[12] = 0x00;
	logoff[13] = 0x02; // assigned node id
	memcpy(&logoff[14], joiner.room.mac, 6);
	logoff[8 + 18] = 2;
	logoff[8 + 19] = 2;
	hostSend(UDS_PACKET_DATA, joiner.room.mac, logoff, sizeof(logoff));
	run(30);
	CHECK(joiner.room.state == UDS_ROOM_JOINED && joiner.room.host.nodeId == 2 && joiner.room.host.connectedNodes == 2,
	      "joined: state %d node %u", joiner.room.state, joiner.room.host.nodeId);
	CHECK(joiner.sessionActive && joiner.session.state == UDS_STATE_IDLE, "the Pia session was not started");
	CHECK(!udsJoinerReady(&joiner), "ready before the mesh state");

	// The host's Pia hello inside a SecureData frame; the joiner answers with its hello reply, wrapped the same way.
	uint8_t hello[UDS_HELLO_SIZE];
	udsBuildHello(hello, 0x79CD0B19);
	uint8_t frame[256];
	size = udsBuildSecureData(frame, sizeof(frame), hello, sizeof(hello), 243, 2, 1, 0, false);
	hostSend(UDS_PACKET_DATA, joiner.room.mac, frame, size);
	run(40);
	bool sawReply = false, sawSetup = false;
	while (hostReceive(&got)) {
		if (got.type != UDS_PACKET_DATA || got.size < UDS_LLC_SIZE + UDS_SECURE_HEADER_SIZE || got.data[6] != 0x87) {
			continue;
		}
		const uint8_t* h = &got.data[UDS_LLC_SIZE];
		CHECK(h[7] == 243 && h[6] == 0 && h[11] == 1 && h[13] == 2 && !memcmp(got.destination, hostMac, 6),
		      "SecureData addressing: channel %u dest %u source %u", h[7], h[11], h[13]);
		struct UDSFrame f;
		if (udsFrameParse(&h[UDS_SECURE_HEADER_SIZE], got.size - UDS_LLC_SIZE - UDS_SECURE_HEADER_SIZE, &f)) {
			if (f.kind == UDS_FRAME_HELLO_REPLY) {
				sawReply = true;
			} else if (f.kind == UDS_FRAME_PIA) {
				sawSetup = true;
			}
		}
	}
	CHECK(sawReply, "no hello reply came back through SecureData");
	CHECK(sawSetup, "no Pia frame (station info) followed the hello");

	// A management packet or another channel is not Pia: nothing reaches the session.
	unsigned before = joiner.session.framesReceived;
	size = udsBuildSecureData(frame, sizeof(frame), hello, sizeof(hello), 3, 2, 1, 1, true);
	hostSend(UDS_PACKET_DATA, joiner.room.mac, frame, size);
	size = udsBuildSecureData(frame, sizeof(frame), hello, sizeof(hello), 200, 2, 1, 2, false);
	hostSend(UDS_PACKET_DATA, joiner.room.mac, frame, size);
	run(30);
	CHECK(joiner.session.framesReceived == before, "a non-Pia SecureData frame reached the session");

	// A host that leaves sends a deauthentication: scanning resumes.
	hostSend(UDS_PACKET_DEAUTH, joiner.room.mac, (const uint8_t[]){0x03, 0x00}, 2);
	run(30);
	CHECK(joiner.room.state == UDS_ROOM_SCAN, "no return to scanning after a deauthentication");

	udsJoinerClose(&joiner);
	udsUdpClose(&host);
}

int main(void) {
	testCodec();
	testJoin();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
