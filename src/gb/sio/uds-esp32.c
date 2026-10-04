/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-esp32.h>

#include "../../gba/sio/esp32/esp32-serial.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define UDS_ESP32_BAUD 921600

// Framing -----------------------------------------------------------------------------------------------------------------------

uint32_t udsEsp32Crc32(const uint8_t* data, size_t length) {
	uint32_t crc = 0xFFFFFFFFu;
	size_t i;
	for (i = 0; i < length; ++i) {
		crc ^= data[i];
		int bit;
		for (bit = 0; bit < 8; ++bit) {
			crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
		}
	}
	return ~crc;
}

// Consistent Overhead Byte Stuffing. Returns the encoded size, or 0 if it does not fit.
static size_t _cobsEncode(const uint8_t* in, size_t length, uint8_t* out, size_t capacity) {
	if (capacity < 1) {
		return 0;
	}
	size_t write = 1, codeIndex = 0;
	uint8_t code = 1;
	size_t i;
	for (i = 0; i < length; ++i) {
		if (in[i] == 0) {
			out[codeIndex] = code;
			if (write >= capacity) {
				return 0;
			}
			codeIndex = write++;
			code = 1;
		} else {
			if (write >= capacity) {
				return 0;
			}
			out[write++] = in[i];
			if (++code == 0xFF) {
				out[codeIndex] = code;
				if (write >= capacity) {
					return 0;
				}
				codeIndex = write++;
				code = 1;
			}
		}
	}
	out[codeIndex] = code;
	return write;
}

// Returns the decoded size, or -1 for invalid COBS or no room.
static int _cobsDecode(const uint8_t* in, size_t length, uint8_t* out, size_t capacity) {
	size_t index = 0, write = 0;
	while (index < length) {
		uint8_t code = in[index++];
		if (code == 0) {
			return -1;
		}
		size_t run = (size_t) code - 1;
		if (index + run > length || write + run > capacity) {
			return -1;
		}
		memcpy(&out[write], &in[index], run);
		write += run;
		index += run;
		if (code != 0xFF && index < length) {
			if (write >= capacity) {
				return -1;
			}
			out[write++] = 0;
		}
	}
	return (int) write;
}

size_t udsEsp32Encode(uint8_t type, uint8_t seq, uint8_t flags, const uint8_t* payload, size_t length, uint8_t* out, size_t capacity) {
	uint8_t raw[UDS_ESP32_MAX_RAW];
	if (length > UDS_ESP32_MAX_PAYLOAD) {
		return 0;
	}
	raw[0] = UDS_ESP32_VERSION;
	raw[1] = type;
	raw[2] = seq;
	raw[3] = flags;
	raw[4] = (uint8_t) length;
	raw[5] = (uint8_t) (length >> 8);
	if (length) {
		memcpy(&raw[UDS_ESP32_HEADER], payload, length);
	}
	size_t rawLength = UDS_ESP32_HEADER + length;
	uint32_t crc = udsEsp32Crc32(raw, rawLength);
	int shift;
	for (shift = 0; shift < 32; shift += 8) {
		raw[rawLength++] = (uint8_t) (crc >> shift);
	}
	if (capacity < 1) {
		return 0;
	}
	size_t encoded = _cobsEncode(raw, rawLength, out, capacity - 1);
	if (!encoded) {
		return 0;
	}
	out[encoded++] = 0;
	return encoded;
}

void udsEsp32DecoderInit(struct UDSEsp32Decoder* decoder) {
	memset(decoder, 0, sizeof(*decoder));
}

bool udsEsp32DecoderFeed(struct UDSEsp32Decoder* decoder, uint8_t byte, struct UDSEsp32Frame* frame) {
	if (byte != 0) {
		if (decoder->length >= sizeof(decoder->encoded)) {
			decoder->overflowed = true;
		} else if (!decoder->overflowed) {
			decoder->encoded[decoder->length++] = byte;
		}
		return false;
	}
	bool hadOverflow = decoder->overflowed;
	size_t length = decoder->length;
	decoder->overflowed = false;
	decoder->length = 0;
	if (!length && !hadOverflow) {
		return false; // an idle delimiter
	}
	uint8_t raw[UDS_ESP32_MAX_RAW];
	int rawLength = hadOverflow ? -1 : _cobsDecode(decoder->encoded, length, raw, sizeof(raw));
	if (rawLength < (int) (UDS_ESP32_HEADER + 4) || raw[0] != UDS_ESP32_VERSION) {
		++decoder->framesBad;
		return false;
	}
	size_t payloadLength = raw[4] | ((size_t) raw[5] << 8);
	if (payloadLength > UDS_ESP32_MAX_PAYLOAD || (size_t) rawLength != UDS_ESP32_HEADER + payloadLength + 4) {
		++decoder->framesBad;
		return false;
	}
	const uint8_t* crcBytes = &raw[rawLength - 4];
	uint32_t expected = crcBytes[0] | ((uint32_t) crcBytes[1] << 8) | ((uint32_t) crcBytes[2] << 16) | ((uint32_t) crcBytes[3] << 24);
	if (udsEsp32Crc32(raw, (size_t) rawLength - 4) != expected) {
		++decoder->framesBad;
		return false;
	}
	frame->type = raw[1];
	frame->seq = raw[2];
	frame->flags = raw[3];
	frame->length = payloadLength;
	memcpy(frame->payload, &raw[UDS_ESP32_HEADER], payloadLength);
	++decoder->framesOk;
	return true;
}

// The board ----------------------------------------------------------------------------------------------------------------------

static uint32_t _nowMs(void) {
#ifdef _WIN32
	return (uint32_t) GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

static void _sleepMs(unsigned ms) {
#ifdef _WIN32
	Sleep(ms);
#else
	struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
#endif
}

bool udsEsp32Open(struct UDSEsp32* esp, const char* portName, const struct UDSEsp32Handlers* handlers) {
	memset(esp, 0, sizeof(*esp));
	if (handlers) {
		esp->handlers = *handlers;
	}
	udsEsp32DecoderInit(&esp->decoder);
	char found[64];
	if (!portName || !*portName) {
		if (!Esp32SerialFindEspressif(found, sizeof(found))) {
			return false;
		}
		portName = found;
	}
	esp->port = Esp32SerialOpen(portName, UDS_ESP32_BAUD);
	return esp->port != NULL;
}

void udsEsp32Close(struct UDSEsp32* esp) {
	if (esp->port) {
		Esp32SerialClose(esp->port);
		esp->port = NULL;
	}
}

static uint32_t _le32(const uint8_t* p) {
	return p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void _dispatch(struct UDSEsp32* esp, const struct UDSEsp32Frame* frame) {
	const uint8_t* p = frame->payload;
	switch (frame->type) {
	case UDS_ESP32_EVT_HELLO_ACK:
		if (frame->length >= 9) {
			esp->info.valid = true;
			esp->info.proto = p[0];
			esp->info.major = p[1];
			esp->info.minor = p[2];
			memcpy(esp->info.factoryMac, &p[3], 6);
		}
		break;
	case UDS_ESP32_EVT_STATUS:
		if (frame->length >= 5) {
			++esp->statusFrames;
			if (esp->handlers.status) {
				esp->handlers.status(esp->handlers.context, p[0], (int32_t) _le32(&p[1]));
			}
		}
		break;
	case UDS_ESP32_EVT_RX:
		if (frame->length >= 3) {
			++esp->rxFrames;
			if (esp->handlers.rx) {
				struct UDSEsp32Rx rx = {p[0], (int8_t) p[1], p[2], &p[3], frame->length - 3};
				esp->handlers.rx(esp->handlers.context, &rx);
			}
		}
		break;
	case UDS_ESP32_EVT_LOG:
		++esp->logFrames;
		if (esp->handlers.log) {
			esp->handlers.log(esp->handlers.context, (const char*) p, frame->length);
		}
		break;
	case UDS_ESP32_EVT_PONG:
		if (frame->length >= 4) {
			esp->lastPong = _le32(p);
		}
		break;
	case UDS_ESP32_EVT_TX_DONE:
		if (frame->length >= 3) {
			++esp->txDoneFrames;
			if (p[0]) {
				++esp->txAcked;
			}
			if (esp->handlers.txDone) {
				esp->handlers.txDone(esp->handlers.context, p[0] != 0, p[1] | ((size_t) p[2] << 8));
			}
		}
		break;
	default:
		break;
	}
}

bool udsEsp32Poll(struct UDSEsp32* esp) {
	if (!esp->port || esp->portFailed) {
		return false;
	}
	uint8_t buffer[1024];
	for (;;) {
		int count = Esp32SerialRead(esp->port, buffer, sizeof(buffer));
		if (count < 0) {
			esp->portFailed = true;
			return false;
		}
		if (count == 0) {
			return true;
		}
		static struct UDSEsp32Frame frame; // large; poll is not re-entered
		int i;
		for (i = 0; i < count; ++i) {
			if (udsEsp32DecoderFeed(&esp->decoder, buffer[i], &frame)) {
				_dispatch(esp, &frame);
			}
		}
	}
}

static bool _send(struct UDSEsp32* esp, uint8_t type, uint8_t flags, const uint8_t* payload, size_t length) {
	if (!esp->port || esp->portFailed) {
		return false;
	}
	uint8_t out[UDS_ESP32_MAX_ENCODED];
	size_t size = udsEsp32Encode(type, esp->seq++, flags, payload, length, out, sizeof(out));
	return size && Esp32SerialWrite(esp->port, out, size);
}

bool udsEsp32WaitReady(struct UDSEsp32* esp, unsigned timeoutMs) {
	uint32_t start = _nowMs(), lastHello = 0;
	bool first = true;
	while (!esp->info.valid && _nowMs() - start < timeoutMs) {
		if (first || _nowMs() - lastHello >= 500) {
			_send(esp, UDS_ESP32_CMD_HELLO, 0, NULL, 0);
			lastHello = _nowMs();
			first = false;
		}
		if (!udsEsp32Poll(esp)) {
			return false;
		}
		_sleepMs(5);
	}
	return esp->info.valid;
}

bool udsEsp32Start(struct UDSEsp32* esp, uint8_t channel, const uint8_t mac[6], bool decoy) {
	uint8_t payload[8];
	payload[0] = channel;
	memcpy(&payload[1], mac, 6);
	payload[7] = decoy ? 1 : 0;
	return _send(esp, UDS_ESP32_CMD_START, 0, payload, decoy ? 8 : 7);
}

bool udsEsp32SendHello(struct UDSEsp32* esp) {
	return _send(esp, UDS_ESP32_CMD_HELLO, 0, NULL, 0);
}

bool udsEsp32Stop(struct UDSEsp32* esp) {
	return _send(esp, UDS_ESP32_CMD_STOP, 0, NULL, 0);
}

bool udsEsp32SetChannel(struct UDSEsp32* esp, uint8_t channel) {
	return _send(esp, UDS_ESP32_CMD_SET_CHANNEL, 0, &channel, 1);
}

bool udsEsp32SetWatch(struct UDSEsp32* esp, const uint8_t mac[6]) {
	return _send(esp, UDS_ESP32_CMD_SET_WATCH, 0, mac, 6);
}

bool udsEsp32TxFrame(struct UDSEsp32* esp, uint8_t flags, uint8_t rate500kbps, const uint8_t* mpdu, size_t length) {
	if (length + 2 > UDS_ESP32_MAX_PAYLOAD) {
		return false;
	}
	uint8_t payload[UDS_ESP32_MAX_PAYLOAD];
	payload[0] = flags;
	payload[1] = rate500kbps;
	memcpy(&payload[2], mpdu, length);
	return _send(esp, UDS_ESP32_CMD_TX_FRAME, 0, payload, length + 2);
}

bool udsEsp32SetBeacon(struct UDSEsp32* esp, const uint8_t* mpdu, size_t length) {
	return _send(esp, UDS_ESP32_CMD_SET_BEACON, 0, mpdu, length);
}

bool udsEsp32Ping(struct UDSEsp32* esp, uint32_t token) {
	uint8_t payload[4] = {(uint8_t) token, (uint8_t) (token >> 8), (uint8_t) (token >> 16), (uint8_t) (token >> 24)};
	return _send(esp, UDS_ESP32_CMD_PING, 0, payload, sizeof(payload));
}

const char* udsEsp32StatusText(int32_t result) {
	static char text[32];
	if (result == 0) {
		return "ok";
	}
	snprintf(text, sizeof(text), "error %d", (int) result);
	return text;
}
