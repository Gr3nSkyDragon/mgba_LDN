/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/rfu.h>

#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio.h>
#include <mgba/internal/gba/video.h>

#include <stdarg.h>
#include <time.h>

mLOG_DEFINE_CATEGORY(GBA_RFU, "GBA Wireless Adapter", "gba.rfu");

/*
 * Protocol references (behaviour, not code):
 *  - https://github.com/afska/gba-link-connection/blob/master/docs/wireless_adapter.md
 *  - https://blog.kuiper.dev/gba-wireless-adapter
 * Fields we could not confirm from documentation follow the behaviour other emulators have shown to work with
 * Pokemon; those spots are marked "(unverified)" and are the first place to look when a game misbehaves.
 */

enum {
	RFU_CMD_HELLO = 0x10,
	RFU_CMD_SIGNAL_LEVEL = 0x11,
	RFU_CMD_VERSION_STATUS = 0x12,
	RFU_CMD_SYSTEM_STATUS = 0x13,
	RFU_CMD_SLOT_STATUS = 0x14,
	RFU_CMD_CONFIG_STATUS = 0x15,
	RFU_CMD_BROADCAST = 0x16,
	RFU_CMD_SETUP = 0x17,
	RFU_CMD_START_HOST = 0x19,
	RFU_CMD_POLL_CONNECTIONS = 0x1A,
	RFU_CMD_END_HOST = 0x1B,
	RFU_CMD_BROADCAST_READ_START = 0x1C,
	RFU_CMD_BROADCAST_READ_POLL = 0x1D,
	RFU_CMD_BROADCAST_READ_END = 0x1E,
	RFU_CMD_CONNECT = 0x1F,
	RFU_CMD_IS_CONNECTION_COMPLETE = 0x20,
	RFU_CMD_FINISH_CONNECTION = 0x21,
	RFU_CMD_SEND_DATA = 0x24,
	RFU_CMD_SEND_DATA_WAIT = 0x25,
	RFU_CMD_RECEIVE_DATA = 0x26,
	RFU_CMD_WAIT = 0x27,
	RFU_CMD_DISCONNECT = 0x30,
	RFU_CMD_RETRANSMIT_WAIT = 0x37,
	RFU_CMD_BYE = 0x3D,

	// Commands the adapter sends to the game while it owns the clock
	RFU_NOTIFY_TIMEOUT = 0x27,
	RFU_NOTIFY_DATA = 0x28,
	RFU_NOTIFY_DISCONNECT = 0x29,
};

enum {
	RFU_ERROR_WRONG_STATE = 1,
	RFU_ERROR_INVALID_COMMAND = 2,
};

enum {
	RFU_CONNECTING = 0x01000000,
	RFU_CONNECTION_FAILED = 0x02000000,

	RFU_DEFAULT_TIMEOUT = 32, // frames
	RFU_DEFAULT_RETRANSMITS = 4,

	RFU_BROADCAST_INTERVAL = 30, // frames
	RFU_PEER_TTL = 255,          // frames
	RFU_CLIENT_TTL = 240,        // frames

	RFU_WAIT_POLL_CYCLES = 1024,
	RFU_IDLE_POLL_CYCLES = 16384,
	RFU_ADAPTER_FRAME = GBA_ARM7TDMI_FREQUENCY / 60,
};

static bool GBASIORFUInit(struct GBASIODriver* driver);
static void GBASIORFUDeinit(struct GBASIODriver* driver);
static void GBASIORFUReset(struct GBASIODriver* driver);
static void GBASIORFUSetMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static bool GBASIORFUHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static int GBASIORFUConnectedDevices(struct GBASIODriver* driver);
static uint16_t GBASIORFUWriteSIOCNT(struct GBASIODriver* driver, uint16_t value);
static uint16_t GBASIORFUWriteRCNT(struct GBASIODriver* driver, uint16_t value);
static bool GBASIORFUStart(struct GBASIODriver* driver);

static void _transferFinished(struct mTiming* timing, void* context, uint32_t cyclesLate);
static void _tick(struct mTiming* timing, void* context, uint32_t cyclesLate);

static void _traceV(struct GBASIORFU* rfu, const char* format, va_list args) {
	fprintf(rfu->trace, "%10u ", rfu->d.p ? mTimingCurrentTime(&rfu->d.p->p->timing) : 0);
	vfprintf(rfu->trace, format, args);
	fputc('\n', rfu->trace);
}

static void _traceBytes(struct GBASIORFU* rfu, const char* label, const uint8_t* data, unsigned length) {
	if (!rfu->trace) {
		return;
	}
	char text[RFU_PACKET_MAX * 2 + 1];
	if (length > RFU_PACKET_MAX) {
		length = RFU_PACKET_MAX;
	}
	for (unsigned i = 0; i < length; ++i) {
		snprintf(&text[i * 2], 3, "%02X", data[i]);
	}
	text[length * 2] = 0;
	fprintf(rfu->trace, "%10u %s %s\n", rfu->d.p ? mTimingCurrentTime(&rfu->d.p->p->timing) : 0, label, text);
}

static void _trace(struct GBASIORFU* rfu, const char* format, ...) {
	if (!rfu->trace) {
		return;
	}
	va_list args;
	va_start(args, format);
	_traceV(rfu, format, args);
	va_end(args);
}

void GBASIORFUTrace(struct GBASIORFU* rfu, const char* format, ...) {
	if (!rfu->trace) {
		return;
	}
	va_list args;
	va_start(args, format);
	_traceV(rfu, format, args);
	va_end(args);
}

// Used when no backend is attached: the adapter itself works, but no other adapter or host can ever be reached.
static struct GBASIORFUBackend sNoBackend;

void GBASIORFUCreate(struct GBASIORFU* rfu, struct GBASIORFUBackend* backend) {
	memset(rfu, 0, sizeof(*rfu));
	rfu->backend = backend ? backend : &sNoBackend;
	rfu->d.init = GBASIORFUInit;
	rfu->d.deinit = GBASIORFUDeinit;
	rfu->d.reset = GBASIORFUReset;
	rfu->d.setMode = GBASIORFUSetMode;
	rfu->d.handlesMode = GBASIORFUHandlesMode;
	rfu->d.connectedDevices = GBASIORFUConnectedDevices;
	rfu->d.writeSIOCNT = GBASIORFUWriteSIOCNT;
	rfu->d.writeRCNT = GBASIORFUWriteRCNT;
	rfu->d.start = GBASIORFUStart;

	rfu->transferEvent.context = rfu;
	rfu->transferEvent.name = "GBA RFU Transfer";
	rfu->transferEvent.callback = _transferFinished;
	rfu->transferEvent.priority = 0x80;

	rfu->tickEvent.context = rfu;
	rfu->tickEvent.name = "GBA RFU Tick";
	rfu->tickEvent.callback = _tick;
	rfu->tickEvent.priority = 0x81;

	MutexInit(&rfu->eventMutex);
}

void GBASIORFUDestroy(struct GBASIORFU* rfu) {
	GBASIORFUSetTraceFile(rfu, NULL);
	MutexDeinit(&rfu->eventMutex);
}

void GBASIORFUSetTraceFile(struct GBASIORFU* rfu, const char* path) {
	if (rfu->trace) {
		fclose(rfu->trace);
		rfu->trace = NULL;
	}
	if (path && path[0]) {
		rfu->trace = fopen(path, "w");
		if (!rfu->trace) {
			mLOG(GBA_RFU, ERROR, "Could not open trace file %s", path);
		}
	}
}

static uint16_t _newDeviceId(struct GBASIORFU* rfu) {
	// Adapters pick a random non-zero id for every session; xorshift is plenty for that.
	uint32_t x = rfu->idSeed ? rfu->idSeed : (uint32_t) time(NULL) ^ 0x9E3779B9u;
	uint16_t id = 0;
	while (!id) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		id = (uint16_t) (x ^ (x >> 16));
	}
	rfu->idSeed = x;
	return id;
}

// ---------------------------------------------------------------------------------------------------------------
// Backend event queue
// ---------------------------------------------------------------------------------------------------------------

static struct GBASIORFUEvent* _queueEvent(struct GBASIORFU* rfu) {
	// Caller holds the mutex. Drops the event when the queue is full: the game will retry or time out.
	unsigned next = (rfu->eventHead + 1) % RFU_EVENT_QUEUE;
	if (next == rfu->eventTail) {
		return NULL;
	}
	struct GBASIORFUEvent* event = &rfu->events[rfu->eventHead];
	memset(event, 0, sizeof(*event));
	rfu->eventHead = next;
	return event;
}

void GBASIORFUBroadcastReceived(struct GBASIORFU* rfu, uint16_t deviceId, uint8_t nextSlot, const uint32_t data[RFU_BROADCAST_WORDS]) {
	MutexLock(&rfu->eventMutex);
	struct GBASIORFUEvent* event = _queueEvent(rfu);
	if (event) {
		event->type = RFU_EVENT_BROADCAST;
		event->deviceId = deviceId;
		event->slot = nextSlot;
		memcpy(event->words, data, sizeof(event->words));
	}
	MutexUnlock(&rfu->eventMutex);
}

void GBASIORFUConnectResult(struct GBASIORFU* rfu, bool accepted, uint16_t deviceId, unsigned slot) {
	MutexLock(&rfu->eventMutex);
	struct GBASIORFUEvent* event = _queueEvent(rfu);
	if (event) {
		event->type = RFU_EVENT_CONNECT_RESULT;
		event->accepted = accepted;
		event->deviceId = deviceId;
		event->slot = slot;
	}
	MutexUnlock(&rfu->eventMutex);
}

void GBASIORFUClientJoined(struct GBASIORFU* rfu, unsigned slot, uint16_t deviceId) {
	MutexLock(&rfu->eventMutex);
	struct GBASIORFUEvent* event = _queueEvent(rfu);
	if (event) {
		event->type = RFU_EVENT_CLIENT_JOINED;
		event->slot = slot;
		event->deviceId = deviceId;
	}
	MutexUnlock(&rfu->eventMutex);
}

void GBASIORFUDataReceived(struct GBASIORFU* rfu, unsigned slot, const uint8_t* data, size_t length) {
	if (length > RFU_PACKET_MAX) {
		length = RFU_PACKET_MAX;
	}
	MutexLock(&rfu->eventMutex);
	struct GBASIORFUEvent* event = _queueEvent(rfu);
	if (event) {
		event->type = RFU_EVENT_DATA;
		event->slot = slot;
		event->length = length;
		memcpy(event->data, data, length);
	}
	MutexUnlock(&rfu->eventMutex);
}

void GBASIORFUDisconnected(struct GBASIORFU* rfu, int slot) {
	MutexLock(&rfu->eventMutex);
	struct GBASIORFUEvent* event = _queueEvent(rfu);
	if (event) {
		event->type = RFU_EVENT_DISCONNECTED;
		event->slot = slot;
	}
	MutexUnlock(&rfu->eventMutex);
}

void GBASIORFUConnectRequested(struct GBASIORFU* rfu, uint16_t clientId) {
	MutexLock(&rfu->eventMutex);
	struct GBASIORFUEvent* event = _queueEvent(rfu);
	if (event) {
		event->type = RFU_EVENT_CONNECT_REQUEST;
		event->deviceId = clientId;
	}
	MutexUnlock(&rfu->eventMutex);
}

bool GBASIORFUPopEvent(struct GBASIORFU* rfu, struct GBASIORFUEvent* out) {
	MutexLock(&rfu->eventMutex);
	if (rfu->eventHead == rfu->eventTail) {
		MutexUnlock(&rfu->eventMutex);
		return false;
	}
	*out = rfu->events[rfu->eventTail];
	rfu->eventTail = (rfu->eventTail + 1) % RFU_EVENT_QUEUE;
	MutexUnlock(&rfu->eventMutex);
	return true;
}

static void _pushPacket(struct GBASIORFU* rfu, struct GBASIORFUPacket packets[RFU_QUEUE_DEPTH], const uint8_t* data, unsigned length, unsigned maxLength) {
	if (length > maxLength) {
		length = maxLength;
	}
	for (unsigned i = 0; i < RFU_QUEUE_DEPTH; ++i) {
		if (!packets[i].length) {
			memcpy(packets[i].data, data, length);
			packets[i].length = length;
			return;
		}
	}
	// Queue full: the newest data is dropped, as if the adapter's buffer overflowed. (Games number their packets and
	// treat a gap as a broken link, so this is worth knowing about.)
	_trace(rfu, "DROP   packet queue full, %u bytes lost", length);
}

static void _popPacket(struct GBASIORFUPacket packets[RFU_QUEUE_DEPTH]) {
	memmove(&packets[0], &packets[1], sizeof(packets[0]) * (RFU_QUEUE_DEPTH - 1));
	memset(&packets[RFU_QUEUE_DEPTH - 1], 0, sizeof(packets[0]));
}

static unsigned _nextFreeSlot(const struct GBASIORFU* rfu);

static void _processEvent(struct GBASIORFU* rfu, const struct GBASIORFUEvent* event) {
	unsigned i;
	switch (event->type) {
	case RFU_EVENT_BROADCAST: {
		struct GBASIORFUPeer* slot = NULL;
		for (i = 0; i < RFU_MAX_PEERS; ++i) {
			if (rfu->peers[i].valid && rfu->peers[i].deviceId == event->deviceId) {
				slot = &rfu->peers[i];
				break;
			}
			if (!slot && !rfu->peers[i].valid) {
				slot = &rfu->peers[i];
			}
		}
		if (slot) {
			if (!slot->valid) {
				_trace(rfu, "PEER   new host %04X", event->deviceId);
			}
			slot->valid = true;
			slot->ttl = RFU_PEER_TTL;
			slot->deviceId = event->deviceId;
			slot->nextSlot = event->slot;
			memcpy(slot->data, event->words, sizeof(slot->data));
		}
		break;
	}
	case RFU_EVENT_CONNECT_RESULT:
		if (rfu->state != RFU_STATE_CONNECTING) {
			// A backend's async connect result that arrives after the game already gave up (FINISH_CONNECTION) is dropped.
			_trace(rfu, "EVENT  connect result DROPPED (state=%d, not CONNECTING) accepted=%s dev=%04X", rfu->state,
			       event->accepted ? "true" : "false", event->deviceId);
			break;
		}
		_trace(rfu, "EVENT  connect %s dev=%04X slot=%d", event->accepted ? "accepted" : "refused", event->deviceId, event->slot);
		if (event->accepted) {
			memset(&rfu->client, 0, sizeof(rfu->client));
			rfu->client.deviceId = event->deviceId;
			rfu->client.slot = event->slot & 3;
			rfu->state = RFU_STATE_CLIENT;
		} else {
			rfu->state = RFU_STATE_IDLE;
		}
		break;
	case RFU_EVENT_CLIENT_JOINED:
		if (rfu->state == RFU_STATE_HOST && event->slot >= 0 && event->slot < RFU_MAX_CLIENTS) {
			_trace(rfu, "EVENT  client joined slot=%d dev=%04X", event->slot, event->deviceId);
			memset(&rfu->host.clients[event->slot], 0, sizeof(rfu->host.clients[0]));
			rfu->host.clients[event->slot].deviceId = event->deviceId;
			rfu->host.clients[event->slot].reported = false;
		}
		break;
	case RFU_EVENT_CONNECT_REQUEST: {
		// The adapter, not the network, decides whether a joiner is let in and which slot it gets.
		int slot = -1;
		if (rfu->state == RFU_STATE_HOST) {
			for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
				if (rfu->host.clients[i].deviceId == event->deviceId) {
					slot = i;
				}
			}
			if (slot < 0) {
				unsigned next = _nextFreeSlot(rfu);
				slot = next == 0xFF ? -1 : (int) next;
			}
		}
		_trace(rfu, "EVENT  join request dev=%04X -> %s slot=%d", event->deviceId, slot >= 0 ? "accept" : "refuse", slot);
		if (slot >= 0 && rfu->host.clients[slot].deviceId != event->deviceId) {
			memset(&rfu->host.clients[slot], 0, sizeof(rfu->host.clients[0]));
			rfu->host.clients[slot].deviceId = event->deviceId;
		}
		if (rfu->backend->connectReply) {
			rfu->backend->connectReply(rfu->backend, event->deviceId, slot >= 0, slot >= 0 ? slot : 0);
		}
		break;
	}
	case RFU_EVENT_DATA:
		if (rfu->state == RFU_STATE_CLIENT) {
			_pushPacket(rfu, rfu->client.packets, event->data, event->length, RFU_PACKET_MAX);
		} else if (rfu->state == RFU_STATE_HOST && event->slot >= 0 && event->slot < RFU_MAX_CLIENTS) {
			if (rfu->host.clients[event->slot].deviceId) {
				rfu->host.clients[event->slot].ttl = 0;
				_pushPacket(rfu, rfu->host.clients[event->slot].packets, event->data, event->length, RFU_CLIENT_TX_MAX);
			}
		}
		break;
	case RFU_EVENT_DISCONNECTED:
		_trace(rfu, "EVENT  disconnected slot=%d", event->slot);
		if (rfu->state == RFU_STATE_CLIENT || rfu->state == RFU_STATE_CONNECTING) {
			memset(&rfu->client, 0, sizeof(rfu->client));
			rfu->state = RFU_STATE_IDLE;
		} else if (rfu->state == RFU_STATE_HOST) {
			if (event->slot >= 0 && event->slot < RFU_MAX_CLIENTS) {
				memset(&rfu->host.clients[event->slot], 0, sizeof(rfu->host.clients[0]));
			}
		}
		break;
	}
}

static void _drainEvents(struct GBASIORFU* rfu) {
	while (true) {
		struct GBASIORFUEvent event;
		MutexLock(&rfu->eventMutex);
		if (rfu->eventTail == rfu->eventHead) {
			MutexUnlock(&rfu->eventMutex);
			return;
		}
		event = rfu->events[rfu->eventTail];
		rfu->eventTail = (rfu->eventTail + 1) % RFU_EVENT_QUEUE;
		MutexUnlock(&rfu->eventMutex);
		_processEvent(rfu, &event);
	}
}

unsigned GBASIORFUHostNextSlot(const struct GBASIORFU* rfu) {
	return rfu->state == RFU_STATE_HOST ? _nextFreeSlot(rfu) : 0xFF;
}

// ---------------------------------------------------------------------------------------------------------------
// Adapter commands
// ---------------------------------------------------------------------------------------------------------------

static unsigned _connectedClients(const struct GBASIORFU* rfu) {
	unsigned count = 0;
	for (unsigned i = 0; i < RFU_MAX_CLIENTS; ++i) {
		if (rfu->host.clients[i].deviceId) {
			++count;
		}
	}
	return count;
}

static void _transmit(struct GBASIORFU* rfu) {
	uint8_t bytes[RFU_HOST_TX_MAX + 4];
	unsigned length = rfu->tx.length;
	if (length > sizeof(rfu->tx.words)) {
		return;
	}
	for (unsigned i = 0; i < length; ++i) {
		bytes[i] = rfu->tx.words[i / 4] >> (8 * (i & 3));
	}
	if (rfu->state == RFU_STATE_HOST) {
		if (length <= RFU_HOST_TX_MAX && rfu->backend->sendData) {
			_trace(rfu, "SEND   host -> clients %u bytes", length);
			_traceBytes(rfu, "SENDB ", bytes, length);
			rfu->backend->sendData(rfu->backend, bytes, length);
		}
	} else if (rfu->state == RFU_STATE_CLIENT) {
		if (length <= RFU_CLIENT_TX_MAX && rfu->backend->sendData) {
			_trace(rfu, "SEND   client -> host %u bytes", length);
			_traceBytes(rfu, "SENDB ", bytes, length);
			rfu->backend->sendData(rfu->backend, bytes, length);
		}
	}
}

// The next client slot a new client would get, or 0xFF when the host cannot take another one.
static unsigned _nextFreeSlot(const struct GBASIORFU* rfu) {
	if (rfu->hostClosed) {
		return 0xFF;
	}
	for (unsigned i = 0; i < rfu->maxClients && i < RFU_MAX_CLIENTS; ++i) {
		if (!rfu->host.clients[i].deviceId) {
			return i;
		}
	}
	return 0xFF;
}

// Returns the number of response words, or a negative adapter error code.
static int _processCommand(struct GBASIORFU* rfu) {
	unsigned i, j;
	unsigned count = 0;
	uint32_t* buffer = rfu->buffer;

	_trace(rfu, "CMD    %02X len=%u state=%d", rfu->command, rfu->length, rfu->state);

	switch (rfu->command) {
	case RFU_CMD_HELLO:
	case RFU_CMD_BYE:
		return 0;

	case RFU_CMD_SETUP:
		rfu->timeout = buffer[0];
		rfu->retransmits = buffer[0] >> 8;
		// Bits 16-17 hold 5 minus the number of adapters allowed: 0 = 1 host + 4 clients ... 3 = 1 host + 1 client.
		rfu->maxClients = RFU_MAX_CLIENTS - ((buffer[0] >> 16) & 3);
		_trace(rfu, "SETUP  timeout=%u frames retransmits=%u maxClients=%u", rfu->timeout, rfu->retransmits, rfu->maxClients);
		return 0;

	case RFU_CMD_VERSION_STATUS:
		buffer[0] = 0x00830117; // (unverified) firmware identifier
		return 1;

	case RFU_CMD_SYSTEM_STATUS:
		// Bits 0-15 device id, bits 16-19 the client's slot bit, bits 24+ adapter state. State values as read back by
		// afska's library from a real adapter: 1 hosting but closed, 2 hosting (open), 3 searching, 4 connecting,
		// 5 connected, 0 otherwise. (The device id in the searching and connecting states is unverified.)
		if (rfu->state == RFU_STATE_HOST) {
			buffer[0] = ((rfu->hostClosed ? 1u : 2u) << 24) | rfu->host.deviceId;
		} else if (rfu->state == RFU_STATE_CLIENT) {
			buffer[0] = (5u << 24) | ((1u << rfu->client.slot) << 16) | rfu->client.deviceId;
		} else if (rfu->state == RFU_STATE_CONNECTING) {
			buffer[0] = 4u << 24;
		} else if (rfu->searching) {
			buffer[0] = 3u << 24;
		} else {
			buffer[0] = 0;
		}
		return 1;

	case RFU_CMD_SLOT_STATUS:
		if (rfu->state != RFU_STATE_HOST) {
			return 0;
		}
		// First word: number of the next free client slot (0xFF when full); then one word per connected client.
		buffer[count++] = _nextFreeSlot(rfu);
		for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
			if (rfu->host.clients[i].deviceId) {
				buffer[count++] = rfu->host.clients[i].deviceId | (i << 16);
			}
		}
		return count;

	case RFU_CMD_SIGNAL_LEVEL:
		// (unverified) one byte per client slot, 0xFF = full strength
		buffer[0] = 0;
		if (rfu->state == RFU_STATE_HOST) {
			for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
				if (rfu->host.clients[i].deviceId) {
					buffer[0] |= 0xFFu << (8 * i);
				}
			}
		} else if (rfu->state == RFU_STATE_CLIENT) {
			buffer[0] = 0xFFFFFFFF;
		}
		return 1;

	case RFU_CMD_BROADCAST:
		if (rfu->length == RFU_BROADCAST_WORDS) {
			_trace(rfu, "BCAST  %08X %08X %08X %08X %08X %08X", buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5]);
			memcpy(rfu->broadcastData, buffer, sizeof(rfu->broadcastData));
			if (rfu->backend->setBroadcast) {
				rfu->backend->setBroadcast(rfu->backend, rfu->broadcastData);
			}
		}
		return 0;

	case RFU_CMD_START_HOST:
		if (rfu->state == RFU_STATE_CLIENT || rfu->state == RFU_STATE_CONNECTING) {
			return -RFU_ERROR_WRONG_STATE;
		}
		if (rfu->state == RFU_STATE_IDLE) {
			rfu->host.deviceId = _newDeviceId(rfu);
			memset(rfu->host.clients, 0, sizeof(rfu->host.clients));
			rfu->state = RFU_STATE_HOST;
			_trace(rfu, "HOST   start dev=%04X", rfu->host.deviceId);
			if (rfu->backend->hostStart) {
				rfu->backend->hostStart(rfu->backend, rfu->host.deviceId);
			}
		}
		rfu->host.broadcastCountdown = 0;
		rfu->hostClosed = false;
		return 0;

	case RFU_CMD_END_HOST:
		if (rfu->state == RFU_STATE_IDLE) {
			return -RFU_ERROR_WRONG_STATE;
		}
		if (rfu->state == RFU_STATE_HOST) {
			rfu->hostClosed = true; // no new clients from now on; the ones already connected stay
			if (rfu->backend->hostStop) {
				rfu->backend->hostStop(rfu->backend);
			}
			if (!_connectedClients(rfu)) {
				rfu->state = RFU_STATE_IDLE;
			}
		}
		return 0;

	case RFU_CMD_POLL_CONNECTIONS:
		if (rfu->state == RFU_STATE_IDLE) {
			return -RFU_ERROR_WRONG_STATE;
		}
		// Every connected client is listed on every poll (afska: a client that has gone away is still reported until the
		// host disconnects it). FireRed's library relies on it: it only counts a joiner as connected after seeing it in
		// four polls, and reads the signal levels (command 0x11) in between.
		for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
			if (rfu->host.clients[i].deviceId) {
				buffer[count++] = rfu->host.clients[i].deviceId | (i << 16);
			}
		}
		return count;

	case RFU_CMD_BROADCAST_READ_START:
		rfu->searching = true;
		if (rfu->backend->searchStart) {
			rfu->backend->searchStart(rfu->backend);
		}
		return 0;

	case RFU_CMD_BROADCAST_READ_POLL:
	case RFU_CMD_BROADCAST_READ_END:
		// Up to four hosts, each as one metadata word (device id) followed by its 6 words of broadcast data.
		for (i = 0; i < RFU_MAX_PEERS && count < 4 * (1 + RFU_BROADCAST_WORDS); ++i) {
			if (rfu->peers[i].valid) {
				buffer[count++] = rfu->peers[i].deviceId | ((uint32_t) rfu->peers[i].nextSlot << 16);
				for (j = 0; j < RFU_BROADCAST_WORDS; ++j) {
					buffer[count++] = rfu->peers[i].data[j];
				}
			}
		}
		if (rfu->command == RFU_CMD_BROADCAST_READ_END) {
			rfu->searching = false;
			if (rfu->backend->searchStop) {
				rfu->backend->searchStop(rfu->backend);
			}
		}
		return count;

	case RFU_CMD_CONNECT:
		if (rfu->state == RFU_STATE_HOST) {
			return -RFU_ERROR_WRONG_STATE;
		}
		{
			uint16_t id = buffer[0];
			for (i = 0; i < RFU_MAX_PEERS; ++i) {
				if (rfu->peers[i].valid && rfu->peers[i].deviceId == id) {
					_trace(rfu, "CONNECT to %04X", id);
					rfu->state = RFU_STATE_CONNECTING;
					if (rfu->backend->connect) {
						rfu->backend->connect(rfu->backend, id);
					}
					return 0;
				}
			}
			_trace(rfu, "CONNECT to unknown host %04X", id);
		}
		return 0;

	case RFU_CMD_IS_CONNECTION_COMPLETE:
		if (rfu->state == RFU_STATE_HOST) {
			return -RFU_ERROR_WRONG_STATE;
		}
		if (rfu->state == RFU_STATE_CONNECTING) {
			buffer[0] = RFU_CONNECTING;
		} else if (rfu->state == RFU_STATE_IDLE) {
			buffer[0] = RFU_CONNECTION_FAILED;
		} else {
			buffer[0] = rfu->client.deviceId | (rfu->client.slot << 16);
		}
		return 1;

	case RFU_CMD_FINISH_CONNECTION:
		if (rfu->state == RFU_STATE_HOST) {
			return -RFU_ERROR_WRONG_STATE;
		}
		if (rfu->state == RFU_STATE_CLIENT) {
			buffer[0] = rfu->client.deviceId | (rfu->client.slot << 16);
		} else {
			// The game gave up (its ~4 s patience) before the backend's connect result arrived.
			_trace(rfu, "FINISH_CONNECTION called while state=%d (not yet CLIENT) - forcing IDLE", rfu->state);
			buffer[0] = RFU_CONNECTING;
			rfu->state = RFU_STATE_IDLE;
		}
		return 1;

	case RFU_CMD_SEND_DATA:
	case RFU_CMD_SEND_DATA_WAIT:
		if (!rfu->length) {
			return 0;
		}
		if (rfu->state == RFU_STATE_HOST) {
			rfu->tx.length = buffer[0] & 0x7F;
		} else if (rfu->state == RFU_STATE_CLIENT) {
			rfu->tx.length = (buffer[0] >> (8 + rfu->client.slot * 5)) & 0x1F;
		} else {
			return -RFU_ERROR_WRONG_STATE;
		}
		if (rfu->length - 1 <= sizeof(rfu->tx.words) / sizeof(rfu->tx.words[0])) {
			memcpy(rfu->tx.words, &buffer[1], (rfu->length - 1) * sizeof(uint32_t));
		}
		_transmit(rfu);
		return 0;

	case RFU_CMD_RETRANSMIT_WAIT:
		if (rfu->state != RFU_STATE_HOST && rfu->state != RFU_STATE_CLIENT) {
			return -RFU_ERROR_WRONG_STATE;
		}
		_transmit(rfu);
		return 0;

	case RFU_CMD_RECEIVE_DATA:
		if (rfu->state == RFU_STATE_HOST) {
			uint8_t merged[RFU_MAX_CLIENTS * RFU_CLIENT_TX_MAX + 4] = {0};
			unsigned bytes = 0;
			buffer[count++] = 0;
			for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
				struct GBASIORFUPacket* packet = &rfu->host.clients[i].packets[0];
				unsigned length = packet->length > RFU_CLIENT_TX_MAX ? RFU_CLIENT_TX_MAX : packet->length;
				if (rfu->host.clients[i].deviceId && length) {
					memcpy(&merged[bytes], packet->data, length);
					bytes += length;
					buffer[0] |= length << (8 + i * 5);
					_popPacket(rfu->host.clients[i].packets);
				}
			}
			for (i = 0; i < (bytes + 3) / 4; ++i) {
				buffer[count++] = merged[i * 4] | (merged[i * 4 + 1] << 8) | (merged[i * 4 + 2] << 16) | ((uint32_t) merged[i * 4 + 3] << 24);
			}
			_trace(rfu, "RECV   host <- clients %u bytes", bytes);
			_traceBytes(rfu, "RECVB ", merged, bytes);
			return count;
		} else if (rfu->state == RFU_STATE_CLIENT) {
			struct GBASIORFUPacket* packet = &rfu->client.packets[0];
			unsigned length = packet->length;
			uint8_t padded[RFU_PACKET_MAX + 4] = {0};
			memcpy(padded, packet->data, length);
			buffer[count++] = length;
			for (i = 0; i < (length + 3) / 4; ++i) {
				buffer[count++] = padded[i * 4] | (padded[i * 4 + 1] << 8) | (padded[i * 4 + 2] << 16) | ((uint32_t) padded[i * 4 + 3] << 24);
			}
			_popPacket(rfu->client.packets);
			_trace(rfu, "RECV   client <- host %u bytes", length);
			_traceBytes(rfu, "RECVB ", padded, length);
			return count;
		}
		return -RFU_ERROR_WRONG_STATE;

	case RFU_CMD_WAIT:
		// Nothing to answer; the clock reversal is set up by the link layer once the ack has been read.
		return 0;

	case RFU_CMD_DISCONNECT:
		if (rfu->state == RFU_STATE_CLIENT || rfu->state == RFU_STATE_CONNECTING) {
			_trace(rfu, "DISCONNECT (self)");
			if (rfu->backend->disconnect) {
				rfu->backend->disconnect(rfu->backend, 0);
			}
			memset(&rfu->client, 0, sizeof(rfu->client));
			rfu->state = RFU_STATE_IDLE;
		} else if (rfu->state == RFU_STATE_HOST) {
			unsigned mask = buffer[0] & 0xF;
			_trace(rfu, "DISCONNECT clients mask=%X", mask);
			for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
				if (mask & (1u << i)) {
					memset(&rfu->host.clients[i], 0, sizeof(rfu->host.clients[0]));
				}
			}
			if (rfu->backend->disconnect) {
				rfu->backend->disconnect(rfu->backend, mask);
			}
		}
		return 0;

	default:
		_trace(rfu, "CMD    %02X is not a known command", rfu->command);
		return -RFU_ERROR_INVALID_COMMAND;
	}
}

static bool _dataAvailable(const struct GBASIORFU* rfu) {
	unsigned i;
	if (rfu->state == RFU_STATE_CLIENT) {
		return rfu->client.packets[0].length != 0;
	}
	if (rfu->state == RFU_STATE_HOST) {
		for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
			if (rfu->host.clients[i].deviceId && rfu->host.clients[i].packets[0].length) {
				return true;
			}
		}
	}
	return false;
}

static void _finishCommand(struct GBASIORFU* rfu) {
	uint8_t command = rfu->command;
	int result = _processCommand(rfu);
	if (result < 0) {
		rfu->link = RFU_LINK_ERROR_HEADER;
		rfu->errorCode = -result;
		_trace(rfu, "ERROR  cmd=%02X code=%u", rfu->command, rfu->errorCode);
	} else {
		rfu->link = RFU_LINK_RESPONSE_ACK;
		rfu->length = result;
		// DEBUG: dump the response words of the broadcast-search commands, to compare a working (local) peer
		// against a synthesized (LDN) one word-for-word. TODO remove once the LDN search-list mystery is solved.
		if ((command == RFU_CMD_BROADCAST_READ_POLL || command == RFU_CMD_BROADCAST_READ_END) && result > 0) {
			char text[512];
			int at = 0;
			for (int i = 0; i < result && at < (int) sizeof(text) - 10; ++i) {
				at += snprintf(&text[at], sizeof(text) - at, "%08X ", rfu->buffer[i]);
			}
			_trace(rfu, "RESP   cmd=%02X words=%d: %s", command, result, text);
		}
	}
	rfu->count = 0;
}

// One 32-bit word exchange with the GBA as clock master. Returns the word the adapter shifts back.
static uint32_t _exchange(struct GBASIORFU* rfu, uint32_t sent) {
	uint32_t reply = 0x80000000;

	switch (rfu->link) {
	case RFU_LINK_RESET:
		reply = 0;
		if ((sent & 0xFFFF) == 0x494E) {
			rfu->link = RFU_LINK_HANDSHAKE;
		}
		break;

	case RFU_LINK_HANDSHAKE:
		// Each reply carries the GBA's low half in the top and the complement of its previous low half below.
		reply = (sent << 16) | (~rfu->previousWord & 0xFFFF);
		if (sent == 0xB0BB8001) {
			rfu->link = RFU_LINK_COMMAND;
			_trace(rfu, "LINK   handshake complete");
			rfu->pcSamples = 1;
		}
		break;

	case RFU_LINK_COMMAND:
		if ((sent >> 16) == 0x9966) {
			rfu->length = (sent >> 8) & 0xFF;
			rfu->command = sent & 0xFF;
			rfu->count = 0;
			if (!rfu->length) {
				_finishCommand(rfu);
			} else {
				rfu->link = RFU_LINK_COMMAND_DATA;
			}
		}
		break;

	case RFU_LINK_COMMAND_DATA:
		rfu->buffer[rfu->count++] = sent;
		if (rfu->count >= rfu->length) {
			_finishCommand(rfu);
		}
		break;

	case RFU_LINK_RESPONSE_ACK:
		reply = 0x99660080 | rfu->command | (rfu->length << 8);
		if (rfu->command == RFU_CMD_WAIT || rfu->command == RFU_CMD_SEND_DATA_WAIT || rfu->command == RFU_CMD_RETRANSMIT_WAIT) {
			// The adapter takes the clock now and will report an event or a timeout.
			rfu->link = RFU_LINK_WAIT_EVENT;
			rfu->timeoutCycles = (int32_t) rfu->timeout * RFU_ADAPTER_FRAME;
			rfu->retransmitCycles = (int32_t) rfu->retransmits * (RFU_ADAPTER_FRAME / 6);
			_trace(rfu, "LINK   wait (cmd %02X)", rfu->command);
		} else {
			rfu->link = rfu->length ? RFU_LINK_RESPONSE_DATA : RFU_LINK_COMMAND;
		}
		break;

	case RFU_LINK_RESPONSE_DATA:
		reply = rfu->buffer[rfu->count++];
		if (rfu->count >= rfu->length) {
			rfu->link = RFU_LINK_COMMAND;
		}
		break;

	case RFU_LINK_ERROR_HEADER:
		reply = 0x996601EE;
		rfu->link = RFU_LINK_ERROR_CODE;
		break;

	case RFU_LINK_ERROR_CODE:
		reply = rfu->errorCode;
		rfu->link = RFU_LINK_COMMAND;
		break;

	case RFU_LINK_WAIT_EVENT:
	case RFU_LINK_WAIT_RESPONSE:
		// The adapter owns the clock; a stray master transfer from the game is ignored.
		break;
	}

	rfu->previousWord = sent;
	return reply;
}

// ---------------------------------------------------------------------------------------------------------------
// Clock reversal: the adapter reports events to a GBA that is waiting as SIO slave.
// ---------------------------------------------------------------------------------------------------------------

static void _beginNotification(struct GBASIORFU* rfu, const uint32_t* words, unsigned count) {
	memcpy(rfu->buffer, words, count * sizeof(*words));
	rfu->count = 0;
	rfu->length = count;
	rfu->link = RFU_LINK_WAIT_RESPONSE;
}

static void _waitUpdate(struct GBASIORFU* rfu, uint32_t elapsed) {
	struct GBASIO* sio = rfu->d.p;
	if (rfu->link == RFU_LINK_WAIT_EVENT) {
		rfu->timeoutCycles -= elapsed < (uint32_t) rfu->timeoutCycles ? elapsed : rfu->timeoutCycles;
		rfu->retransmitCycles -= elapsed < (uint32_t) rfu->retransmitCycles ? elapsed : rfu->retransmitCycles;

		// Only when the game has switched to external clock (slave) is it listening for the adapter.
		if (!(sio->siocnt & 0x0001)) {
			if (rfu->state == RFU_STATE_IDLE) {
				const uint32_t words[] = {0x99660000 | (1 << 8) | RFU_NOTIFY_DISCONNECT, 0xF, 0x80000000};
				_trace(rfu, "NOTIFY disconnected");
				_beginNotification(rfu, words, 3);
			} else if (_dataAvailable(rfu)) {
				const uint32_t words[] = {0x99660000 | RFU_NOTIFY_DATA, 0x80000000};
				_trace(rfu, "NOTIFY data available");
				_beginNotification(rfu, words, 2);
			} else if (rfu->state == RFU_STATE_HOST && !rfu->retransmitCycles) {
				// The retransmit time elapsed without an answer from any client.
				// Bits 0-3: clients that received the data (every connected one, the link cannot lose it); bits 8-11:
				// clients marked inactive (none).
				uint32_t received = 0;
				for (unsigned i = 0; i < RFU_MAX_CLIENTS; ++i) {
					received |= rfu->host.clients[i].deviceId ? 1u << i : 0;
				}
				const uint32_t words[] = {0x99660000 | (1 << 8) | RFU_NOTIFY_DATA, received, 0x80000000};
				_trace(rfu, "NOTIFY host retransmit elapsed");
				_beginNotification(rfu, words, 3);
			} else if (!rfu->timeoutCycles) {
				const uint32_t words[] = {0x99660000 | RFU_NOTIFY_TIMEOUT, 0x80000000};
				_trace(rfu, "NOTIFY timeout");
				_beginNotification(rfu, words, 2);
			}
		}
	}

	if (rfu->link == RFU_LINK_WAIT_RESPONSE) {
		// The game arms a slave transfer (Start set, external clock) with SO/SI low when it is ready for the next word.
		if ((sio->siocnt & 0x000D) == 0 && (sio->siocnt & 0x0080)) {
			uint32_t word = rfu->buffer[rfu->count++];
			_trace(rfu, "NOTIFY word %08X", word);
			if (rfu->count >= rfu->length) {
				rfu->link = RFU_LINK_COMMAND;
			}
			GBASIONormal32FinishTransfer(sio, word, 0);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Frame / timing
// ---------------------------------------------------------------------------------------------------------------

static void _frameUpdate(struct GBASIORFU* rfu) {
	unsigned i;
	if (rfu->link == RFU_LINK_RESET) {
		return;
	}

	for (i = 0; i < RFU_MAX_PEERS; ++i) {
		if (rfu->peers[i].valid && !--rfu->peers[i].ttl) {
			_trace(rfu, "PEER   host %04X expired", rfu->peers[i].deviceId);
			rfu->peers[i].valid = false;
		}
	}

	if (rfu->state == RFU_STATE_HOST) {
		for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
			if (rfu->host.clients[i].deviceId && ++rfu->host.clients[i].ttl >= RFU_CLIENT_TTL) {
				_trace(rfu, "HOST   client slot %u timed out", i);
				memset(&rfu->host.clients[i], 0, sizeof(rfu->host.clients[0]));
			}
		}
	}

	if (rfu->backend->frame) {
		rfu->backend->frame(rfu->backend);
	}
}

static void _tick(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBASIORFU* rfu = context;
	uint32_t now = mTimingCurrentTime(timing);
	uint32_t elapsed = now - rfu->lastTick;
	rfu->lastTick = now;

	if (rfu->link != RFU_LINK_RESET && rfu->backend->poll) {
		rfu->backend->poll(rfu->backend);
	}
	_drainEvents(rfu);

	if (rfu->trace) {
		// Register snapshot whenever the game changed something we are not hooked into.
		struct GBASIO* sio = rfu->d.p;
		uint32_t data = sio->p->memory.io[GBA_REG(SIODATA32_LO)] | ((uint32_t) sio->p->memory.io[GBA_REG(SIODATA32_HI)] << 16);
		uint32_t state = sio->siocnt | ((uint32_t) sio->rcnt << 16);
		if (data != rfu->snapData || state != rfu->snapState) {
			rfu->snapData = data;
			rfu->snapState = state;
			_trace(rfu, "SNAP   siocnt=%04X rcnt=%04X data=%08X ie=%04X if=%04X link=%d", sio->siocnt, sio->rcnt, data,
			       sio->p->memory.io[GBA_REG(IE)], sio->p->memory.io[GBA_REG(IF)], rfu->link);
		}
	}

	if (rfu->trace && rfu->pcSamples > 0 && rfu->pcSamples < 90) {
		// Where is the game while it waits for the adapter?
		struct ARMCore* cpu = rfu->d.p->p->cpu;
		_trace(rfu, "PC     %08X %s r0=%08X r1=%08X r2=%08X r3=%08X", cpu->gprs[ARM_PC], cpu->executionMode == MODE_THUMB ? "thumb" : "arm  ",
		       cpu->gprs[0], cpu->gprs[1], cpu->gprs[2], cpu->gprs[3]);
		++rfu->pcSamples;
	}

	rfu->frameAccumulator += elapsed;
	while (rfu->frameAccumulator >= RFU_ADAPTER_FRAME) {
		rfu->frameAccumulator -= RFU_ADAPTER_FRAME;
		if (rfu->trace) {
			fflush(rfu->trace);
		}
		_frameUpdate(rfu);
	}

	_waitUpdate(rfu, elapsed);

	int32_t next = (rfu->link == RFU_LINK_WAIT_EVENT || rfu->link == RFU_LINK_WAIT_RESPONSE) ? RFU_WAIT_POLL_CYCLES : RFU_IDLE_POLL_CYCLES;
	mTimingSchedule(timing, &rfu->tickEvent, next - (int32_t) cyclesLate);
}

// Loading a save state clears the core's timing queue without telling the SIO driver, which silently drops our
// events. Any activity from the game re-arms the tick, so a state loaded mid-session keeps working.
static void _ensureTick(struct GBASIORFU* rfu) {
	struct mTiming* timing = &rfu->d.p->p->timing;
	if (!mTimingIsScheduled(timing, &rfu->tickEvent)) {
		_trace(rfu, "TICK   re-armed (timing queue was cleared)");
		rfu->lastTick = mTimingCurrentTime(timing);
		rfu->frameAccumulator = 0;
		mTimingSchedule(timing, &rfu->tickEvent, RFU_WAIT_POLL_CYCLES);
	}
}

static void _transferFinished(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(timing);
	struct GBASIORFU* rfu = context;
	struct GBASIO* sio = rfu->d.p;
	_trace(rfu, "XFER   rx=%08X link=%d", rfu->pendingReply, rfu->link);
	GBASIONormal32FinishTransfer(sio, rfu->pendingReply, cyclesLate);
	// The adapter drives SI high (busy) after each word; see the SO/SI handling in GBASIORFUWriteSIOCNT.
	sio->siocnt = GBASIONormalFillSi(sio->siocnt);
}

// ---------------------------------------------------------------------------------------------------------------
// GBASIODriver interface
// ---------------------------------------------------------------------------------------------------------------

static void _resetAdapter(struct GBASIORFU* rfu) {
	rfu->link = RFU_LINK_RESET;
	rfu->state = RFU_STATE_IDLE;
	rfu->previousWord = 0;
	rfu->pendingReply = 0;
	rfu->count = 0;
	rfu->length = 0;
	rfu->timeout = RFU_DEFAULT_TIMEOUT;
	rfu->retransmits = RFU_DEFAULT_RETRANSMITS;
	rfu->maxClients = RFU_MAX_CLIENTS;
	rfu->hostClosed = false;
	rfu->searching = false;
	rfu->timeoutCycles = 0;
	rfu->retransmitCycles = 0;
	rfu->tx.length = 0;
	memset(rfu->peers, 0, sizeof(rfu->peers));
	memset(&rfu->host, 0, sizeof(rfu->host));
	memset(&rfu->client, 0, sizeof(rfu->client));

	MutexLock(&rfu->eventMutex);
	rfu->eventHead = rfu->eventTail = 0;
	MutexUnlock(&rfu->eventMutex);

	if (rfu->backend && rfu->backend->reset) {
		rfu->backend->reset(rfu->backend);
	}
}

static bool GBASIORFUInit(struct GBASIODriver* driver) {
	struct GBASIORFU* rfu = (struct GBASIORFU*) driver;
	rfu->mode = GBA_SIO_NORMAL_8;
	_resetAdapter(rfu);
	if (rfu->backend && rfu->backend->init && !rfu->backend->init(rfu->backend, rfu)) {
		return false;
	}
	struct mTiming* timing = &driver->p->p->timing;
	rfu->lastTick = mTimingCurrentTime(timing);
	rfu->frameAccumulator = 0;
	mTimingDeschedule(timing, &rfu->tickEvent);
	mTimingSchedule(timing, &rfu->tickEvent, RFU_IDLE_POLL_CYCLES);
	_trace(rfu, "DRIVER attached");
	return true;
}

static void GBASIORFUDeinit(struct GBASIODriver* driver) {
	struct GBASIORFU* rfu = (struct GBASIORFU*) driver;
	struct mTiming* timing = &driver->p->p->timing;
	mTimingDeschedule(timing, &rfu->tickEvent);
	mTimingDeschedule(timing, &rfu->transferEvent);
	if (rfu->backend && rfu->backend->deinit) {
		rfu->backend->deinit(rfu->backend);
	}
	_trace(rfu, "DRIVER detached");
}

static void GBASIORFUReset(struct GBASIODriver* driver) {
	struct GBASIORFU* rfu = (struct GBASIORFU*) driver;
	struct mTiming* timing = &driver->p->p->timing;
	// The core clears its timing queue before resetting the SIO, so our events have to be armed again.
	mTimingDeschedule(timing, &rfu->transferEvent);
	mTimingDeschedule(timing, &rfu->tickEvent);
	_resetAdapter(rfu);
	rfu->lastTick = mTimingCurrentTime(timing);
	rfu->frameAccumulator = 0;
	mTimingSchedule(timing, &rfu->tickEvent, RFU_IDLE_POLL_CYCLES);
}

static void GBASIORFUSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	struct GBASIORFU* rfu = (struct GBASIORFU*) driver;
	_trace(rfu, "MODE   %d -> %d", rfu->mode, mode);
	rfu->mode = mode;
}

static bool GBASIORFUHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	return mode == GBA_SIO_NORMAL_32;
}

static int GBASIORFUConnectedDevices(struct GBASIODriver* driver) {
	UNUSED(driver);
	return 1;
}

static uint16_t GBASIORFUWriteRCNT(struct GBASIODriver* driver, uint16_t value) {
	struct GBASIORFU* rfu = (struct GBASIORFU*) driver;
	uint16_t old = driver->p->rcnt;
	_ensureTick(rfu);
	if (value != old) {
		_trace(rfu, "RCNT   %04X -> %04X (mode %d)", old, value, rfu->mode);
	}
	if (rfu->mode == GBA_SIO_GPIO) {
		// The game resets the adapter by driving the SD pin (bit 1, direction bit 5) high.
		if ((value & 0x22) == 0x22 && !(old & 0x02)) {
			_trace(rfu, "RESET  adapter (SD driven high)");
			if (rfu->trace) {
				struct ARMCore* cpu = driver->p->p->cpu;
				uint32_t sp = cpu->gprs[ARM_SP];
				_trace(rfu, "RESETBY pc=%08X lr=%08X sp=%08X %s", cpu->gprs[ARM_PC], cpu->gprs[ARM_LR], sp,
				       cpu->executionMode == MODE_THUMB ? "thumb" : "arm");
				for (unsigned i = 0; i < 12; ++i) {
					_trace(rfu, "RESETBY   [sp+%02X] = %08X", i * 4, cpu->memory.load32(cpu, sp + i * 4, NULL));
				}
			}
			mTimingDeschedule(&driver->p->p->timing, &rfu->transferEvent);
			_resetAdapter(rfu);
		}
	}
	return value;
}

static bool GBASIORFUStart(struct GBASIODriver* driver) {
	struct GBASIORFU* rfu = (struct GBASIORFU*) driver;
	// Normal 32-bit transfers are started from GBASIORFUWriteSIOCNT, where the new register value is visible, and
	// completed by this driver. Anything else (e.g. a cable multiplayer transfer) gets the core's default timing so
	// the game is never left waiting on a transfer nobody finishes.
	_trace(rfu, "START  core notified (mode %d)", rfu->mode);
	return rfu->mode != GBA_SIO_NORMAL_32;
}

static uint16_t GBASIORFUWriteSIOCNT(struct GBASIODriver* driver, uint16_t value) {
	struct GBASIORFU* rfu = (struct GBASIORFU*) driver;
	struct GBASIO* sio = driver->p;
	uint16_t old = sio->siocnt;
	_ensureTick(rfu);

	// SI (bit 2) is driven by the adapter, the game cannot write it.
	value = (value & 0x7F8B) | (old & 0x0004);

	// The game starts a master transfer by having Start set while the clock is internal. FireRed sets Start first
	// (external clock, waiting) and then flips the clock bit with Start still set, so watch for either edge.
	bool startRising = (value & 0x0080) && !(old & 0x0080);
	bool masterStart = (value & 0x0080) && (value & 0x0001) && (startRising || !(old & 0x0001));
	if (masterStart) {
		// Game is clock master: exchange one word with the adapter.
		if (!mTimingIsScheduled(&sio->p->timing, &rfu->transferEvent)) {
			uint32_t sent = sio->p->memory.io[GBA_REG(SIODATA32_LO)] | ((uint32_t) sio->p->memory.io[GBA_REG(SIODATA32_HI)] << 16);
			rfu->pendingReply = _exchange(rfu, sent);
			_trace(rfu, "XFER   tx=%08X", sent);
			mTimingSchedule(&sio->p->timing, &rfu->transferEvent, GBASIOTransferCycles(GBA_SIO_NORMAL_32, value, 1));
		}
	}

	uint16_t requested = value;

	// The SO/SI handshake between transfers.
	if (value & 0x0001) {
		// Master: once the GBA raises SO to say it is busy, the adapter is ready again.
		if ((value & 0x0008) && !(old & 0x0008)) {
			value &= ~0x0004;
		}
	} else {
		// Slave: SI follows SO.
		if ((value & 0x0008) && !(old & 0x0008)) {
			value |= 0x0004;
		}
		if (!(value & 0x0008) && (old & 0x0008)) {
			value &= ~0x0004;
		}
	}
	_trace(rfu, "SIOCNT %04X -> %04X (wrote %04X; SI %d, SO %d, %s clock, start %d)", old, value, requested, !!(value & 4),
	       !!(value & 8), (value & 1) ? "internal" : "external", !!(value & 0x80));
	return value;
}
