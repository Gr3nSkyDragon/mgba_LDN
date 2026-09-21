/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_RFU_H
#define GBA_SIO_RFU_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/log.h>
#include <mgba/core/timing.h>
#include <mgba/gba/interface.h>
#include <mgba-util/threading.h>

mLOG_DECLARE_CATEGORY(GBA_RFU);

/*
 * Emulation of the GBA Wireless Adapter (AGB-015, "RFU").
 *
 * The driver implements the adapter's side of the link port: the reset line, the NINTENDO handshake, the
 * 0x9966 command protocol and the clock reversal used by the wait commands. Everything that touches an actual
 * network sits behind struct GBASIORFUBackend so the same driver can be attached to a local loopback or a bridge to
 * a real Switch (LDN). With no backend (NULL) the adapter is present but nothing else can ever be reached.
 *
 * Threading: the driver runs on the emulation thread. A backend may run threads of its own; it reports what
 * happened through the GBASIORFU*Received/Result/... functions below, which are safe to call from any thread
 * and are queued until the emulation thread next ticks. Calls the driver makes *into* the backend happen on
 * the emulation thread and must not block.
 */

enum {
	RFU_MAX_CLIENTS = 4,
	RFU_MAX_PEERS = 16,
	RFU_BROADCAST_WORDS = 6,
	RFU_CLIENT_TX_MAX = 16,
	RFU_HOST_TX_MAX = 90,
	RFU_PACKET_MAX = 96,
	RFU_QUEUE_DEPTH = 32,
	RFU_EVENT_QUEUE = 64,
	RFU_BUFFER_WORDS = 255,
};

enum GBASIORFULink {
	RFU_LINK_RESET,
	RFU_LINK_HANDSHAKE,
	RFU_LINK_COMMAND,
	RFU_LINK_COMMAND_DATA,
	RFU_LINK_RESPONSE_ACK,
	RFU_LINK_RESPONSE_DATA,
	RFU_LINK_ERROR_HEADER,
	RFU_LINK_ERROR_CODE,
	RFU_LINK_WAIT_EVENT,
	RFU_LINK_WAIT_RESPONSE,
};

enum GBASIORFUState {
	RFU_STATE_IDLE,
	RFU_STATE_HOST,
	RFU_STATE_CONNECTING,
	RFU_STATE_CLIENT,
};

enum GBASIORFUEventType {
	RFU_EVENT_BROADCAST,
	RFU_EVENT_CONNECT_RESULT,
	RFU_EVENT_CLIENT_JOINED,
	RFU_EVENT_DATA,
	RFU_EVENT_DISCONNECTED,
	RFU_EVENT_CONNECT_REQUEST,
};

struct GBASIORFU;

struct GBASIORFUBackend {
	bool (*init)(struct GBASIORFUBackend*, struct GBASIORFU*);
	void (*deinit)(struct GBASIORFUBackend*);

	// The game reset the adapter; forget everything.
	void (*reset)(struct GBASIORFUBackend*);
	// Called once per emulated frame (~59.7 Hz) on the emulation thread. Optional.
	void (*frame)(struct GBASIORFUBackend*);
	// Called on the emulation thread every few hundred to few thousand emulated cycles while the adapter is in use,
	// for backends that would rather take in network traffic there than on a thread of their own. Optional.
	void (*poll)(struct GBASIORFUBackend*);

	// Host role: the 24 bytes of game/user data to advertise, and start/stop of advertising.
	void (*setBroadcast)(struct GBASIORFUBackend*, const uint32_t data[RFU_BROADCAST_WORDS]);
	void (*hostStart)(struct GBASIORFUBackend*, uint16_t deviceId);
	void (*hostStop)(struct GBASIORFUBackend*);

	// Host role: another adapter asked to join (see GBASIORFUConnectRequested); the adapter has decided. When accepted,
	// slot is the client's slot number.
	void (*connectReply)(struct GBASIORFUBackend*, uint16_t clientId, bool accepted, unsigned slot);

	// Client role: scan for hosts (results arrive as GBASIORFUBroadcastReceived), then join one.
	void (*searchStart)(struct GBASIORFUBackend*);
	void (*searchStop)(struct GBASIORFUBackend*);
	void (*connect)(struct GBASIORFUBackend*, uint16_t deviceId);

	// Client: slotMask is ignored, leave the host. Host: bit N drops client slot N.
	void (*disconnect)(struct GBASIORFUBackend*, unsigned slotMask);

	// Payload of a SendData command. Host: broadcast to all clients. Client: send to the host.
	void (*sendData)(struct GBASIORFUBackend*, const uint8_t* data, size_t length);
};

struct GBASIORFUPacket {
	uint8_t length;
	uint8_t data[RFU_PACKET_MAX];
};

struct GBASIORFUPeer {
	bool valid;
	uint8_t ttl;
	uint16_t deviceId;
	uint8_t nextSlot; // the next free client slot of that host, 0xFF when it is full
	uint32_t data[RFU_BROADCAST_WORDS];
};

struct GBASIORFUEvent {
	enum GBASIORFUEventType type;
	uint16_t deviceId;
	int slot;
	bool accepted;
	uint8_t length;
	uint32_t words[RFU_BROADCAST_WORDS];
	uint8_t data[RFU_PACKET_MAX];
};

struct GBASIORFU {
	struct GBASIODriver d;
	struct GBASIORFUBackend* backend;

	struct mTimingEvent transferEvent;
	struct mTimingEvent tickEvent;
	uint32_t lastTick;
	uint32_t frameAccumulator;

	enum GBASIOMode mode;

	// Link layer (what the GBA sees over SIO)
	enum GBASIORFULink link;
	uint32_t previousWord;
	uint32_t pendingReply;
	uint32_t buffer[RFU_BUFFER_WORDS];
	unsigned count;
	unsigned length;
	uint8_t command;
	uint8_t errorCode;
	int32_t timeoutCycles;
	int32_t retransmitCycles;

	// Adapter configuration set by the game
	uint8_t timeout;
	uint8_t retransmits;
	uint8_t maxClients;
	bool hostClosed;
	bool searching;

	// Adapter (radio) layer
	enum GBASIORFUState state;
	uint32_t broadcastData[RFU_BROADCAST_WORDS];
	struct GBASIORFUPeer peers[RFU_MAX_PEERS];

	struct {
		uint16_t deviceId;
		unsigned broadcastCountdown;
		struct {
			uint16_t deviceId;
			unsigned ttl;
			bool reported; // already returned by a PollConnections command
			struct GBASIORFUPacket packets[RFU_QUEUE_DEPTH];
		} clients[RFU_MAX_CLIENTS];
	} host;

	struct {
		uint16_t deviceId;
		unsigned slot;
		struct GBASIORFUPacket packets[RFU_QUEUE_DEPTH];
	} client;

	struct {
		uint32_t words[23];
		unsigned length;
	} tx;

	uint16_t idSeed;

	// Events from the backend, possibly from another thread
	Mutex eventMutex;
	struct GBASIORFUEvent events[RFU_EVENT_QUEUE];
	unsigned eventHead;
	unsigned eventTail;

	FILE* trace;
	uint32_t snapData;
	uint32_t snapState;
	unsigned pcSamples;
};

void GBASIORFUCreate(struct GBASIORFU*, struct GBASIORFUBackend*);
void GBASIORFUDestroy(struct GBASIORFU*);

// Send the protocol trace to a file (used for development). Pass NULL to stop.
void GBASIORFUSetTraceFile(struct GBASIORFU*, const char* path);

// Backend -> driver notifications. Thread safe.
void GBASIORFUBroadcastReceived(struct GBASIORFU*, uint16_t deviceId, uint8_t nextSlot, const uint32_t data[RFU_BROADCAST_WORDS]);
void GBASIORFUConnectResult(struct GBASIORFU*, bool accepted, uint16_t deviceId, unsigned slot);
void GBASIORFUClientJoined(struct GBASIORFU*, unsigned slot, uint16_t deviceId);
void GBASIORFUDataReceived(struct GBASIORFU*, unsigned slot, const uint8_t* data, size_t length);
void GBASIORFUDisconnected(struct GBASIORFU*, int slot);
// Host role: a client wants to join. The adapter decides (free slot, still open) and answers through connectReply.
void GBASIORFUConnectRequested(struct GBASIORFU*, uint16_t clientId);

// The client slot the next joiner would get, 0xFF when the host is full or closed. Call on the emulation thread.
unsigned GBASIORFUHostNextSlot(const struct GBASIORFU*);

// Write a line to the protocol trace (if one is open). Emulation thread only.
void GBASIORFUTrace(struct GBASIORFU*, const char* format, ...);

CXX_GUARD_END

#endif
