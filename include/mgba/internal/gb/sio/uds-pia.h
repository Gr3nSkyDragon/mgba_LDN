/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_PIA_H
#define GB_SIO_UDS_PIA_H

#include <mgba-util/common.h>

CXX_GUARD_START

/*
 * Wire format of the Pia datagrams the 3DS Virtual Console releases of the Game Boy Pokemon games exchange over UDS
 * (channel 243). Everything here is measured from captures; docs/wiki/vc_link.md in this repository has the evidence.
 *
 * A UDS payload (one "frame") is:
 *
 *   0   2   01 kind        kind: 01 Pia data, 11 host hello, 21 joiner hello reply, 12 host bye
 *   2   2   little-endian  frame length - 12
 *   4   6   zeros
 *  10   2   little-endian  CRC-16/ARC (poly 0xA001 reflected, init 0) of bytes 0..9
 *  --- Pia data frames only, from here ---
 *  12   4   32 AB 98 64    Pia magic
 *  16   1   01             "encrypted" field of Pia <= 5.6: 1 = not encrypted
 *  17   1                  Pia connection id (chosen per session by each side)
 *  18   2   big-endian     packet id
 *  20   2   big-endian     sender's session clock in ms
 *  22   2   big-endian     sender's estimate of the peer's clock
 *  24   n                  messages, each padded to a multiple of 4 bytes
 *  end-16 16               HMAC-MD5 over bytes 12 .. end-16, key "PokemonSIO"
 *
 * A message is a 20-byte header and a payload:
 *
 *   0   1   00
 *   1   1                  sender's station index (host 00, joiner 01, joiner FD until the host's mesh state)
 *   2   2   big-endian     payload length (before padding)
 *   4   4   big-endian     destination station id (0 broadcast, host 1, joiner 2)
 *   8   4   zeros
 *  12   1                  protocol
 *  13   1                  C0 on keep-alive, 10 on clock sync, otherwise 00
 *  14   1   00
 *  15   1                  01 on the reliable system stream, otherwise 00
 *  16   4   zeros
 */

#define UDS_FRAME_PREFIX_SIZE 12
#define UDS_PIA_HEADER_SIZE 12
#define UDS_FRAME_HEADER_SIZE (UDS_FRAME_PREFIX_SIZE + UDS_PIA_HEADER_SIZE)
#define UDS_TAIL_SIZE 16
#define UDS_MESSAGE_HEADER_SIZE 20
#define UDS_MAX_FRAME_SIZE 1440 // the largest frame seen: 25 game units
#define UDS_UNIT_SIZE 56 // a game-stream message: 20-byte header + 36-byte payload

#define UDS_HELLO_SIZE 52
#define UDS_HELLO_REPLY_SIZE 20
#define UDS_BYE_SIZE 16

// The HMAC key, the same in every Virtual Console title checked (Red/Blue/Yellow share a code.bin, Gold/Silver another,
// Crystal its own; only Red's use has been verified on the wire).
extern const uint8_t UDS_PIA_KEY[10];

enum UDSFrameKind {
	UDS_FRAME_PIA = 0x01,
	UDS_FRAME_HELLO = 0x11,
	UDS_FRAME_BYE = 0x12,
	UDS_FRAME_HELLO_REPLY = 0x21,
};

enum UDSProtocol {
	UDS_PROTOCOL_KEEPALIVE = 0x00,
	UDS_PROTOCOL_SETUP = 0x01,
	UDS_PROTOCOL_SYSTEM = 0x02,
	UDS_PROTOCOL_PING = 0x06,
	UDS_PROTOCOL_GAME = 0x30,
};

enum {
	UDS_SUBTYPE_KEEPALIVE = 0xC0,
	UDS_SUBTYPE_CLOCK_SYNC = 0x10,
	UDS_STATION_HOST = 0x00,
	UDS_STATION_JOINER = 0x01,
	UDS_STATION_UNASSIGNED = 0xFD,
	UDS_ID_HOST = 1, // destination station ids
	UDS_ID_JOINER = 2,
	UDS_ID_BROADCAST = 0,
};

struct UDSPiaHeader {
	uint8_t connectionId;
	uint16_t packetId;
	uint16_t clock;
	uint16_t peerClock;
};

struct UDSMessage {
	uint8_t sender;
	uint32_t destination;
	uint8_t protocol;
	uint8_t subtype;
	uint8_t reliable;
	uint16_t length; // payload length, without padding
	const uint8_t* payload; // points into the frame when parsed; the caller's buffer when building
};

struct UDSFrame {
	enum UDSFrameKind kind;
	size_t size;
	bool crcOk; // bytes 10..11 match the CRC of bytes 0..9
	// Pia data frames:
	struct UDSPiaHeader pia;
	bool tailOk;
	const uint8_t* messages; // between the 24-byte header and the 16-byte tail
	size_t messagesSize;
	// Hello and bye frames:
	uint32_t helloValue; // the host's 4-byte per-session value of a hello (header offset 16)
};

uint16_t udsCrc16(const uint8_t* data, size_t size);
void udsHmacMd5(const uint8_t* key, size_t keySize, const uint8_t* data, size_t size, uint8_t out[16]);

// Builds a Pia data frame in `buffer` (capacity `capacity`): header, messages as appended, tail.
// Usage: pos = udsFrameBegin(buffer, &pia); pos = udsFrameAppend(buffer, capacity, pos, &message) ...; size = udsFrameEnd(buffer, pos).
// Append returns 0 when the message does not fit.
size_t udsFrameBegin(uint8_t* buffer, const struct UDSPiaHeader* pia);
size_t udsFrameAppend(uint8_t* buffer, size_t capacity, size_t pos, const struct UDSMessage* message);
size_t udsFrameEnd(uint8_t* buffer, size_t capacity, size_t pos);

size_t udsBuildHello(uint8_t* buffer, uint32_t value);
size_t udsBuildHelloReply(uint8_t* buffer);
size_t udsBuildBye(uint8_t* buffer);

// Parses (and checks) a frame; false if it is too short or its length field disagrees with `size`.
// Pia data frames get their tail verified into `tailOk`.
bool udsFrameParse(const uint8_t* buffer, size_t size, struct UDSFrame* out);

// Walks the messages of a parsed Pia data frame. `*pos` starts at 0. False at the end or on a malformed message.
bool udsMessageNext(const struct UDSFrame* frame, size_t* pos, struct UDSMessage* out);

CXX_GUARD_END

#endif
