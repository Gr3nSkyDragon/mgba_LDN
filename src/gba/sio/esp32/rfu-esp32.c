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

	RFU1_BROADCAST = 0,
	RFU1_CONNECT_REQ = 1,
	RFU1_CONNECT_ACK = 2,
	RFU1_DISCONNECT = 4,
	RFU1_HOST_SEND = 5,
	RFU1_CLIENT_SEND = 6,

	kMaxPayload = 92,
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

	// I/O thread only.
	struct Esp32Serial* port;
	struct Esp32WireParser parser;
	uint32_t request;
	uint32_t session;
	const char* awaitPrefix; // while set, a response or event frame whose text starts with it sets awaitMatched
	bool awaitMatched;
	char lastText[64];
	// The board joins the first room it sees, once per LDN_BRIDGE_START, and stops after a failed or ended join. The
	// I/O thread starts it again a moment later, so a join that timed out (radio, a busy channel, the Switch not ready
	// yet) is retried instead of leaving the emulator searching an idle board for good.
	bool bridgeRestart;
	uint32_t bridgeRestartAt;
	unsigned bridgeRestarts;
	unsigned bytesRead;
	unsigned beacons;
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
		} else if (frame->type == ESP32_TYPE_EVENT &&
		           (!strncmp(text, "LDN_ROOM", 8) || !strncmp(text, "LDN_LINK", 8) || !strncmp(text, "LDN_BRIDGE", 10) ||
		            !strncmp(text, "LDN_NET_LOST", 12))) {
			GBASIORFUTrace(esp->rfu, "ESP32  event: %s", text);
			if (!esp->stop && (!strncmp(text, "LDN_BRIDGE stopped", 18) || !strncmp(text, "LDN_BRIDGE join timed out", 25) ||
			                   !strncmp(text, "LDN_NET_LOST", 12))) {
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
		if (esp->beacons++ == 0) {
			GBASIORFUTrace(esp->rfu, "ESP32  first room beacon: device %04X, words %08X %08X %08X %08X %08X %08X", header & 0xFFFF, words[0], words[1],
			               words[2], words[3], words[4], words[5]);
		}
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

static bool _handshake(struct GBASIORFUESP32* esp) {
	const char* configured = esp->configuredPort[0] ? esp->configuredPort : getenv("MGBA_RFU_ESP32_PORT");
	if (configured && configured[0]) {
		snprintf(esp->portName, sizeof(esp->portName), "%s", configured);
	} else if (!Esp32SerialFindEspressif(esp->portName, sizeof(esp->portName))) {
		return false;
	}
	esp->port = Esp32SerialOpen(esp->portName, 921600);
	if (!esp->port) {
		return false;
	}
	GBASIORFUTrace(esp->rfu, "ESP32  opened %s (the board resets when its port is opened; waiting for it to boot)", esp->portName);
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
		return false;
	}
	uint32_t session = (_ticks() ^ 0x5A5A1234u) | 1u;
	char text[48];
	snprintf(text, sizeof(text), "LDN_BEGIN %08X", session);
	_command(esp, text);
	if (!_await(esp, "LDN_BEGUN", 3000)) {
		GBASIORFUTrace(esp->rfu, "ESP32  LDN_BEGIN failed");
		return false;
	}
	esp->session = session;
	_command(esp, "LDN_BRIDGE_START");
	_await(esp, "LDN_BRIDGE_STARTED", 2000);
	_command(esp, "LDN_ADAPTER host");
	if (!_await(esp, "LDN_ADAPTER host", 2000)) {
		GBASIORFUTrace(esp->rfu, "ESP32  LDN_ADAPTER host failed (needs bridge firmware 2.0 or later)");
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
	bool failed = false;
	while (!esp->stop && !failed) {
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
		if (_handshake(esp)) {
			reportedMissing = false;
			_flagSet(&esp->ready, 1);
			_run(esp);
			_flagSet(&esp->ready, 0);
			if (esp->port) {
				// Hand the board back to its standalone GBA-cable mode.
				_command(esp, "LDN_ADAPTER uart");
				_espSleep(100);
			}
		} else if (!esp->stop && !esp->port && !reportedMissing) {
			GBASIORFUTrace(esp->rfu, "ESP32  no board found (looking for an Espressif USB serial port; set MGBA_RFU_ESP32_PORT to name one)");
			reportedMissing = true;
		}
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

static void _reset(struct GBASIORFUBackend* backend) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	_lockEnter(&esp->lock);
	bool wasConnected = esp->connected;
	esp->connected = false;
	esp->connectPending = false;
	_lockLeave(&esp->lock);
	if (wasConnected) {
		_sendRfu1(esp, RFU1_DISCONNECT, 0, NULL, 0, 16);
	}
}

static void _noop(struct GBASIORFUBackend* backend) {
	(void) backend;
}

static void _noopBroadcast(struct GBASIORFUBackend* backend, const uint32_t data[RFU_BROADCAST_WORDS]) {
	(void) backend;
	(void) data;
}

static void _noopDeviceId(struct GBASIORFUBackend* backend, uint16_t deviceId) {
	(void) backend;
	(void) deviceId;
}

static void _noopReply(struct GBASIORFUBackend* backend, uint16_t clientId, bool accepted, unsigned slot) {
	(void) backend;
	(void) clientId;
	(void) accepted;
	(void) slot;
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

static void _disconnect(struct GBASIORFUBackend* backend, unsigned slotMask) {
	(void) slotMask;
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	_lockEnter(&esp->lock);
	bool wasConnected = esp->connected || esp->connectPending;
	esp->connected = false;
	esp->connectPending = false;
	_lockLeave(&esp->lock);
	if (wasConnected) {
		GBASIORFUTrace(esp->rfu, "ESP32  disconnect");
		_sendRfu1(esp, RFU1_DISCONNECT, 0, NULL, 0, 16);
	}
}

// The game's client slot (LLSF header + payload, as it would have handed a real adapter): RFU1 CLIENT_SEND carries
// the length in the header's top byte and up to 92 payload bytes.
static void _sendData(struct GBASIORFUBackend* backend, const uint8_t* data, size_t length) {
	struct GBASIORFUESP32* esp = (struct GBASIORFUESP32*) backend;
	if (!esp->connected) {
		return;
	}
	if (length > kMaxPayload) {
		length = kMaxPayload;
	}
	_sendRfu1(esp, RFU1_CLIENT_SEND, (uint32_t) length << 24, data, length, 104);
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
	esp->d.setBroadcast = _noopBroadcast;
	esp->d.hostStart = _noopDeviceId;
	esp->d.hostStop = _noop;
	esp->d.connectReply = _noopReply;
	esp->d.searchStart = _noop;
	esp->d.searchStop = _noop;
	esp->d.connect = _connect;
	esp->d.disconnect = _disconnect;
	esp->d.sendData = _sendData;
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
