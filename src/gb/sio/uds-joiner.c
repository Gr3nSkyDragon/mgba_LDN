/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-joiner.h>

#include <stdlib.h>
#include <string.h>

static void _roomSend(void* context, const uint8_t* datagram, size_t size) {
	struct UDSJoiner* joiner = context;
	udsUdpSend(&joiner->udp, datagram, size);
}

static void _sessionSend(void* context, const uint8_t* frame, size_t size) {
	struct UDSJoiner* joiner = context;
	udsRoomSendPia(&joiner->room, frame, size);
}

static void _roomPia(void* context, const uint8_t* payload, size_t size) {
	struct UDSJoiner* joiner = context;
	if (joiner->sessionActive) {
		udsSessionReceive(&joiner->session, joiner->nowMs, payload, size);
	}
}

static void _roomJoined(void* context, const struct UDSRoomHost* host) {
	struct UDSJoiner* joiner = context;
	struct UDSSessionConfig config;
	memset(&config, 0, sizeof(config));
	// The Pia connection id is random per session (2..255). The setup sequence values count from the low 32 bits of the
	// 3DS tick clock, which only has to look like one.
	config.connectionId = 2 + rand() % 254;
	config.tickBase = 0x25C3B5D2F0ULL + ((uint64_t) (rand() & 0xFFFF) << 16);
	config.sequenceBase = (uint32_t) config.tickBase;
	memcpy(config.appData, host->appData, sizeof(config.appData));
	memcpy(config.name, joiner->name, sizeof(config.name));
	config.profileRole = 0x06;
	config.gameRoleFlags[0] = 0x01;
	config.gameRoleFlags[1] = 0x01;
	udsSessionInit(&joiner->session, &config, _sessionSend, joiner);
	udsSessionStart(&joiner->session, joiner->nowMs);
	joiner->sessionActive = true;
}

bool udsJoinerOpen(struct UDSJoiner* joiner, const uint16_t name[UDS_NAME_WORDS], uint16_t listenPort, uint16_t sendPort) {
	memset(joiner, 0, sizeof(*joiner));
	memcpy(joiner->name, name, sizeof(joiner->name));
	if (!listenPort || !sendPort) {
		udsUdpPortsFromEnvironment(&listenPort, &sendPort);
	}
	if (!udsUdpOpen(&joiner->udp, listenPort, sendPort)) {
		return false;
	}
	// A locally administered unicast address.
	uint8_t mac[6] = {0x02, 0x47, 0x42, rand() & 0xFF, rand() & 0xFF, rand() & 0xFF};
	udsRoomInit(&joiner->room, mac, joiner->name, _roomSend, _roomJoined, _roomPia, joiner);
	return true;
}

void udsJoinerClose(struct UDSJoiner* joiner) {
	udsUdpClose(&joiner->udp);
	joiner->sessionActive = false;
}

void udsJoinerPoll(struct UDSJoiner* joiner, uint32_t nowMs) {
	joiner->nowMs = nowMs;
	uint8_t datagram[UDS_BRIDGE_MAX_DATAGRAM];
	size_t size;
	while ((size = udsUdpReceive(&joiner->udp, datagram, sizeof(datagram))) > 0) {
		udsRoomReceive(&joiner->room, nowMs, datagram, size);
	}
	udsRoomPoll(&joiner->room, nowMs);
	if (joiner->sessionActive) {
		udsSessionPoll(&joiner->session, nowMs);
		if (joiner->session.state == UDS_STATE_CLOSED) {
			joiner->sessionActive = false;
			joiner->room.state = UDS_ROOM_SCAN; // the host is gone: wait for the next beacon
		}
	}
}

bool udsJoinerReady(const struct UDSJoiner* joiner) {
	return joiner->sessionActive && joiner->session.state == UDS_STATE_JOINED;
}
