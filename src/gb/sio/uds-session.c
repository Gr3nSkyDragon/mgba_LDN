/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-session.h>

#define SETUP_RESEND_MS 500
#define UNIT_RESEND_MS 500
#define PING_MS 2000
#define CLOCK_SYNC_MS 2000
#define KEEPALIVE_MS 2000
#define SILENCE_MS 10000
#define SETUP_FALLBACK_MS 1500
#define TICKS_PER_SECOND 268111856ULL

enum {
	SLOT_STATION_INFO,
	SLOT_PROFILE,
	SLOT_JOIN_REQUEST,
};

static void _put32(uint8_t* p, uint32_t v) {
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static uint32_t _get32(const uint8_t* p) {
	return ((uint32_t) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static bool _due(uint32_t nowMs, uint32_t since, uint32_t interval) {
	return (int32_t) (nowMs - since) >= (int32_t) interval;
}

uint32_t udsProfileValue(const uint8_t appData[16]) {
	return appData[4] | (appData[5] << 8) | (appData[6] << 16) | ((uint32_t) appData[7] << 24);
}

void udsUnitEncode(uint8_t payload[36], const struct UDSUnit* unit) {
	memset(payload, 0, 36);
	payload[1] = 0x03;
	payload[3] = 0x0C;
	_put32(&payload[8], (uint32_t) unit->index);
	_put32(&payload[12], (uint32_t) unit->ack);
	memcpy(&payload[16], unit->lossBits, 8);
	payload[24] = unit->byte;
	memcpy(&payload[25], unit->roleFlags, 3);
	_put32(&payload[28], unit->value);
	uint16_t counter = (uint16_t) (unit->index - UDS_FIRST_INDEX);
	payload[32] = counter;
	payload[33] = counter >> 8;
}

bool udsUnitDecode(const uint8_t* payload, size_t size, struct UDSUnit* unit) {
	if (size != 36 || payload[0] || payload[1] != 0x03 || payload[2] || payload[3] != 0x0C) {
		return false;
	}
	unit->index = (int32_t) _get32(&payload[8]);
	unit->ack = (int32_t) _get32(&payload[12]);
	memcpy(unit->lossBits, &payload[16], 8);
	unit->byte = payload[24];
	memcpy(unit->roleFlags, &payload[25], 3);
	unit->value = _get32(&payload[28]);
	return true;
}

static uint64_t _ticks(const struct UDSSession* session, uint32_t nowMs) {
	return session->config.tickBase + (uint64_t) (nowMs - session->startMs) * TICKS_PER_SECOND / 1000;
}

// Sending

static void _sendFrame(struct UDSSession* session, uint32_t nowMs, uint8_t* frame, size_t size) {
	session->lastTxMs = nowMs;
	++session->framesSent;
	session->send(session->context, frame, size);
}

// Frames before the host's mesh state have connection id 01 and packet id 0; from the mesh state on they carry the station
// constant id and count up from 1. Clock sync requests always go out as "direct" frames: connection id 0, packet id 0.
static void _beginPia(struct UDSSession* session, uint32_t nowMs, uint8_t* frame, size_t* pos, bool direct) {
	struct UDSPiaHeader pia;
	if (direct) {
		pia.connectionId = 0;
		pia.packetId = 0;
	} else if (session->piaNumbered) {
		pia.connectionId = session->config.connectionId;
		pia.packetId = ++session->packetId;
	} else {
		pia.connectionId = UDS_STATION_JOINER;
		pia.packetId = 0;
	}
	pia.clock = nowMs - session->startMs;
	pia.peerClock = session->haveHostClock ? (uint16_t) (session->hostClock + (nowMs - session->hostClockMs)) : 0;
	*pos = udsFrameBegin(frame, &pia);
}

static uint32_t _destination(const struct UDSSession* session) {
	return session->stationIndex == UDS_STATION_UNASSIGNED ? UDS_ID_BROADCAST : UDS_ID_HOST;
}

static void _sendMessageEx(struct UDSSession* session, uint32_t nowMs, uint8_t protocol, uint8_t subtype,
                           uint8_t reliable, uint32_t destination, const uint8_t* payload, uint16_t length, bool direct) {
	uint8_t frame[UDS_MAX_FRAME_SIZE];
	size_t pos;
	_beginPia(session, nowMs, frame, &pos, direct);
	struct UDSMessage message = {
		.sender = session->stationIndex,
		.destination = destination,
		.protocol = protocol,
		.subtype = subtype,
		.reliable = reliable,
		.length = length,
		.payload = payload,
	};
	pos = udsFrameAppend(frame, sizeof(frame), pos, &message);
	if (!pos) {
		return;
	}
	pos = udsFrameEnd(frame, sizeof(frame), pos);
	if (pos) {
		_sendFrame(session, nowMs, frame, pos);
	}
}

static void _sendMessage(struct UDSSession* session, uint32_t nowMs, uint8_t protocol, uint8_t subtype, uint8_t reliable,
                         uint32_t destination, const uint8_t* payload, uint16_t length) {
	_sendMessageEx(session, nowMs, protocol, subtype, reliable, destination, payload, length, false);
}

static void _sendSequenceAck(struct UDSSession* session, uint32_t nowMs, uint32_t sequence) {
	uint8_t payload[8] = { 0x05, 0, 0, 0 };
	_put32(&payload[4], sequence);
	// Always addressed to 0, before and after the index is assigned.
	_sendMessage(session, nowMs, UDS_PROTOCOL_SETUP, 0, 0, UDS_ID_BROADCAST, payload, sizeof(payload));
}

static void _sendRecordAck(struct UDSSession* session, uint32_t nowMs, uint8_t protocol, uint8_t reliable, int32_t ack) {
	uint8_t payload[24] = { 0 };
	_put32(&payload[12], (uint32_t) ack);
	_sendMessage(session, nowMs, protocol, 0, reliable, UDS_ID_HOST, payload, sizeof(payload));
}

// Setup messages (station info, profile, join request)

static struct UDSSetupMessage* _queueSetup(struct UDSSession* session, int slot, uint8_t protocol, uint16_t length) {
	struct UDSSetupMessage* message = &session->setup[slot];
	memset(message, 0, sizeof(*message));
	message->used = true;
	message->protocol = protocol;
	message->length = length;
	message->sequence = session->nextSequence++;
	_put32(&message->payload[length - 4], message->sequence);
	return message;
}

static void _queueStationInfo(struct UDSSession* session) {
	struct UDSSetupMessage* message = _queueSetup(session, SLOT_STATION_INFO, UDS_PROTOCOL_SETUP, 62);
	message->payload[0] = 0x01;
	message->payload[1] = session->config.connectionId;
	message->payload[2] = 0x05;
	message->payload[3] = 0x00; // the host flag: 01 only in the host's
	message->payload[11] = UDS_ID_JOINER;
}

static void _queueProfile(struct UDSSession* session) {
	struct UDSSetupMessage* message = _queueSetup(session, SLOT_PROFILE, UDS_PROTOCOL_SETUP, 80);
	message->payload[0] = 0x02;
	message->payload[2] = 0x05;
	message->payload[3] = 0x02;
	int i;
	for (i = 0; i < UDS_NAME_CHARS; ++i) {
		uint16_t c = session->config.name[i];
		message->payload[4 + i * 2] = c; // little endian copy
		message->payload[5 + i * 2] = c >> 8;
		message->payload[36 + i * 2] = c >> 8; // big endian copy
		message->payload[37 + i * 2] = c;
	}
	message->payload[68] = session->config.profileRole;
	_put32(&message->payload[70], udsProfileValue(session->config.appData)); // the sequence is at 76..79
}

static void _queueJoinRequest(struct UDSSession* session) {
	struct UDSSetupMessage* message = _queueSetup(session, SLOT_JOIN_REQUEST, UDS_PROTOCOL_SYSTEM, 16);
	message->payload[0] = 0x01;
	message->payload[1] = UDS_STATION_UNASSIGNED;
	message->payload[11] = UDS_ID_JOINER;
}

static void _sendSetup(struct UDSSession* session, uint32_t nowMs, struct UDSSetupMessage* message) {
	message->everSent = true;
	message->lastSent = nowMs;
	_sendMessage(session, nowMs, message->protocol, 0, 0, UDS_ID_BROADCAST, message->payload, message->length);
}

static void _pumpSetup(struct UDSSession* session, uint32_t nowMs) {
	int i;
	for (i = 0; i < 3; ++i) {
		struct UDSSetupMessage* message = &session->setup[i];
		if (!message->used || message->acked) {
			continue;
		}
		if (!message->everSent || _due(nowMs, message->lastSent, SETUP_RESEND_MS)) {
			_sendSetup(session, nowMs, message);
		}
	}
}

// Receiving

static bool _handledSequence(struct UDSSession* session, uint32_t sequence) {
	int i;
	for (i = 0; i < session->hostSequenceCount; ++i) {
		if (session->hostSequences[i] == sequence) {
			return true;
		}
	}
	if (session->hostSequenceCount < (int) (sizeof(session->hostSequences) / sizeof(session->hostSequences[0]))) {
		session->hostSequences[session->hostSequenceCount++] = sequence;
	}
	return false;
}

static void _onSequenceAck(struct UDSSession* session, uint32_t sequence) {
	int i;
	for (i = 0; i < 3; ++i) {
		if (session->setup[i].used && session->setup[i].sequence == sequence) {
			session->setup[i].acked = true;
		}
	}
}

static void _sendPing(struct UDSSession* session, uint32_t nowMs);
static void _sendClockSync(struct UDSSession* session, uint32_t nowMs);

static void _joined(struct UDSSession* session, uint32_t nowMs) {
	if (session->state == UDS_STATE_JOINED) {
		return;
	}
	session->state = UDS_STATE_JOINED;
	// The host only acknowledges the profile once it carries the assigned index; the setup timer sends it again.
	// Ping and clock sync begin right behind the acknowledgement of the mesh state.
	session->lastPingMs = nowMs;
	session->lastClockSyncMs = nowMs;
	_sendPing(session, nowMs);
	_sendClockSync(session, nowMs);
}

static void _sendPing(struct UDSSession* session, uint32_t nowMs) {
	uint8_t payload[16] = { 0 };
	uint64_t ticks = _ticks(session, nowMs);
	_put32(&payload[8], ticks >> 32);
	_put32(&payload[12], ticks);
	_sendMessage(session, nowMs, UDS_PROTOCOL_PING, 0, 0, UDS_ID_HOST, payload, sizeof(payload));
}

static void _sendClockSync(struct UDSSession* session, uint32_t nowMs) {
	uint8_t payload[16] = { 0 };
	uint64_t ticks = _ticks(session, nowMs);
	_put32(&payload[0], ticks >> 32);
	_put32(&payload[4], ticks);
	_sendMessageEx(session, nowMs, UDS_PROTOCOL_SYSTEM, UDS_SUBTYPE_CLOCK_SYNC, 0, UDS_ID_BROADCAST, payload,
	               sizeof(payload), true);
}

static void _onUnit(struct UDSSession* session, const uint8_t* payload) {
	int32_t index = (int32_t) _get32(&payload[8]);
	int32_t ack = (int32_t) _get32(&payload[12]);
	// payload[24] is the Game Boy byte; the loss bits, role flags and per-run value are not used on this side yet
	if ((int32_t) (ack - session->sendAcked) > 0 && (int32_t) (ack - session->sendNext) <= 0) {
		session->sendAcked = ack;
	}
	if (index == session->recvNext) {
		if (session->recvCount < UDS_RECV_RING) {
			session->recvRing[(session->recvHead + session->recvCount) % UDS_RECV_RING] = payload[24];
			++session->recvCount;
			++session->unitsReceived;
			++session->recvNext;
		}
		// The units that were waiting behind a gap may now be in order.
		for (;;) {
			unsigned slot = (unsigned) session->recvNext % UDS_OUT_OF_ORDER;
			if (!session->recvPending[slot] || session->recvCount >= UDS_RECV_RING) {
				break;
			}
			session->recvPending[slot] = false;
			session->recvRing[(session->recvHead + session->recvCount) % UDS_RECV_RING] =
				session->recvPendingByte[slot];
			++session->recvCount;
			++session->unitsReceived;
			++session->recvNext;
		}
	} else if ((int32_t) (index - session->recvNext) > 0 && (int32_t) (index - session->recvNext) < UDS_OUT_OF_ORDER) {
		unsigned slot = (unsigned) index % UDS_OUT_OF_ORDER;
		session->recvPending[slot] = true;
		session->recvPendingByte[slot] = payload[24];
	} else {
		++session->duplicateUnits;
	}
	session->ackOwed = true;
}

static void _onMessage(struct UDSSession* session, uint32_t nowMs, const struct UDSMessage* m) {
	const uint8_t* p = m->payload;
	switch (m->protocol) {
	case UDS_PROTOCOL_KEEPALIVE:
		break;
	case UDS_PROTOCOL_SETUP:
		if (m->length == 8 && p[0] == 0x05) {
			_onSequenceAck(session, _get32(&p[4]));
		} else if ((m->length == 62 && p[0] == 0x01) || (m->length == 80 && p[0] == 0x02)) {
			uint32_t sequence = _get32(&p[m->length - 4]);
			if (!_handledSequence(session, sequence)) {
				if (m->length == 62) {
					session->hostConstantId = p[1];
					session->hostStationInfo = true;
				} else {
					session->hostProfile = true;
				}
			}
			_sendSequenceAck(session, nowMs, sequence);
		}
		break;
	case UDS_PROTOCOL_SYSTEM:
		if (m->subtype == UDS_SUBTYPE_CLOCK_SYNC && m->length == 16) {
			// The response to our request: our tick back, then the host's session clock. The header clocks already
			// give the same estimate, so nothing more is needed.
		} else if (m->reliable && m->length >= 16 && p[1] == 0x03) {
			int32_t index = (int32_t) _get32(&p[8]);
			if (index == session->systemNext) {
				++session->systemNext;
			}
			_sendRecordAck(session, nowMs, UDS_PROTOCOL_SYSTEM, 1, session->systemNext);
		} else if (m->length == 128 && p[0] == 0x02 && p[1] == 0x02) {
			uint32_t sequence = _get32(&p[m->length - 4]);
			_handledSequence(session, sequence);
			// The mesh state assigns the joiner's index; the acknowledgement of it already carries index 01.
			session->stationIndex = UDS_STATION_JOINER;
			session->piaNumbered = true;
			_sendSequenceAck(session, nowMs, sequence);
			_joined(session, nowMs);
		}
		break;
	case UDS_PROTOCOL_PING:
		if (m->length == 16 && p[3] == 0x00) {
			// The pong goes out once the whole frame has been read: a frame that holds a ping and then the mesh state
			// gets its pong with the newly assigned station index.
			if (session->pendingPongs < 2) {
				memcpy(session->pendingPong[session->pendingPongs], p, 16);
				session->pendingPong[session->pendingPongs][3] = 0x01;
				++session->pendingPongs;
			}
		}
		break;
	case UDS_PROTOCOL_GAME:
		if (m->length == 36 && p[1] == 0x03) {
			_onUnit(session, p);
		} else if (m->length == 24) {
			int32_t ack = (int32_t) _get32(&p[12]);
			if ((int32_t) (ack - session->sendAcked) > 0 && (int32_t) (ack - session->sendNext) <= 0) {
				session->sendAcked = ack;
			}
		}
		break;
	}
}

// Game units

static void _sendUnits(struct UDSSession* session, uint32_t nowMs, int32_t first, int32_t end) {
	while ((int32_t) (end - first) > 0) {
		int32_t count = end - first;
		if (count > UDS_UNIT_WINDOW) {
			count = UDS_UNIT_WINDOW;
		}
		uint8_t frame[UDS_MAX_FRAME_SIZE];
		size_t pos;
		_beginPia(session, nowMs, frame, &pos, false);
		int32_t i;
		for (i = 0; i < count; ++i) {
			int32_t index = first + i;
			unsigned slot = (unsigned) index % UDS_SEND_RING;
			uint8_t payload[36];
			struct UDSUnit unit = { .index = index, .ack = session->recvNext, .byte = session->sendRing[slot].byte };
			memcpy(unit.roleFlags, session->config.gameRoleFlags, 3);
			udsUnitEncode(payload, &unit);
			struct UDSMessage message = {
				.sender = session->stationIndex,
				.destination = UDS_ID_HOST,
				.protocol = UDS_PROTOCOL_GAME,
				.length = sizeof(payload),
				.payload = payload,
			};
			pos = udsFrameAppend(frame, sizeof(frame), pos, &message);
			session->sendRing[slot].sent = true;
			session->sendRing[slot].sentMs = nowMs;
		}
		pos = udsFrameEnd(frame, sizeof(frame), pos);
		if (pos) {
			_sendFrame(session, nowMs, frame, pos);
		}
		session->ackOwed = false; // the units carried the ack
		first += count;
	}
}

// Public

void udsSessionInit(struct UDSSession* session, const struct UDSSessionConfig* config, UDSSendFunction send,
                    void* context) {
	memset(session, 0, sizeof(*session));
	session->config = *config;
	session->send = send;
	session->context = context;
	session->state = UDS_STATE_IDLE;
	session->stationIndex = UDS_STATION_UNASSIGNED;
	session->nextSequence = config->sequenceBase;
	session->systemNext = UDS_FIRST_INDEX;
	session->sendNext = UDS_FIRST_INDEX;
	session->sendAcked = UDS_FIRST_INDEX;
	session->sendFlushed = UDS_FIRST_INDEX;
	session->recvNext = UDS_FIRST_INDEX;
}

void udsSessionStart(struct UDSSession* session, uint32_t nowMs) {
	session->startMs = nowMs;
	session->lastTxMs = nowMs;
	session->lastRxMs = nowMs;
}

static void _beginSetup(struct UDSSession* session, uint32_t nowMs) {
	if (session->state != UDS_STATE_IDLE) {
		return;
	}
	session->state = UDS_STATE_SETUP;
	session->setupStartMs = nowMs;
	_queueStationInfo(session);
	_pumpSetup(session, nowMs);
}

void udsSessionReceive(struct UDSSession* session, uint32_t nowMs, const uint8_t* frame, size_t size) {
	struct UDSFrame parsed;
	if (session->state == UDS_STATE_CLOSED) {
		return;
	}
	if (!udsFrameParse(frame, size, &parsed)) {
		++session->framesRejected;
		return;
	}
	switch (parsed.kind) {
	case UDS_FRAME_HELLO: {
		session->sawHello = true;
		uint8_t reply[UDS_HELLO_REPLY_SIZE];
		udsBuildHelloReply(reply);
		_sendFrame(session, nowMs, reply, sizeof(reply));
		_beginSetup(session, nowMs);
		return;
	}
	case UDS_FRAME_BYE:
		session->state = UDS_STATE_CLOSED;
		return;
	case UDS_FRAME_PIA:
		break;
	default:
		return;
	}
	if (!parsed.tailOk) {
		++session->framesRejected;
		return;
	}
	++session->framesReceived;
	session->lastRxMs = nowMs;
	session->hostClock = parsed.pia.clock;
	session->hostClockMs = nowMs;
	session->haveHostClock = true;
	_beginSetup(session, nowMs);
	size_t pos = 0;
	struct UDSMessage message;
	while (udsMessageNext(&parsed, &pos, &message)) {
		_onMessage(session, nowMs, &message);
	}
	int pong;
	for (pong = 0; pong < session->pendingPongs; ++pong) {
		_sendMessage(session, nowMs, UDS_PROTOCOL_PING, 0, 0, UDS_ID_HOST, session->pendingPong[pong], 16);
	}
	session->pendingPongs = 0;
	if (session->ackOwed && session->state == UDS_STATE_JOINED) {
		_sendRecordAck(session, nowMs, UDS_PROTOCOL_GAME, 0, session->recvNext);
		session->ackOwed = false;
	}
	// What the host has said so far decides what we say next.
	if (session->state == UDS_STATE_SETUP) {
		if (session->hostStationInfo && !session->setup[SLOT_PROFILE].used) {
			_queueProfile(session);
		}
		if (session->hostProfile && !session->setup[SLOT_JOIN_REQUEST].used) {
			_queueJoinRequest(session);
		}
		_pumpSetup(session, nowMs);
	}
}

void udsSessionPoll(struct UDSSession* session, uint32_t nowMs) {
	if (session->state == UDS_STATE_IDLE || session->state == UDS_STATE_CLOSED) {
		return;
	}
	if (_due(nowMs, session->lastRxMs, SILENCE_MS)) {
		session->state = UDS_STATE_CLOSED;
		return;
	}
	if (session->state == UDS_STATE_SETUP) {
		// If the host is slow to answer, send the rest anyway; it re-sends until acknowledged.
		if (_due(nowMs, session->setupStartMs, SETUP_FALLBACK_MS)) {
			if (!session->setup[SLOT_PROFILE].used) {
				_queueProfile(session);
			}
			if (!session->setup[SLOT_JOIN_REQUEST].used) {
				_queueJoinRequest(session);
			}
		}
		_pumpSetup(session, nowMs);
		return;
	}
	// Joined.
	_pumpSetup(session, nowMs);
	if (_due(nowMs, session->lastPingMs, PING_MS)) {
		session->lastPingMs = nowMs;
		_sendPing(session, nowMs);
	}
	if (_due(nowMs, session->lastClockSyncMs, CLOCK_SYNC_MS)) {
		session->lastClockSyncMs = nowMs;
		_sendClockSync(session, nowMs);
	}
	if ((int32_t) (session->sendFlushed - session->sendAcked) > 0 && _due(nowMs, session->lastResendMs, UNIT_RESEND_MS)) {
		unsigned slot = (unsigned) session->sendAcked % UDS_SEND_RING;
		if (_due(nowMs, session->sendRing[slot].sentMs, UNIT_RESEND_MS)) {
			session->lastResendMs = nowMs;
			int32_t end = session->sendFlushed;
			if ((int32_t) (end - session->sendAcked) > 2 * UDS_UNIT_WINDOW) {
				end = session->sendAcked + 2 * UDS_UNIT_WINDOW;
			}
			_sendUnits(session, nowMs, session->sendAcked, end);
		}
	}
	if (_due(nowMs, session->lastTxMs, KEEPALIVE_MS)) {
		_sendMessage(session, nowMs, UDS_PROTOCOL_KEEPALIVE, UDS_SUBTYPE_KEEPALIVE, 0, UDS_ID_HOST, NULL, 0);
	}
}

bool udsSessionQueueUnit(struct UDSSession* session, uint8_t byte) {
	if (session->state != UDS_STATE_JOINED) {
		return false;
	}
	if ((int32_t) (session->sendNext - session->sendAcked) >= UDS_SEND_RING - UDS_UNIT_WINDOW) {
		return false;
	}
	unsigned slot = (unsigned) session->sendNext % UDS_SEND_RING;
	session->sendRing[slot].byte = byte;
	session->sendRing[slot].sent = false;
	++session->sendNext;
	return true;
}

void udsSessionFlush(struct UDSSession* session, uint32_t nowMs) {
	if (session->state != UDS_STATE_JOINED) {
		return;
	}
	if ((int32_t) (session->sendNext - session->sendFlushed) > 0) {
		_sendUnits(session, nowMs, session->sendFlushed, session->sendNext);
		session->sendFlushed = session->sendNext;
	}
}

bool udsSessionPopUnit(struct UDSSession* session, uint8_t* byte) {
	if (!session->recvCount) {
		return false;
	}
	*byte = session->recvRing[session->recvHead];
	session->recvHead = (session->recvHead + 1) % UDS_RECV_RING;
	--session->recvCount;
	return true;
}

size_t udsSessionUnitsWaiting(const struct UDSSession* session) {
	return session->recvCount;
}
