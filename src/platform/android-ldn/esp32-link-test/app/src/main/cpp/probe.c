// The ESP32 link session from mGBA's rfu-esp32.c / esp32-probe.c, made portable: same handshake, same "Pico"
// reports, same RFU1 decoding, no emulator. Everything goes through the Esp32Serial* functions, so on Android it
// exercises the Java-backed implementation installed with Esp32SerialSetOps.
#include "probe.h"

#include "esp32-serial.h"
#include "esp32-wire.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void (*gLog)(const char*);
static volatile bool gStop;
static struct Esp32Serial* gPort;
static struct Esp32WireParser gParser;
static uint32_t gRequest;
static uint32_t gSession;

static struct {
	unsigned frames[8];
	unsigned responses;
	unsigned beacons;
	bool joined;
	bool haveBeacon;
	char beaconName[16];
	unsigned beaconId;
} gResult;

// GB channel-1 bytes are an RFU1 byte stream cut into 64-byte chunks (see rfu-esp32.c); reassembled here.
static uint8_t gRx[256];
static size_t gRxUsed;

static void LOG(const char* fmt, ...) {
	char line[600];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	gLog(line);
}

static unsigned long long _now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long) ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
}

static uint32_t _be32(const uint8_t* p) {
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static void _text(const uint8_t* data, size_t length, char* out, size_t capacity) {
	size_t n = 0;
	for (size_t i = 0; i < length && n + 1 < capacity; ++i) {
		out[n++] = data[i] >= 0x20 && data[i] < 0x7F ? (char) data[i] : '.';
	}
	out[n] = 0;
}

static char _frlgChar(uint8_t c) {
	if (c >= 0xBB && c <= 0xD4) return (char) ('A' + c - 0xBB);
	if (c >= 0xD5 && c <= 0xEE) return (char) ('a' + c - 0xD5);
	if (c >= 0xA1 && c <= 0xAA) return (char) ('0' + c - 0xA1);
	return c == 0 ? ' ' : '?';
}

static int _rfu1Size(uint32_t type) {
	switch (type) {
	case 0: return 36;
	case 1: case 2: case 4: return 16;
	case 5: case 6: return 104;
	default: return 0;
	}
}

static void _rfu1(const uint8_t* p) {
	static const char* const names[] = {"BROADCAST", "CONNECT_REQ", "CONNECT_ACK", "?3", "DISCONNECT", "HOST_SEND", "CLIENT_SEND"};
	uint32_t type = _be32(p + 4);
	uint32_t header = _be32(p + 8);
	if (type == 0) {
		++gResult.beacons;
		if (!gResult.haveBeacon) {
			gResult.haveBeacon = true;
			gResult.beaconId = header & 0xFFFF;
			uint8_t name[8];
			for (int i = 0; i < 4; ++i) {
				name[i] = (uint8_t) (_be32(p + 12 + 16) >> (8 * i));
				name[4 + i] = (uint8_t) (_be32(p + 12 + 20) >> (8 * i));
			}
			size_t n = 0;
			for (; n < 8 && name[n] != 0xFF; ++n) {
				gResult.beaconName[n] = _frlgChar(name[n]);
			}
			gResult.beaconName[n] = 0;
			LOG("BEACON: first room beacon, device %04X, host name \"%s\"", gResult.beaconId, gResult.beaconName);
		}
	} else {
		LOG("RFU1 %s(%u) header=%08X", type < 7 ? names[type] : "?", type, header);
	}
}

static void _gbChunk(const uint8_t* data, size_t length) {
	if (gRxUsed + length > sizeof(gRx)) {
		gRxUsed = 0;
	}
	memcpy(gRx + gRxUsed, data, length);
	gRxUsed += length;
	size_t at = 0;
	while (gRxUsed - at >= 12) {
		if (memcmp(gRx + at, "RFU1", 4)) {
			++at;
			continue;
		}
		int size = _rfu1Size(_be32(gRx + at + 4));
		if (!size) {
			++at;
			continue;
		}
		if (gRxUsed - at < (size_t) size) {
			break;
		}
		_rfu1(gRx + at);
		at += (size_t) size;
	}
	if (at) {
		memmove(gRx, gRx + at, gRxUsed - at);
		gRxUsed -= at;
	}
}

static void _frame(const struct Esp32WireFrame* frame) {
	if (frame->type < 8) {
		++gResult.frames[frame->type];
	}
	if (frame->type == ESP32_TYPE_GB_FRAME) {
		// Only the data channel carries RFU1 (the board's own "set mode" command repeats on the command channel). The
		// board sends it as a byte stream in chunks of UP TO 64 bytes (a 36-byte beacon is one 36-byte chunk); only the
		// host-to-board direction pads to exactly 64.
		if (frame->payloadLength > GB_HEADER && frame->payload[0] == 0x47 && frame->payload[1] == 0x42 &&
		    frame->payload[2] == GB_CHANNEL_DATA) {
			_gbChunk(frame->payload + GB_HEADER, frame->payloadLength - GB_HEADER);
		}
		return;
	}
	char text[300];
	_text(frame->payload, frame->payloadLength, text, sizeof(text));
	if (frame->type == ESP32_TYPE_RESPONSE) {
		++gResult.responses;
	}
	if (frame->type == ESP32_TYPE_RESPONSE || frame->type == ESP32_TYPE_EVENT) {
		LOG("board %s: %s", frame->type == ESP32_TYPE_RESPONSE ? "reply" : "event", text);
		if (strstr(text, "joined room")) {
			gResult.joined = true;
		}
	}
}

static bool _pump(unsigned ms, bool rawText) {
	unsigned long long end = _now() + ms;
	char rawLine[200];
	size_t rawUsed = 0;
	while (!gStop && _now() < end) {
		uint8_t buffer[512];
		int got = Esp32SerialRead(gPort, buffer, sizeof(buffer));
		if (got < 0) {
			LOG("ERROR: serial read failed (device unplugged or permission lost)");
			return false;
		}
		for (int i = 0; i < got; ++i) {
			if (rawText) {
				uint8_t c = buffer[i];
				if (c == '\n' || rawUsed + 1 >= sizeof(rawLine)) {
					if (rawUsed) {
						rawLine[rawUsed] = 0;
						LOG("boot: %s", rawLine);
						if (strstr(rawLine, "joined room")) {
							gResult.joined = true;
						}
					}
					rawUsed = 0;
				}
				if (c >= 0x20 && c < 0x7F) {
					rawLine[rawUsed++] = (char) c;
				}
				continue;
			}
			struct Esp32WireFrame frame;
			if (Esp32WireFeed(&gParser, buffer[i], &frame)) {
				_frame(&frame);
			}
		}
		if (!got) {
			usleep(2000);
		}
	}
	if (rawText && rawUsed) {
		rawLine[rawUsed] = 0;
		LOG("boot: %s", rawLine);
	}
	return true;
}

static bool _command(const char* text) {
	uint8_t out[512];
	size_t length = Esp32WireEncode(ESP32_TYPE_COMMAND, ++gRequest, gSession, text, strlen(text), out, sizeof(out));
	if (strcmp(text, "LDN_PING")) {
		LOG("send: %s", text);
	}
	return length && Esp32SerialWrite(gPort, out, length);
}

// Plays the GB-Link Pico for the board (see rfu-esp32.c _picoReport): status 0xFF02 and "GBA active" twice a second.
static void _picoReport(void) {
	static const uint8_t status[] = {0x02, 0xFF};
	static const uint8_t active[] = {0x0E, 0x01};
	uint8_t gb[16];
	uint8_t out[128];
	size_t length = Esp32GbFrameBuild(GB_CHANNEL_STATUS, status, sizeof(status), gb, sizeof(gb));
	size_t encoded = Esp32WireEncode(ESP32_TYPE_GB_STREAM, 0, gSession, gb, length, out, sizeof(out));
	if (encoded) {
		Esp32SerialWrite(gPort, out, encoded);
	}
	length = Esp32GbFrameBuild(GB_CHANNEL_DATA, active, sizeof(active), gb, sizeof(gb));
	encoded = Esp32WireEncode(ESP32_TYPE_GB_STREAM, 0, gSession, gb, length, out, sizeof(out));
	if (encoded) {
		Esp32SerialWrite(gPort, out, encoded);
	}
}

void probe_stop(void) {
	gStop = true;
}

int probe_run(unsigned seconds, void (*log)(const char* line)) {
	gLog = log;
	gStop = false;
	gRequest = 0;
	gSession = 0;
	gRxUsed = 0;
	memset(&gResult, 0, sizeof(gResult));
	int rc = 1;

	char name[64];
	if (!Esp32SerialFindEspressif(name, sizeof(name))) {
		LOG("FAIL: no ESP32 (Espressif USB Serial/JTAG, 303A:1001) found. Plug the board's USB port into the phone.");
		return 2;
	}
	gPort = Esp32SerialOpen(name, 921600);
	if (!gPort) {
		LOG("FAIL: could not open the device (permission refused, or another app has it).");
		return 3;
	}
	LOG("opened %s. Opening the port resets the board; waiting for it to boot (4.5 s)...", name);
	Esp32WireParserInit(&gParser);
	if (!_pump(4500, true)) {
		goto done;
	}

	static const uint8_t kBinary[] = "\nLDN_BINARY\n";
	Esp32SerialWrite(gPort, kBinary, sizeof(kBinary) - 1);
	const uint8_t nul = 0;
	Esp32SerialWrite(gPort, &nul, 1);
	Esp32WireParserInit(&gParser);
	_pump(1000, false);

	_command("LDN_HELLO");
	if (!_pump(1500, false)) {
		goto done;
	}
	if (!gResult.responses) {
		LOG("no reply to LDN_HELLO; retrying");
		_command("LDN_HELLO");
		_pump(1500, false);
	}
	if (!gResult.responses) {
		LOG("FAIL: the board never answered the handshake (the USB link itself is not working).");
		goto done;
	}
	LOG("PASS: the board answered the handshake (USB serial link works).");

	uint32_t wanted = 0x1A2B3C4D;
	char begin[64];
	snprintf(begin, sizeof(begin), "LDN_BEGIN %08X", wanted);
	gSession = 0; // BEGIN itself is sent before the device has a session
	_command(begin);
	gSession = wanted;
	_pump(1500, false);
	_command("LDN_BRIDGE_START");
	_pump(1500, false);
	_command("LDN_ADAPTER host");
	_pump(1500, false);

	LOG("listening for up to %u s (the Switch should be hosting in Wireless Club > Direct Corner)...", seconds);
	unsigned long long start = _now();
	unsigned tick = 0;
	while (!gStop && _now() - start < seconds * 1000ull) {
		_command("LDN_PING");
		_picoReport();
		if (!_pump(500, false)) {
			break;
		}
		_picoReport();
		if (!_pump(500, false)) {
			break;
		}
		if (++tick % 10 == 0) {
			LOG("... %us: replies=%u events=%u gb frames=%u beacons=%u joined=%d", tick, gResult.responses, gResult.frames[ESP32_TYPE_EVENT],
			    gResult.frames[ESP32_TYPE_GB_FRAME], gResult.beacons, gResult.joined);
		}
		if (gResult.beacons >= 3) {
			break;
		}
	}

	LOG("--- summary: replies=%u events=%u gb frames=%u beacons=%u joined_room=%d bad_frames=%u", gResult.responses,
	    gResult.frames[ESP32_TYPE_EVENT], gResult.frames[ESP32_TYPE_GB_FRAME], gResult.beacons, gResult.joined, gParser.framesBad);
	if (gResult.haveBeacon) {
		LOG("PASS: full chain works: joined the Switch room and received its beacon (\"%s\").", gResult.beaconName);
		rc = 0;
	} else if (gResult.joined) {
		LOG("PARTIAL: joined the room but no beacon arrived.");
	} else {
		LOG("PARTIAL: the link works but no Switch room was found. Is the Switch hosting in Direct Corner, and are prod.keys on the board?");
	}
	_command("LDN_ADAPTER uart");
	_pump(500, false);

done:
	Esp32SerialClose(gPort);
	gPort = NULL;
	return rc;
}
