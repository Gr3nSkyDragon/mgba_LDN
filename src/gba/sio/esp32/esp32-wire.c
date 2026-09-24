/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "esp32-wire.h"

#include <mgba-util/crc32.h>

#include <string.h>

static size_t _cobsEncode(const uint8_t* in, size_t length, uint8_t* out, size_t capacity) {
	size_t read = 0, write = 1, codeIndex = 0;
	uint8_t code = 1;
	if (capacity < 1) {
		return 0;
	}
	while (read < length) {
		if (in[read] == 0) {
			out[codeIndex] = code;
			code = 1;
			codeIndex = write++;
			if (write > capacity) {
				return 0;
			}
			++read;
		} else {
			if (write >= capacity) {
				return 0;
			}
			out[write++] = in[read++];
			++code;
			if (code == 0xFF) {
				out[codeIndex] = code;
				code = 1;
				codeIndex = write++;
				if (write > capacity) {
					return 0;
				}
			}
		}
	}
	out[codeIndex] = code;
	return write;
}

static size_t _cobsDecode(const uint8_t* in, size_t length, uint8_t* out, size_t capacity) {
	size_t read = 0, write = 0;
	while (read < length) {
		uint8_t code = in[read++];
		if (!code) {
			return (size_t) -1;
		}
		for (uint8_t i = 1; i < code; ++i) {
			if (read >= length || write >= capacity) {
				return (size_t) -1;
			}
			out[write++] = in[read++];
		}
		if (code != 0xFF && read < length) {
			if (write >= capacity) {
				return (size_t) -1;
			}
			out[write++] = 0;
		}
	}
	return write;
}

size_t Esp32WireEncode(uint8_t type, uint32_t requestId, uint32_t sessionId, const void* payload, size_t payloadLength, uint8_t* out, size_t capacity) {
	if (payloadLength > ESP32_WIRE_MAX_PAYLOAD) {
		return 0;
	}
	uint8_t raw[ESP32_WIRE_MAX_FRAME];
	raw[0] = ESP32_WIRE_VERSION;
	raw[1] = type;
	for (int i = 0; i < 4; ++i) {
		raw[2 + i] = (uint8_t) (requestId >> (8 * i));
		raw[6 + i] = (uint8_t) (sessionId >> (8 * i));
	}
	raw[10] = (uint8_t) payloadLength;
	raw[11] = (uint8_t) (payloadLength >> 8);
	if (payloadLength) {
		memcpy(&raw[ESP32_WIRE_HEADER], payload, payloadLength);
	}
	size_t body = ESP32_WIRE_HEADER + payloadLength;
	uint32_t crc = crc32(0, raw, body);
	for (int i = 0; i < 4; ++i) {
		raw[body + i] = (uint8_t) (crc >> (8 * i));
	}
	size_t encoded = _cobsEncode(raw, body + 4, out, capacity);
	if (!encoded || encoded + 1 > capacity) {
		return 0;
	}
	out[encoded] = 0;
	return encoded + 1;
}

void Esp32WireParserInit(struct Esp32WireParser* parser) {
	memset(parser, 0, sizeof(*parser));
}

bool Esp32WireFeed(struct Esp32WireParser* parser, uint8_t byte, struct Esp32WireFrame* frame) {
	if (byte != 0) {
		if (parser->length < sizeof(parser->buffer)) {
			parser->buffer[parser->length++] = byte;
		} else {
			parser->overflowed = true;
		}
		return false;
	}
	size_t length = parser->length;
	bool overflowed = parser->overflowed;
	parser->length = 0;
	parser->overflowed = false;
	if (!length && !overflowed) {
		return false; // an empty frame: just a delimiter (the handshake sends one)
	}
	uint8_t raw[ESP32_WIRE_MAX_FRAME + 64];
	size_t decoded = overflowed ? (size_t) -1 : _cobsDecode(parser->buffer, length, raw, sizeof(raw));
	if (decoded == (size_t) -1 || decoded < ESP32_WIRE_HEADER + 4 || raw[0] != ESP32_WIRE_VERSION) {
		++parser->framesBad;
		return false;
	}
	size_t payloadLength = raw[10] | ((size_t) raw[11] << 8);
	if (payloadLength > ESP32_WIRE_MAX_PAYLOAD || ESP32_WIRE_HEADER + payloadLength + 4 != decoded) {
		++parser->framesBad;
		return false;
	}
	uint32_t expected = raw[decoded - 4] | (raw[decoded - 3] << 8) | (raw[decoded - 2] << 16) | ((uint32_t) raw[decoded - 1] << 24);
	if (crc32(0, raw, decoded - 4) != expected) {
		++parser->framesBad;
		return false;
	}
	frame->type = raw[1];
	frame->requestId = raw[2] | (raw[3] << 8) | (raw[4] << 16) | ((uint32_t) raw[5] << 24);
	frame->sessionId = raw[6] | (raw[7] << 8) | (raw[8] << 16) | ((uint32_t) raw[9] << 24);
	frame->payloadLength = payloadLength;
	memcpy(frame->payload, &raw[ESP32_WIRE_HEADER], payloadLength);
	++parser->framesOk;
	return true;
}

size_t Esp32GbFrameBuild(uint8_t channel, const void* payload, size_t length, uint8_t* out, size_t capacity) {
	if (length > 0xFFFF || capacity < GB_HEADER + length) {
		return 0;
	}
	out[0] = 0x47;
	out[1] = 0x42;
	out[2] = channel;
	out[3] = (uint8_t) length;
	out[4] = (uint8_t) (length >> 8);
	if (length) {
		memcpy(&out[GB_HEADER], payload, length);
	}
	return GB_HEADER + length;
}
