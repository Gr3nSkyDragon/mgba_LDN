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
	if (joiner->useRadio) {
		udsAirRadioSend(&joiner->radio, datagram, size);
	} else {
		udsUdpSend(&joiner->udp, datagram, size);
	}
}

// A packet the radio received, in the room's datagram form.
static void _radioDeliver(void* context, const uint8_t* datagram, size_t size) {
	struct UDSJoiner* joiner = context;
	udsRoomReceive(&joiner->room, joiner->nowMs, datagram, size);
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

bool udsJoinerOpenRadio(struct UDSJoiner* joiner, const uint16_t name[UDS_NAME_WORDS], const char* portName, const char* keyPath, char* error,
                        size_t errorSize) {
	memset(joiner, 0, sizeof(*joiner));
	memcpy(joiner->name, name, sizeof(joiner->name));
	uint8_t mac[6] = {0x02, 0x47, 0x42, rand() & 0xFF, rand() & 0xFF, rand() & 0xFF};
	joiner->useRadio = true;
	if (!udsAirRadioOpen(&joiner->radio, portName, keyPath, mac, _radioDeliver, joiner, error, errorSize)) {
		return false;
	}
	udsRoomInit(&joiner->room, mac, joiner->name, _roomSend, _roomJoined, _roomPia, joiner);
	return true;
}

void udsJoinerClose(struct UDSJoiner* joiner) {
	if (joiner->useRadio) {
		udsAirRadioClose(&joiner->radio);
	}
	udsUdpClose(&joiner->udp);
	joiner->sessionActive = false;
}

void udsJoinerPoll(struct UDSJoiner* joiner, uint32_t nowMs) {
	joiner->nowMs = nowMs;
	uint8_t datagram[UDS_BRIDGE_MAX_DATAGRAM];
	size_t size;
	if (joiner->useRadio) {
		udsAirRadioPoll(&joiner->radio, nowMs);
	} else {
		while ((size = udsUdpReceive(&joiner->udp, datagram, sizeof(datagram))) > 0) {
			udsRoomReceive(&joiner->room, nowMs, datagram, size);
		}
	}
	udsRoomPoll(&joiner->room, nowMs);
	if (joiner->leaveWithHost && joiner->sessionActive && joiner->session.hostLeaving) {
		// The host's game is leaving the room. Its VC waits about five seconds for its partner's end-of-session record and then closes the
		// network ("communication lost"): answer with ours (once more a moment later, in case the first is lost), then leave the network
		// as a 3DS joiner does.
		if (!joiner->leaveStartMs) {
			joiner->leaveStartMs = nowMs ? nowMs : 1;
			udsSessionSendLeave(&joiner->session, nowMs);
		} else if (nowMs - joiner->leaveStartMs >= UDS_JOINER_LEAVE_RESEND_MS && !joiner->leaveResent) {
			joiner->leaveResent = true;
			udsSessionSendLeave(&joiner->session, nowMs);
		} else if (nowMs - joiner->leaveStartMs >= UDS_JOINER_LEAVE_DELAY_MS) {
			joiner->sessionActive = false;
			joiner->leaveStartMs = 0;
			joiner->leaveResent = false;
			udsRoomLeave(&joiner->room, nowMs, UDS_JOINER_REJOIN_HOLD_MS);
		}
	}
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
