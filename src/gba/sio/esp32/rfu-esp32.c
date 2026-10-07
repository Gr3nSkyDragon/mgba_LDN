/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/rfu-esp32.h>

#include "esp32-serial.h"
#include "esp32-wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Portability: the backend needs one I/O thread, one lock, a millisecond tick and a sleep. Windows uses its native
 * calls; everything else (Android, Linux, macOS) uses POSIX threads. Everything below is otherwise platform independent
 * (the serial port itself comes from esp32-serial.h).
 */
#ifdef _WIN32
#include <windows.h>

typedef HANDLE EspThread;
typedef CRITICAL_SECTION EspLock;
typedef volatile LONG EspFlag;

#define ESP_THREAD_FUNC(name) static DWORD WINAPI name(LPVOID context)
#define ESP_THREAD_RETURN return 0

static uint32_t _ticks(void) { return GetTickCount(); }
static void _espSleep(unsigned ms) { Sleep(ms); }
static void _lockInit(EspLock* lock) { InitializeCriticalSection(lock); }
static void _lockEnter(EspLock* lock) { EnterCriticalSection(lock); }
static void _lockLeave(EspLock* lock) { LeaveCriticalSection(lock); }
static void _lockDelete(EspLock* lock) { DeleteCriticalSection(lock); }
static void _flagSet(EspFlag* flag, int value) { InterlockedExchange(flag, value); }
static bool _threadStart(EspThread* thread, LPTHREAD_START_ROUTINE function, void* context) {
	*thread = CreateThread(NULL, 0, function, context, 0, NULL);
	return *thread != NULL;
}
static void _threadJoin(EspThread* thread) {
	WaitForSingleObject(*thread, 8000);
	CloseHandle(*thread);
}
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>

typedef pthread_t EspThread;
typedef pthread_mutex_t EspLock;
typedef volatile int EspFlag;

#define ESP_THREAD_FUNC(name) static void* name(void* context)
#define ESP_THREAD_RETURN return NULL

static uint32_t _ticks(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t) ((uint64_t) ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
static void _espSleep(unsigned ms) { usleep(ms * 1000u); }
static void _lockInit(EspLock* lock) { pthread_mutex_init(lock, NULL); }
static void _lockEnter(EspLock* lock) { pthread_mutex_lock(lock); }
static void _lockLeave(EspLock* lock) { pthread_mutex_unlock(lock); }
static void _lockDelete(EspLock* lock) { pthread_mutex_destroy(lock); }
static void _flagSet(EspFlag* flag, int value) { __atomic_store_n(flag, value, __ATOMIC_SEQ_CST); }
static bool _threadStart(EspThread* thread, void* (*function)(void*), void* context) {
	return pthread_create(thread, NULL, function, context) == 0;
}
static void _threadJoin(EspThread* thread) {
	pthread_join(*thread, NULL);
}
#endif

/*
 * How this backend talks to GB-Link's ESP32 LDN bridge (everything below was observed against a real ESP32-S3
 * with esp32-probe before being relied on):
 *
 *   1. Opening the board's native USB serial port resets it. After it has booted, "\nLDN_BINARY\n" + 0x00 switches
 *      the console to COBS/CRC framed binary mode (esp32-wire.c). LDN_HELLO / LDN_BEGIN <session> follow, then
 *      LDN_BRIDGE_START (harmless if it is already scanning) and "LDN_ADAPTER host": the board then treats this
 *      program as the Pico that normally sits on the GBA cable.
 *   2. As that Pico it must report, about twice a second, a status frame (GB channel 2, 0xFF02 "awaiting mode") and
 *      a data frame (channel 1, 0x0E 0x01 "GBA active"); until it does, the board keeps asking for mode 0x07.
 *      LDN_PING goes out once a second.
 *   3. The board scans for the Switch's FRLG room, joins it, and from then on sends RFU1 frames (message type 6, GB
 *      channel 1): BROADCAST for each of three groups (trade, single battle, double battle) every 500ms, CONNECT_ACK
 *      and HOST_SEND. We send CONNECT_REQ, CLIENT_SEND and DISCONNECT the same way inside message type 7.
 *   4. Hosting (firmware 2.1 and later, when the game in the emulator leads a group): while the board is still scanning,
 *      the RFU1 BROADCAST frames WE send - the leader's own device id and the six words of its game data, every 500ms,
 *      until a client is in - make it host a room that looks like a Switch's (pia_bridge.c BR_HOST). When the Switch
 *      joins, the board sends ONE CONNECT_REQ (header: our device id); the answer is CONNECT_ACK, whose header carries
 *      the client's id (ours to choose) and slot, or CONNECT_NACK. From then on we send one HOST_SEND per host frame
 *      (header: its length), and the Switch's frames arrive as CLIENT_SEND (header: id | slot << 16 | length << 24), with
 *      CLIENT_ACK keep-alives every 500ms and DISCONNECT (header: id | slot << 16) either way.
 *
 * RFU1 = "RFU1" + type:u32 BE + header:u32 BE + body. The 24-byte BROADCAST body is six big-endian words that are
 * exactly the words the game reads back in a broadcast record.
 */

enum {
	kBridgeRestartMs = 2500,
	kOutSlots = 64,
	kOutBytes = 128,
	kBootWaitMs = 4500,
	kPicoReportMs = 500,
	kPingMs = 1000,
	kConnectTimeoutMs = 8000,
	kRetryMs = 2500,
	kBeaconMs = 500,

	RFU1_BROADCAST = 0,
	RFU1_CONNECT_REQ = 1,
	RFU1_CONNECT_ACK = 2,
	RFU1_CONNECT_NACK = 3,
	RFU1_DISCONNECT = 4,
	RFU1_HOST_SEND = 5,
	RFU1_CLIENT_SEND = 6,
	RFU1_CLIENT_ACK = 7,

	kMaxPayload = 92,

	// A room beacon heard within this long counts as in range for the status display.
	kBeaconFreshMs = 2000,
};

// Where the I/O thread is, for the status display.
enum EspPhase {
	ESP_PHASE_LOOKING,
	ESP_PHASE_MISSING,
	ESP_PHASE_BOOTING,
	ESP_PHASE_HANDSHAKE,
	ESP_PHASE_READY,
};

struct GBASIORFUESP32 {
	struct GBASIORFUBackend d;
	struct GBASIORFU* rfu;
	char configuredPort[32];

	EspThread thread;
	bool threadValid;
	EspFlag stop;
	EspFlag ready; // handshake finished: the board is in adapter-host mode

	// Emulation thread -> I/O thread, and the connect bookkeeping both threads share. Created in Create(), not init():
	// the SIO driver calls backend->reset() before backend->init(), and reset() takes this lock.
	EspLock lock;
	struct {
		uint8_t data[kOutBytes];
		size_t length;
	} out[kOutSlots];
	unsigned outHead;
	unsigned outCount;
	bool connectPending;
	bool connected;
	uint16_t connectDevice;
	uint32_t connectDeadline;
	// For the status display (_status), written by the I/O thread.
	enum EspPhase phase;
	char statusPort[32];
	char statusError[RFU_BACKEND_STATUS_TEXT];
	char statusEvent[64]; // the board's last bridge/room/link event
	bool beaconSeen;
	uint32_t beaconAt;
	uint16_t beaconDevice;

	// Host role (the game in the emulator leads a group, see 4. above). Also under `lock`: the emulation thread sets it, the
	// I/O thread advertises it and reports the board's join request.
	bool hosting; // hostStart .. hostStop: advertising while no client is in
	uint16_t hostDeviceId;
	uint32_t broadcastWords[RFU_BROADCAST_WORDS];
	bool haveBroadcast;
	bool hostClient; // the Switch was accepted and is in the room
	uint16_t hostClientId;
	uint8_t hostClientSlot;
	uint16_t pendingClientId; // a CONNECT_REQ the driver has not answered yet
	uint32_t nextBeacon;

	// I/O thread only.
	struct Esp32Serial* port;
	struct Esp32WireParser parser;
	uint32_t request;
	uint32_t session;
	const char* awaitPrefix; // while set, a response or event frame whose text starts with it sets awaitMatched
	bool awaitMatched;
	char lastText[64];
	// A join timeout or lost network reported without the board stopping: the I/O thread nudges it with LDN_BRIDGE_START
	// a moment later. A stopped bridge means a reboot instead (boardRebooting below).
	bool bridgeRestart;
	uint32_t bridgeRestartAt;
	unsigned bridgeRestarts;
	// "LDN_BRIDGE stopped" is the last thing the board says before it reboots (pia_bridge.c bridge_restart ends in
	// esp_restart, after every failed join, handshake timeout, lost network or ended session). It comes back in text mode
	// with no session and not in adapter-host mode, while the native USB port stays open, so nothing short of the whole
	// handshake again reaches it: _run returns and the I/O thread redoes it on the same port.
	bool boardRebooting;
	unsigned boardReboots;
	unsigned bytesRead;
	unsigned beacons;
	unsigned beaconsSent;
	uint8_t rx[256]; // RFU1 stream reassembly (see _handleFrame)
	size_t rxUsed;
	char portName[32];
};

static uint32_t _be32(const uint8_t* p) {
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static void _putBe32(uint8_t* p, uint32_t value) {
	p[0] = (uint8_t) (value >> 24);
	p[1] = (uint8_t) (value >> 16);
	p[2] = (uint8_t) (value >> 8);
	p[3] = (uint8_t) value;
}

static void _setPhase(struct GBASIORFUESP32* esp, enum EspPhase phase) {
	_lockEnter(&esp->lock);
	esp->phase = phase;
	snprintf(esp->statusPort, sizeof(esp->statusPort), "%s", phase == ESP_PHASE_LOOKING || phase == ESP_PHASE_MISSING ? "" : esp->portName);
	if (phase != ESP_PHASE_READY) {
		esp->statusEvent[0] = 0;
		esp->beaconSeen = false;
	}
	if (phase == ESP_PHASE_BOOTING) {
		// The board is back: whatever went wrong before (it was lost, the port would not open) is history. A fresh failure
		// in this attempt sets its own error.
		esp->statusError[0] = 0;
	}
	_lockLeave(&esp->lock);
}

static void _setError(struct GBASIORFUESP32* esp, const char* error) {
	_lockEnter(&esp->lock);
	snprintf(esp->statusError, sizeof(esp->statusError), "%s", error);
	_lockLeave(&esp->lock);
}

// Queues one GB frame (already built) for the I/O thread to send as a type 7 message. Any thread.
static bool _enqueue(struct GBASIORFUESP32* esp, const uint8_t* gbFrame, size_t length) {
	if (length > kOutBytes) {
		return false;
	}
	bool queued = false;
	_lockEnter(&esp->lock);
	if (esp->outCount < kOutSlots) {
		unsigned slot = (esp->outHead + esp->outCount) % kOutSlots;
		memcpy(esp->out[slot].data, gbFrame, length);
		esp->out[slot].length = length;
		++esp->outCount;
		queued = true;
	}
	_lockLeave(&esp->lock);
	return queued;
}

static void _sendRfu1(struct GBASIORFUESP32* esp, uint32_t type, uint32_t header, const uint8_t* body, size_t bodyLength, size_t frameLength) {
	uint8_t rfu1[104] = {0};
	memcpy(rfu1, "RFU1", 4);
	_putBe32(&rfu1[4], type);
	_putBe32(&rfu1[8], header);
	if (body && bodyLength) {
		memcpy(&rfu1[12], body, bodyLength);
	}
	// The board reads this direction as a byte stream cut into data-channel chunks of EXACTLY 64 bytes, zero padded:
	// "RFU1 chunks are exactly 64 bytes... anything shorter is the adapter's own telemetry" (and is thrown away - a
	// bare 16-byte connect request never got an answer). It resynchronises on the "RFU1" magic and knows each frame's
	// size from its type, so the padding is skipped and a 104-byte frame simply spans two chunks.
	for (size_t offset = 0; offset < frameLength; offset += 64) {
		uint8_t chunk[64] = {0};
		size_t n = frameLength - offset < 64 ? frameLength - offset : 64;
		memcpy(chunk, &rfu1[offset], n);
		uint8_t gb[kOutBytes];
		size_t length = Esp32GbFrameBuild(GB_CHANNEL_DATA, chunk, sizeof(chunk), gb, sizeof(gb));
		if (length) {
			_enqueue(esp, gb, length);
		}
	}
}

// ---- I/O thread -----------------------------------------------------------------------------------------------

static void _writeFrame(struct GBASIORFUESP32* esp, uint8_t type, uint32_t requestId, const void* payload, size_t length) {
	uint8_t out[512];
	size_t encoded = Esp32WireEncode(type, requestId, esp->session, payload, length, out, sizeof(out));
	if (encoded) {
		Esp32SerialWrite(esp->port, out, encoded);
	}
}

static void _command(struct GBASIORFUESP32* esp, const char* text) {
	_writeFrame(esp, ESP32_TYPE_COMMAND, ++esp->request, text, strlen(text));
}

static void _handleRfu1(struct GBASIORFUESP32* esp, const uint8_t* rfu1, size_t available);

static void _handleFrame(struct GBASIORFUESP32* esp, const struct Esp32WireFrame* frame) {
	if (frame->type == ESP32_TYPE_RESPONSE || frame->type == ESP32_TYPE_EVENT) {
		char text[96];
		size_t n = frame->payloadLength < sizeof(text) - 1 ? frame->payloadLength : sizeof(text) - 1;
		memcpy(text, frame->payload, n);
		text[n] = 0;
		snprintf(esp->lastText, sizeof(esp->lastText), "%s", text);
		// Matched here, per frame: a reply is often several frames ("LDN_HELLO ...", then "LDN_DONE") that arrive in
		// one serial read, so checking only the last one after the read would lose the one that matters.
		if (esp->awaitPrefix && !strncmp(text, esp->awaitPrefix, strlen(esp->awaitPrefix))) {
			esp->awaitMatched = true;
		}
		if (!strncmp(text, "LDN_ERROR", 9)) {
			GBASIORFUTrace(esp->rfu, "ESP32  device error: %s", text);
			_setError(esp, text);
		} else if (frame->type == ESP32_TYPE_EVENT &&
		           (!strncmp(text, "LDN_ROOM", 8) || !strncmp(text, "LDN_LINK", 8) || !strncmp(text, "LDN_BRIDGE", 10) ||
		            !strncmp(text, "LDN_NET_LOST", 12))) {
			GBASIORFUTrace(esp->rfu, "ESP32  event: %s", text);
			_lockEnter(&esp->lock);
			snprintf(esp->statusEvent, sizeof(esp->statusEvent), "%s", text);
			_lockLeave(&esp->lock);
			if (!esp->stop && !strncmp(text, "LDN_BRIDGE stopped", 18)) {
				esp->boardRebooting = true;
				esp->bridgeRestart = false;
			} else if (!esp->stop && (!strncmp(text, "LDN_BRIDGE join timed out", 25) || !strncmp(text, "LDN_NET_LOST", 12))) {
				esp->bridgeRestart = true;
				esp->bridgeRestartAt = _ticks() + kBridgeRestartMs;
			}
		}
		return;
	}
	if (frame->type != ESP32_TYPE_GB_FRAME) {
		return;
	}
	const uint8_t* gb = frame->payload;
	if (frame->payloadLength < GB_HEADER || gb[0] != 0x47 || gb[1] != 0x42 || gb[2] != GB_CHANNEL_DATA) {
		return;
	}
	// The board sends RFU1 frames as a byte stream cut into data-channel chunks of up to 64 bytes (a 104-byte
	// HOST_SEND arrives as 64 + 40), so reassemble: resynchronise on the "RFU1" magic and take each frame's size from
	// its type - exactly as the firmware's own receive side does.
	size_t chunk = frame->payloadLength - GB_HEADER;
	if (esp->rxUsed + chunk > sizeof(esp->rx)) {
		esp->rxUsed = 0;
	}
	memcpy(esp->rx + esp->rxUsed, gb + GB_HEADER, chunk);
	esp->rxUsed += chunk;
	size_t at = 0;
	while (esp->rxUsed - at >= 12) {
		if (memcmp(esp->rx + at, "RFU1", 4)) {
			++at;
			continue;
		}
		uint32_t type = _be32(esp->rx + at + 4);
		size_t size = type == RFU1_BROADCAST ? 36 : (type == RFU1_HOST_SEND || type == RFU1_CLIENT_SEND ? 104 : 16);
		if (esp->rxUsed - at < size) {
			break;
		}
		_handleRfu1(esp, esp->rx + at, size);
		at += size;
	}
	if (at) {
		memmove(esp->rx, esp->rx + at, esp->rxUsed - at);
		esp->rxUsed -= at;
	}
}

// One complete RFU1 frame from the board.
static void _handleRfu1(struct GBASIORFUESP32* esp, const uint8_t* rfu1, size_t available) {
	uint32_t type = _be32(&rfu1[4]);
	uint32_t header = _be32(&rfu1[8]);
	if (type == RFU1_BROADCAST && available >= 12 + 24) {
		uint32_t words[RFU_BROADCAST_WORDS];
		for (int i = 0; i < RFU_BROADCAST_WORDS; ++i) {
			words[i] = _be32(&rfu1[12 + i * 4]);
		}
		bool occupied = (header >> 16) & 1;
		// The Switch room record can arrive with "can link nationally" (compat bit 7) clear even for a finished game
		// (national dex bit 8 + game clear bit 9 set); Emerald then refuses the trade as "not ready yet" when the
		// trainer is picked. Experimental: treat a finished save as able to link nationally and fix the checksum
		// (byte 15) the game validates. MGBA_RFU_NO_NATIONAL_FIX=1 disables it.
		uint32_t compat = words[0] >> 16;
		if ((compat & 0x0300) == 0x0300 && !(compat & 0x0080) && !getenv("MGBA_RFU_NO_NATIONAL_FIX")) {
			words[0] |= 0x0080u << 16;
			uint32_t sum = ((words[0] >> 16) & 0xFF) + ((words[0] >> 24) & 0xFF) + (words[1] & 0xFF) + ((words[1] >> 8) & 0xFF) +
			               ((words[1] >> 16) & 0xFF) + ((words[1] >> 24) & 0xFF) + (words[2] & 0xFF) + ((words[2] >> 8) & 0xFF);
			for (int i = 0; i < 4; ++i) {
				sum += (words[4] >> (i * 8)) & 0xFF;
				sum += (words[5] >> (i * 8)) & 0xFF;
			}
			words[3] = (words[3] & 0x00FFFFFFu) | ((~sum & 0xFFu) << 24);
		}
		if (esp->beacons++ == 0) {
			GBASIORFUTrace(esp->rfu, "ESP32  first room beacon: device %04X, words %08X %08X %08X %08X %08X %08X", header & 0xFFFF, words[0], words[1],
			               words[2], words[3], words[4], words[5]);
		}
		_lockEnter(&esp->lock);
		esp->beaconSeen = true;
		esp->beaconAt = _ticks();
		esp->beaconDevice = (uint16_t) header;
		_lockLeave(&esp->lock);
		GBASIORFUBroadcastReceived(esp->rfu, (uint16_t) header, occupied ? 0xFF : 0, words);
	} else if (type == RFU1_CONNECT_ACK) {
		_lockEnter(&esp->lock);
		bool wasPending = esp->connectPending;
		uint16_t device = esp->connectDevice;
		if (wasPending) {
			esp->connectPending = false;
			esp->connected = true;
		}
		_lockLeave(&esp->lock);
		if (wasPending) {
			GBASIORFUTrace(esp->rfu, "ESP32  connect accepted by the board (our id %04X)", header & 0xFFFF);
			GBASIORFUConnectResult(esp->rfu, true, device, 0);
		}
	} else if (type == RFU1_HOST_SEND) {
		size_t length = header & 0x7F;
		if (length > kMaxPayload) {
			length = kMaxPayload;
		}
		if (esp->connected && available >= 12 + length) {
			GBASIORFUDataReceived(esp->rfu, 0, &rfu1[12], length);
		}
	} else if (type == RFU1_CONNECT_REQ) {
		// Hosting: the Switch asked to join the room. The driver (the game's own host state) decides, and answers through
		// connectReply(); the id it is given here stands for the Switch from now on.
		_lockEnter(&esp->lock);
		bool hosting = esp->hosting && !esp->hostClient;
		uint16_t clientId = 0;
		if (hosting) {
			clientId = (uint16_t) ((_ticks() & 0x7FFF) | 0x8000);
			if (clientId == esp->hostDeviceId) {
				clientId ^= 0x0101;
			}
			esp->pendingClientId = clientId;
		}
		_lockLeave(&esp->lock);
		if (hosting) {
			GBASIORFUTrace(esp->rfu, "ESP32  the Switch asks to join the group (header %08X): asking the game, client id %04X", header, clientId);
			GBASIORFUConnectRequested(esp->rfu, clientId);
		} else {
			GBASIORFUTrace(esp->rfu, "ESP32  join request from the board, but we are not advertising a group (or already have a client): refused");
			_sendRfu1(esp, RFU1_CONNECT_NACK, header & 0xFFFF, NULL, 0, 16);
		}
	} else if (type == RFU1_CLIENT_SEND) {
		size_t length = (header >> 24) & 0x7F;
		if (length > kMaxPayload) {
			length = kMaxPayload;
		}
		_lockEnter(&esp->lock);
		bool inRoom = esp->hostClient;
		unsigned slot = esp->hostClientSlot;
		_lockLeave(&esp->lock);
		if (inRoom && available >= 12 + length) {
			GBASIORFUDataReceived(esp->rfu, slot, &rfu1[12], length);
		}
	} else if (type == RFU1_DISCONNECT) {
		_lockEnter(&esp->lock);
		bool inRoom = esp->hostClient;
		unsigned slot = esp->hostClientSlot;
		esp->hostClient = false;
		esp->pendingClientId = 0;
		_lockLeave(&esp->lock);
		if (inRoom) {
			GBASIORFUTrace(esp->rfu, "ESP32  the Switch left the group");
			GBASIORFUDisconnected(esp->rfu, (int) slot);
		}
	} else if (type == RFU1_CLIENT_ACK) {
		// The board's keep-alive for the client's slot; nothing to do.
	}
}

static void _pumpOnce(struct GBASIORFUESP32* esp, bool* failed) {
	uint8_t buffer[512];
	int got = Esp32SerialRead(esp->port, buffer, sizeof(buffer));
	if (got < 0) {
		*failed = true;
		return;
	}
	esp->bytesRead += (unsigned) got;
	for (int i = 0; i < got; ++i) {
		struct Esp32WireFrame frame;
		if (Esp32WireFeed(&esp->parser, buffer[i], &frame)) {
			_handleFrame(esp, &frame);
		}
	}
	if (!got) {
		_espSleep(2);
	}
}

// Reads and parses for `ms`, then reports whether a response/event frame starting with `prefix` was seen (a NULL
// prefix just pumps).
static bool _await(struct GBASIORFUESP32* esp, const char* prefix, unsigned ms) {
	uint32_t end = _ticks() + ms;
	esp->awaitPrefix = prefix;
	esp->awaitMatched = false;
	bool failed = false;
	while (!esp->stop && !failed && (int32_t) (end - _ticks()) > 0) {
		_pumpOnce(esp, &failed);
		if (esp->awaitMatched) {
			break;
		}
	}
	bool matched = esp->awaitMatched;
	esp->awaitPrefix = NULL;
	esp->awaitMatched = false;
	return matched;
}

static bool _waitMs(struct GBASIORFUESP32* esp, unsigned ms) {
	uint32_t end = _ticks() + ms;
	while (!esp->stop && (int32_t) (end - _ticks()) > 0) {
		_espSleep(20);
	}
	return !esp->stop;
}

static bool _negotiate(struct GBASIORFUESP32* esp);

static bool _handshake(struct GBASIORFUESP32* esp) {
	const char* configured = esp->configuredPort[0] ? esp->configuredPort : getenv("MGBA_RFU_ESP32_PORT");
	if (configured && configured[0]) {
		snprintf(esp->portName, sizeof(esp->portName), "%s", configured);
	} else if (!Esp32SerialFindEspressif(esp->portName, sizeof(esp->portName))) {
		return false;
	}
	esp->port = Esp32SerialOpen(esp->portName, 921600);
	if (!esp->port) {
		char error[64];
		snprintf(error, sizeof(error), "Could not open %s", esp->portName);
		_setError(esp, error);
		return false;
	}
	GBASIORFUTrace(esp->rfu, "ESP32  opened %s (the board resets when its port is opened; waiting for it to boot)", esp->portName);
	return _negotiate(esp);
}

// Everything after the port is open: wait out the boot, then binary mode, session and adapter-host mode. Also used on its
// own after the board rebooted itself (boardRebooting), on the port that is still open.
static bool _negotiate(struct GBASIORFUESP32* esp) {
	_setPhase(esp, ESP_PHASE_BOOTING);
	Esp32WireParserInit(&esp->parser);

	// Let the board boot (its console text is discarded), then switch it to binary mode.
	uint32_t end = _ticks() + kBootWaitMs;
	bool failed = false;
	while (!esp->stop && !failed && (int32_t) (end - _ticks()) > 0) {
		uint8_t discard[256];
		if (Esp32SerialRead(esp->port, discard, sizeof(discard)) <= 0) {
			_espSleep(20);
		}
	}
	if (esp->stop) {
		return false;
	}
	static const uint8_t kBinary[] = "\nLDN_BINARY\n";
	Esp32SerialWrite(esp->port, kBinary, sizeof(kBinary) - 1);
	const uint8_t nul = 0;
	Esp32SerialWrite(esp->port, &nul, 1);
	Esp32WireParserInit(&esp->parser);
	_setPhase(esp, ESP_PHASE_HANDSHAKE);
	esp->bytesRead = 0;
	esp->rxUsed = 0;
	esp->session = 0;
	// The board announces the mode switch with an unsolicited "LDN_HELLO ..." event; esp32-probe read for a second
	// here before sending anything, and that is the sequence proven against a real board.
	bool hello = _await(esp, "LDN_HELLO", 1500);
	for (int attempt = 0; !hello && attempt < 3 && !esp->stop; ++attempt) {
		_command(esp, "LDN_HELLO");
		hello = _await(esp, "LDN_HELLO", 2000);
	}
	if (!hello) {
		GBASIORFUTrace(esp->rfu, "ESP32  no LDN_HELLO reply (read %u bytes, %u frames ok / %u bad, last text \"%s\") - is this the GB-Link bridge firmware?",
		               esp->bytesRead, esp->parser.framesOk, esp->parser.framesBad, esp->lastText);
		_setError(esp, "No reply from the board - is this the GB-Link bridge firmware?");
		return false;
	}
	uint32_t session = (_ticks() ^ 0x5A5A1234u) | 1u;
	char text[48];
	snprintf(text, sizeof(text), "LDN_BEGIN %08X", session);
	_command(esp, text);
	if (!_await(esp, "LDN_BEGUN", 3000)) {
		GBASIORFUTrace(esp->rfu, "ESP32  LDN_BEGIN failed");
		_setError(esp, "The board did not start a session (LDN_BEGIN failed)");
		return false;
	}
	esp->session = session;
	_command(esp, "LDN_BRIDGE_START");
	_await(esp, "LDN_BRIDGE_STARTED", 2000);
	_command(esp, "LDN_ADAPTER host");
	if (!_await(esp, "LDN_ADAPTER host", 2000)) {
		GBASIORFUTrace(esp->rfu, "ESP32  LDN_ADAPTER host failed (needs bridge firmware 2.0 or later)");
		_setError(esp, "Adapter-host mode failed (needs bridge firmware 2.0 or later)");
		return false;
	}
	GBASIORFUTrace(esp->rfu, "ESP32  board is in adapter-host mode");
	return true;
}

static void _picoReport(struct GBASIORFUESP32* esp) {
	static const uint8_t status[] = {0x02, 0xFF};
	static const uint8_t active[] = {0x0E, 0x01};
	uint8_t gb[16];
	size_t length = Esp32GbFrameBuild(GB_CHANNEL_STATUS, status, sizeof(status), gb, sizeof(gb));
	_writeFrame(esp, ESP32_TYPE_GB_STREAM, 0, gb, length);
	length = Esp32GbFrameBuild(GB_CHANNEL_DATA, active, sizeof(active), gb, sizeof(gb));
	_writeFrame(esp, ESP32_TYPE_GB_STREAM, 0, gb, length);
}

static void _run(struct GBASIORFUESP32* esp) {
	uint32_t nextReport = _ticks();
	uint32_t nextPing = _ticks() + kPingMs;
	esp->nextBeacon = _ticks();
	bool failed = false;
	while (!esp->stop && !failed && !esp->boardRebooting) {
		_pumpOnce(esp, &failed);

		uint32_t now = _ticks();
		if ((int32_t) (now - nextReport) >= 0) {
			_picoReport(esp);
			nextReport = now + kPicoReportMs;
		}
		if ((int32_t) (now - nextPing) >= 0) {
			_command(esp, "LDN_PING");
			nextPing = now + kPingMs;
		}
		if ((int32_t) (now - esp->nextBeacon) >= 0) {
			// Hosting: the game's group, until a client is in. This is what makes the board open a room for the Switch.
			esp->nextBeacon = now + kBeaconMs;
			_lockEnter(&esp->lock);
			bool advertise = esp->hosting && esp->haveBroadcast && !esp->hostClient;
			uint16_t deviceId = esp->hostDeviceId;
			uint32_t words[RFU_BROADCAST_WORDS];
			memcpy(words, esp->broadcastWords, sizeof(words));
			_lockLeave(&esp->lock);
			if (advertise) {
				uint8_t body[4 * RFU_BROADCAST_WORDS];
				for (int i = 0; i < RFU_BROADCAST_WORDS; ++i) {
					_putBe32(&body[i * 4], words[i]);
				}
				if (esp->beaconsSent++ == 0) {
					GBASIORFUTrace(esp->rfu, "ESP32  advertising the game's group: device %04X, words %08X %08X %08X %08X %08X %08X", deviceId, words[0],
					               words[1], words[2], words[3], words[4], words[5]);
				}
				_sendRfu1(esp, RFU1_BROADCAST, deviceId, body, sizeof(body), 36);
			}
		}
		if (esp->bridgeRestart && (int32_t) (now - esp->bridgeRestartAt) >= 0) {
			esp->bridgeRestart = false;
			++esp->bridgeRestarts;
			GBASIORFUTrace(esp->rfu, "ESP32  the board's bridge stopped: starting it again (attempt %u)", esp->bridgeRestarts);
			_command(esp, "LDN_BRIDGE_START");
		}

		// Anything the emulation thread queued, and the connect timeout.
		for (;;) {
			uint8_t frame[kOutBytes];
			size_t length = 0;
			_lockEnter(&esp->lock);
			if (esp->outCount) {
				length = esp->out[esp->outHead].length;
				memcpy(frame, esp->out[esp->outHead].data, length);
				esp->outHead = (esp->outHead + 1) % kOutSlots;
				--esp->outCount;
			}
			_lockLeave(&esp->lock);
			if (!length) {
				break;
			}
			_writeFrame(esp, ESP32_TYPE_GB_STREAM, 0, frame, length);
		}
		_lockEnter(&esp->lock);
		bool timedOut = esp->connectPending && (int32_t) (now - esp->connectDeadline) >= 0;
		uint16_t device = esp->connectDevice;
		if (timedOut) {
			esp->connectPending = false;
		}
		_lockLeave(&esp->lock);
		if (timedOut) {
			GBASIORFUTrace(esp->rfu, "ESP32  connect to %04X timed out (no CONNECT_ACK from the board)", device);
			GBASIORFUConnectResult(esp->rfu, false, device, 0);
		}
	}
}

ESP_THREAD_FUNC(_thread) {
	struct GBASIORFUESP32* esp = context;
	bool reportedMissing = false;
	while (!esp->stop) {
		const bool rebooted = esp->boardRebooting && esp->port;
		esp->boardRebooting = false;
		if (rebooted ? _negotiate(esp) : _handshake(esp)) {
			reportedMissing = false;
			_setError(esp, "");
			_setPhase(esp, ESP_PHASE_READY);
			_flagSet(&esp->ready, 1);
			_run(esp);
			_flagSet(&esp->ready, 0);
			// The board went away (or was restarted) with the game still leading a group: the client it had is gone too.
			_lockEnter(&esp->lock);
			bool hadClient = esp->hostClient;
			unsigned clientSlot = esp->hostClientSlot;
			esp->hostClient = false;
			esp->pendingClientId = 0;
			_lockLeave(&esp->lock);
			if (hadClient) {
				GBASIORFUTrace(esp->rfu, "ESP32  the board went away: the Switch's session with the game is over");
				GBASIORFUDisconnected(esp->rfu, (int) clientSlot);
			}
			if (esp->boardRebooting && !esp->stop) {
				++esp->boardReboots;
				GBASIORFUTrace(esp->rfu, "ESP32  the board's bridge stopped and the board is rebooting: setting it up again on %s (reboot %u)",
				               esp->portName, esp->boardReboots);
				continue;
			}
			if (!esp->stop) {
				char error[64];
				snprintf(error, sizeof(error), "Lost the board on %s", esp->portName);
				_setError(esp, error);
			}
			if (esp->port) {
				// Hand the board back to its standalone GBA-cable mode.
				_command(esp, "LDN_ADAPTER uart");
				_espSleep(100);
			}
		} else if (!esp->stop && !esp->port && !reportedMissing) {
			// A named port that exists but will not open is held by another program (the GB-Link web page, pokeldn, ...),
			// which is not the same as no board at all.
			_lockEnter(&esp->lock);
			char error[RFU_BACKEND_STATUS_TEXT];
			snprintf(error, sizeof(error), "%s", esp->statusError);
			_lockLeave(&esp->lock);
			if (!strncmp(error, "Could not open", 14)) {
				GBASIORFUTrace(esp->rfu, "ESP32  %s: is another program (the GB-Link web page, pokeldn, a serial monitor) using it? Retrying", error);
			} else {
				GBASIORFUTrace(esp->rfu, "ESP32  no board found (looking for an Espressif USB serial port; set MGBA_RFU_ESP32_PORT to name one)");
			}
			reportedMissing = true;
		}
		_setPhase(esp, reportedMissing ? ESP_PHASE_MISSING : ESP_PHASE_LOOKING);
		if (esp->port) {
			Esp32SerialClose(esp->port);
			esp->port = NULL;
		}
		_waitMs(esp, kRetryMs);
	}
	ESP_THREAD_RETURN;
}

// ---- backend hooks (emulation thread) -------------------------------------------------------------------------

static bool _init(struct GBASIORFUBackend* backend, struct GBASIORFU* rfu) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	esp->rfu = rfu;
	esp->stop = 0;
	esp->threadValid = _threadStart(&esp->thread, _thread, esp);
	GBASIORFUTrace(rfu, "ESP32  backend attached");
	return esp->threadValid;
}

static void _deinit(struct GBASIORFUBackend* backend) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	_flagSet(&esp->stop, 1);
	if (esp->threadValid) {
		_threadJoin(&esp->thread);
		esp->threadValid = false;
	}
	_lockDelete(&esp->lock);
}

// Tells the board its client left (host role); the id and slot are the ones the board was given in CONNECT_ACK.
static void _dropHostClient(struct GBASIORFUESP32* esp) {
	_lockEnter(&esp->lock);
	bool hadClient = esp->hostClient;
	uint32_t header = (uint32_t) esp->hostClientId | ((uint32_t) esp->hostClientSlot << 16);
	esp->hostClient = false;
	esp->pendingClientId = 0;
	_lockLeave(&esp->lock);
	if (hadClient) {
		_sendRfu1(esp, RFU1_DISCONNECT, header, NULL, 0, 16);
	}
}

static void _reset(struct GBASIORFUBackend* backend) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	_lockEnter(&esp->lock);
	bool wasConnected = esp->connected;
	esp->connected = false;
	esp->connectPending = false;
	esp->hosting = false;
	esp->haveBroadcast = false;
	_lockLeave(&esp->lock);
	if (wasConnected) {
		_sendRfu1(esp, RFU1_DISCONNECT, 0, NULL, 0, 16);
	}
	_dropHostClient(esp);
}

// ---- host role ----

// The 24 bytes of game data the leader advertises (sent to the board as they change and every 500ms).
static void _setBroadcast(struct GBASIORFUBackend* backend, const uint32_t data[RFU_BROADCAST_WORDS]) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	_lockEnter(&esp->lock);
	memcpy(esp->broadcastWords, data, sizeof(esp->broadcastWords));
	esp->haveBroadcast = true;
	_lockLeave(&esp->lock);
}

static void _hostStart(struct GBASIORFUBackend* backend, uint16_t deviceId) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	_lockEnter(&esp->lock);
	esp->hosting = true;
	esp->hostDeviceId = deviceId;
	esp->hostClient = false;
	esp->beaconsSent = 0;
	_lockLeave(&esp->lock);
	GBASIORFUTrace(esp->rfu, "ESP32  the game leads a group as device %04X (%s)", deviceId,
	               esp->ready ? "advertising it to the board" : "the board is not ready yet");
}

// Advertising stops; a client that is already in stays (the driver keeps the slot too).
static void _hostStop(struct GBASIORFUBackend* backend) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	_lockEnter(&esp->lock);
	esp->hosting = false;
	_lockLeave(&esp->lock);
	GBASIORFUTrace(esp->rfu, "ESP32  the game stopped advertising its group");
}

// The driver's answer to the board's join request.
static void _connectReply(struct GBASIORFUBackend* backend, uint16_t clientId, bool accepted, unsigned slot) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	_lockEnter(&esp->lock);
	bool current = esp->pendingClientId == clientId;
	if (current) {
		esp->pendingClientId = 0;
		if (accepted) {
			esp->hostClient = true;
			esp->hostClientId = clientId;
			esp->hostClientSlot = (uint8_t) (slot & 3);
		}
	}
	_lockLeave(&esp->lock);
	if (!current) {
		return;
	}
	if (accepted) {
		GBASIORFUTrace(esp->rfu, "ESP32  the game accepted the Switch as client %u (id %04X)", slot & 3, clientId);
		_sendRfu1(esp, RFU1_CONNECT_ACK, (uint32_t) clientId | ((uint32_t) (slot & 3) << 16), NULL, 0, 16);
	} else {
		GBASIORFUTrace(esp->rfu, "ESP32  the game refused the Switch");
		_sendRfu1(esp, RFU1_CONNECT_NACK, clientId, NULL, 0, 16);
	}
}

static void _noop(struct GBASIORFUBackend* backend) {
	(void) backend;
}

static void _connect(struct GBASIORFUBackend* backend, uint16_t deviceId) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	if (!esp->ready) {
		GBASIORFUTrace(esp->rfu, "ESP32  connect to %04X requested, but the board is not ready", deviceId);
		GBASIORFUConnectResult(esp->rfu, false, deviceId, 0);
		return;
	}
	_lockEnter(&esp->lock);
	esp->connectPending = true;
	esp->connected = false;
	esp->connectDevice = deviceId;
	esp->connectDeadline = _ticks() + kConnectTimeoutMs;
	_lockLeave(&esp->lock);
	GBASIORFUTrace(esp->rfu, "ESP32  connect request for %04X sent to the board", deviceId);
	_sendRfu1(esp, RFU1_CONNECT_REQ, deviceId, NULL, 0, 16);
}

// Client role: leave the host. Host role: bit N of slotMask drops client slot N (the board keeps its room while it tears
// the Switch's session down).
static void _disconnect(struct GBASIORFUBackend* backend, unsigned slotMask) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	_lockEnter(&esp->lock);
	bool hostSlotDropped = esp->hostClient && (slotMask & (1u << esp->hostClientSlot));
	bool wasConnected = esp->connected || esp->connectPending;
	esp->connected = false;
	esp->connectPending = false;
	_lockLeave(&esp->lock);
	if (hostSlotDropped) {
		GBASIORFUTrace(esp->rfu, "ESP32  the game dropped the Switch");
		_dropHostClient(esp);
	}
	if (wasConnected) {
		GBASIORFUTrace(esp->rfu, "ESP32  disconnect");
		_sendRfu1(esp, RFU1_DISCONNECT, 0, NULL, 0, 16);
	}
}

// Client role: the game's client slot (LLSF header + payload, as it would have handed a real adapter): RFU1 CLIENT_SEND
// carries the length in the header's top byte and up to 92 payload bytes. Host role: the leader's frame for its client
// goes as HOST_SEND, the length in the header's low bits.
static void _sendData(struct GBASIORFUBackend* backend, const uint8_t* data, size_t length) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	_lockEnter(&esp->lock);
	bool hostClient = esp->hostClient;
	bool connected = esp->connected;
	_lockLeave(&esp->lock);
	if (length > kMaxPayload) {
		length = kMaxPayload;
	}
	if (hostClient) {
		_sendRfu1(esp, RFU1_HOST_SEND, (uint32_t) (length & 0x7F), data, length, 104);
	} else if (connected) {
		_sendRfu1(esp, RFU1_CLIENT_SEND, (uint32_t) length << 24, data, length, 104);
	}
}

// Any thread.
static void _status(struct GBASIORFUBackend* backend, struct GBASIORFUBackendStatus* out) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	uint32_t now = _ticks();
	_lockEnter(&esp->lock);
	bool beacon = esp->beaconSeen && now - esp->beaconAt < kBeaconFreshMs;
	out->hostsHeard = beacon ? 1 : 0;
	if (esp->statusPort[0]) {
		snprintf(out->device, sizeof(out->device), "ESP32 board on %s", esp->statusPort);
	} else if (esp->configuredPort[0]) {
		snprintf(out->device, sizeof(out->device), "ESP32 board on %s", esp->configuredPort);
	} else {
		snprintf(out->device, sizeof(out->device), "ESP32 board (auto-detect)");
	}
	snprintf(out->lastError, sizeof(out->lastError), "%s", esp->statusError);
	switch (esp->phase) {
	case ESP_PHASE_LOOKING:
		out->link = RFU_BACKEND_STARTING;
		snprintf(out->detail, sizeof(out->detail), "Looking for the board");
		break;
	case ESP_PHASE_MISSING:
		out->link = RFU_BACKEND_UNAVAILABLE;
		snprintf(out->detail, sizeof(out->detail), "No board found; retrying");
		break;
	case ESP_PHASE_BOOTING:
		out->link = RFU_BACKEND_STARTING;
		snprintf(out->detail, sizeof(out->detail), "Waiting for the board to boot");
		break;
	case ESP_PHASE_HANDSHAKE:
		out->link = RFU_BACKEND_STARTING;
		snprintf(out->detail, sizeof(out->detail), "Handshaking with the board");
		break;
	case ESP_PHASE_READY:
		if (esp->connected) {
			out->link = RFU_BACKEND_JOINED;
			snprintf(out->detail, sizeof(out->detail), "In a session with %04X", esp->connectDevice);
		} else if (esp->connectPending) {
			out->link = RFU_BACKEND_JOINING;
			snprintf(out->detail, sizeof(out->detail), "Joining %04X", esp->connectDevice);
		} else {
			out->link = RFU_BACKEND_READY;
			if (beacon) {
				snprintf(out->detail, sizeof(out->detail), "Room %04X in range", esp->beaconDevice);
			} else if (esp->statusEvent[0]) {
				snprintf(out->detail, sizeof(out->detail), "Searching (%s)", esp->statusEvent);
			} else {
				snprintf(out->detail, sizeof(out->detail), "Searching");
			}
		}
		break;
	}
	_lockLeave(&esp->lock);
}

struct GBASIORFUBackend* GBASIORFUESP32Create(void) {
	struct GBASIORFUESP32* esp = calloc(1, sizeof(*esp));
	if (!esp) {
		return NULL;
	}
	_lockInit(&esp->lock);
	esp->d.init = _init;
	esp->d.deinit = _deinit;
	esp->d.reset = _reset;
	esp->d.setBroadcast = _setBroadcast;
	esp->d.hostStart = _hostStart;
	esp->d.hostStop = _hostStop;
	esp->d.connectReply = _connectReply;
	esp->d.searchStart = _noop;
	esp->d.searchStop = _noop;
	esp->d.connect = _connect;
	esp->d.disconnect = _disconnect;
	esp->d.sendData = _sendData;
	esp->d.status = _status;
	return &esp->d;
}

void GBASIORFUESP32SetPort(struct GBASIORFUBackend* backend, const char* port) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	if (!port) {
		esp->configuredPort[0] = 0;
		return;
	}
	snprintf(esp->configuredPort, sizeof(esp->configuredPort), "%s", port);
}

size_t GBASIORFUESP32ListPorts(struct GBASIORFUESP32Port* out, size_t max) {
	struct Esp32SerialPortInfo found[16];
	size_t count = Esp32SerialListEspressif(found, max < 16 ? max : 16);
	for (size_t i = 0; i < count; ++i) {
		snprintf(out[i].name, sizeof(out[i].name), "%s", found[i].name);
		snprintf(out[i].description, sizeof(out[i].description), "%s", found[i].description);
	}
	return count;
}
