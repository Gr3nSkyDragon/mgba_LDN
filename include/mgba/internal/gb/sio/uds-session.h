/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_SESSION_H
#define GB_SIO_UDS_SESSION_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gb/sio/uds-pia.h>

/*
 * The joiner ("client") side of a Pia session with a 3DS Virtual Console Game Boy title that hosts. It sits between a
 * transport that moves UDS channel-243 payloads (the legacy ldnd, an ESP32 board, or a test harness) and a consumer that
 * wants Game Boy serial bytes (the not-yet-written Game Boy link driver).
 *
 * The session never looks at a clock or a socket itself: every entry point is given the time in milliseconds, and frames
 * leave through the send callback. That keeps it deterministic and lets the test replay a capture.
 *
 * Behaviour is taken from the retail-hosted captures (docs/wiki/vc_link.md): the 2DS as joiner in the passive capture of
 * 2026-10-04, and Azahar as joiner in the three trades of 2026-10-03.
 *
 *   hello (host, 0.5 s)  ->  hello reply
 *   station info, profile, join request: sequenced setup messages, each acknowledged by `05 00 00 00 <seq>`
 *   mesh state (host)    ->  the joiner's station index becomes 01; ping, clock sync and keep-alive start
 *   system data (host, every 10 s) -> system ack
 *   game stream: one reliable "unit" per Game Boy serial exchange, indexes from -2001
 */

#define UDS_NAME_CHARS 10
#define UDS_UNIT_WINDOW 25 // units in one 1440-byte frame, the largest the VC sends
#define UDS_FIRST_INDEX (-2001)
#define UDS_SEND_RING 1024
#define UDS_RECV_RING 256
#define UDS_OUT_OF_ORDER 64

typedef void (*UDSSendFunction)(void* context, const uint8_t* frame, size_t size);

struct UDSSessionConfig {
	uint8_t connectionId; // Pia connection id and "station constant id": random per session, 2..255
	uint32_t sequenceBase; // first setup sequence value: the low 32 bits of the 3DS tick clock at session start
	uint64_t tickBase; // the tick clock (268.11 MHz) at now = 0
	uint8_t appData[16]; // the host's beacon application data; bytes 4..7 become the profile's shared value
	uint16_t name[UDS_NAME_CHARS]; // player name, UTF-16, zero padded
	uint8_t profileRole; // the profile's role byte: 06 for a joiner
	uint8_t gameRoleFlags[3]; // game unit offset 25..27: 01 01 00 for a joiner
};

enum UDSSessionState {
	UDS_STATE_IDLE, // started, nothing heard from the host yet
	UDS_STATE_SETUP, // exchanging setup messages
	UDS_STATE_JOINED, // mesh state received: the game stream may start
	UDS_STATE_CLOSED, // the host said bye or went silent
};

struct UDSSetupMessage {
	bool used;
	bool acked;
	uint8_t protocol;
	uint16_t length;
	uint32_t sequence;
	uint32_t lastSent;
	bool everSent;
	uint8_t payload[96];
};

struct UDSSession {
	struct UDSSessionConfig config;
	UDSSendFunction send;
	void* context;

	enum UDSSessionState state;
	uint32_t startMs;
	uint32_t setupStartMs;
	uint32_t lastTxMs;
	uint32_t lastRxMs;
	uint16_t packetId;
	uint8_t stationIndex; // FD until the mesh state, then 01
	uint8_t hostConstantId;
	bool sawHello;

	uint32_t nextSequence;
	struct UDSSetupMessage setup[4]; // station info, profile, join request, spare
	uint32_t hostSequences[8]; // setup sequence values already handled
	int hostSequenceCount;
	bool hostStationInfo;
	bool hostProfile;
	bool hostLeaving; // the host sent its end-of-session message on the system stream (a 36-byte record, seen when its game leaves the room)

	uint8_t pendingPong[2][16]; // pongs owed for the pings in the frame being processed; sent once the frame is done
	int pendingPongs;
	uint32_t lastPingMs;
	uint32_t lastClockSyncMs;
	bool pinged;
	uint16_t hostClock; // the host's session clock from the header of its last Pia frame ...
	uint32_t hostClockMs; // ... and when it arrived; the header's second clock estimates the host's clock from these
	bool haveHostClock;
	bool piaNumbered; // packet ids and the station constant id start with the host's mesh state; setup frames use 0 and 01

	// reliable system stream (host to us only: system data in, system ack out)
	int32_t systemNext;

	// reliable game stream
	struct {
		uint8_t byte;
		bool sent;
		uint32_t sentMs;
	} sendRing[UDS_SEND_RING];
	int32_t sendNext; // index of the next unit to be queued
	int32_t sendAcked; // the peer has everything below this
	int32_t sendFlushed; // units below this have been transmitted at least once
	uint32_t lastResendMs;

	int32_t recvNext; // the next in-order unit expected; also the ack index we send
	bool recvPending[UDS_OUT_OF_ORDER];
	uint8_t recvPendingByte[UDS_OUT_OF_ORDER];
	uint8_t recvRing[UDS_RECV_RING];
	unsigned recvHead;
	unsigned recvCount;
	bool ackOwed;

	// counters for the status line and the tests
	unsigned framesSent;
	unsigned framesReceived;
	unsigned framesRejected;
	unsigned unitsReceived;
	unsigned duplicateUnits;
};

void udsSessionInit(struct UDSSession* session, const struct UDSSessionConfig* config, UDSSendFunction send,
                    void* context);

// Begins the session. The joiner waits for the host's hello (or for any Pia frame) before it says anything.
void udsSessionStart(struct UDSSession* session, uint32_t nowMs);

// A UDS channel-243 payload from the transport.
void udsSessionReceive(struct UDSSession* session, uint32_t nowMs, const uint8_t* frame, size_t size);

// Timers: setup re-sends, ping, clock sync, keep-alive, game-unit re-sends, the 10 s silence timeout. Call often (every
// few milliseconds is fine; it does nothing until something is due).
void udsSessionPoll(struct UDSSession* session, uint32_t nowMs);
// Our own end-of-session record on the system stream, the answer to the host's (Gen 2: the host's VC waits about five seconds for it).
void udsSessionSendLeave(struct UDSSession* session, uint32_t nowMs);

// Queues one Game Boy serial byte as the next unit. False when the unacknowledged window is full. Nothing is
// transmitted until udsSessionFlush (or a poll re-send), so a block can be queued and sent in full frames.
bool udsSessionQueueUnit(struct UDSSession* session, uint8_t byte);
void udsSessionFlush(struct UDSSession* session, uint32_t nowMs);

// Units received from the host, in order. False when none is waiting.
bool udsSessionPopUnit(struct UDSSession* session, uint8_t* byte);
bool udsSessionPeekUnit(const struct UDSSession* session, uint8_t* byte); // the next unit, left in place
size_t udsSessionUnitsWaiting(const struct UDSSession* session);

// The 36-byte payload of a game-stream unit (a reliable-stream data record carrying one Game Boy serial byte).
struct UDSUnit {
	int32_t index;
	int32_t ack;
	uint8_t lossBits[8]; // meaning unknown; zero unless the peer is recovering from loss
	uint8_t byte;
	uint8_t roleFlags[3];
	uint32_t value; // the per-run 32-bit value the host stamps on some units; zero from the joiner
};
void udsUnitEncode(uint8_t payload[36], const struct UDSUnit* unit);
bool udsUnitDecode(const uint8_t* payload, size_t size, struct UDSUnit* unit);

// The profile's shared 32-bit value for a host beacon's application data (little-endian bytes 4..7).
uint32_t udsProfileValue(const uint8_t appData[16]);

CXX_GUARD_END

#endif
