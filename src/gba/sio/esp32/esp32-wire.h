/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_ESP32_WIRE_H
#define GBA_SIO_ESP32_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The serial framing of GB-Link's ESP32 LDN bridge firmware (a separate project; only its documented serial
 * protocol is used here, no code). Each frame is
 *
 *   | version=1 | type | request_id:u32 LE | session_id:u32 LE | length:u16 LE | payload | crc32:u32 LE |
 *
 * with the CRC-32/ISO-HDLC (the ordinary zlib crc32) taken over everything before it, then COBS-encoded and
 * terminated by one 0x00 byte. Maximum raw frame 4096 bytes, maximum payload 4080.
 */

enum {
	ESP32_WIRE_VERSION = 1,
	ESP32_WIRE_HEADER = 12,
	ESP32_WIRE_MAX_FRAME = 4096,
	ESP32_WIRE_MAX_PAYLOAD = 4080,

	ESP32_TYPE_COMMAND = 1, // host -> device: ASCII control command
	ESP32_TYPE_RESPONSE = 2, // device -> host: reply to a request
	ESP32_TYPE_EVENT = 3, // device -> host: asynchronous event (request_id 0)
	ESP32_TYPE_UDP_OUT = 4, // host -> device: IPv4 + UDP payload
	ESP32_TYPE_UDP_IN = 5, // device -> host: IPv4 source + UDP payload
	ESP32_TYPE_GB_FRAME = 6, // device -> host (protocol v2): one whole GB-Link 'GB' frame
	ESP32_TYPE_GB_STREAM = 7, // host -> device (protocol v2): GB-Link byte stream, need not be frame aligned
};

struct Esp32WireFrame {
	uint8_t type;
	uint32_t requestId;
	uint32_t sessionId;
	size_t payloadLength;
	uint8_t payload[ESP32_WIRE_MAX_PAYLOAD];
};

// Encodes one frame (raw header + payload + CRC, COBS, trailing 0x00) into `out`. Returns the number of bytes
// written, or 0 if `out` (capacity `capacity`) or the payload limit is exceeded.
size_t Esp32WireEncode(uint8_t type, uint32_t requestId, uint32_t sessionId, const void* payload, size_t payloadLength, uint8_t* out, size_t capacity);

struct Esp32WireParser {
	uint8_t buffer[ESP32_WIRE_MAX_FRAME + 64];
	size_t length;
	bool overflowed;
	unsigned framesOk;
	unsigned framesBad; // a corrupt frame is dropped and counted, and synchronisation continues at the next delimiter
};

void Esp32WireParserInit(struct Esp32WireParser*);
// Feeds one received byte. Returns true when it completed a valid frame, which is then in *frame.
bool Esp32WireFeed(struct Esp32WireParser*, uint8_t byte, struct Esp32WireFrame* frame);

// The GB-Link 'GB' frame the device passes in type 6/7: | 0x47 0x42 | channel | length:u16 LE | payload |.
enum { GB_CHANNEL_COMMAND = 0x00, GB_CHANNEL_DATA = 0x01, GB_CHANNEL_STATUS = 0x02, GB_HEADER = 5 };
size_t Esp32GbFrameBuild(uint8_t channel, const void* payload, size_t length, uint8_t* out, size_t capacity);

#endif
