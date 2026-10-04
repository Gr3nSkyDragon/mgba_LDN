/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-pia.h>

#include <mgba-util/md5.h>

const uint8_t UDS_PIA_KEY[10] = { 'P', 'o', 'k', 'e', 'm', 'o', 'n', 'S', 'I', 'O' };

static const uint8_t PIA_MAGIC[4] = { 0x32, 0xAB, 0x98, 0x64 };

uint16_t udsCrc16(const uint8_t* data, size_t size) {
	uint16_t crc = 0;
	size_t i;
	for (i = 0; i < size; ++i) {
		crc ^= data[i];
		int bit;
		for (bit = 0; bit < 8; ++bit) {
			crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
		}
	}
	return crc;
}

void udsHmacMd5(const uint8_t* key, size_t keySize, const uint8_t* data, size_t size, uint8_t out[16]) {
	uint8_t block[64] = { 0 };
	if (keySize > sizeof(block)) {
		md5Buffer(key, keySize, block);
	} else {
		memcpy(block, key, keySize);
	}
	uint8_t pad[64];
	struct MD5Context ctx;
	size_t i;
	for (i = 0; i < sizeof(pad); ++i) {
		pad[i] = block[i] ^ 0x36;
	}
	md5Init(&ctx);
	md5Update(&ctx, pad, sizeof(pad));
	md5Update(&ctx, data, size);
	md5Finalize(&ctx);
	uint8_t inner[16];
	memcpy(inner, ctx.digest, sizeof(inner));
	for (i = 0; i < sizeof(pad); ++i) {
		pad[i] = block[i] ^ 0x5C;
	}
	md5Init(&ctx);
	md5Update(&ctx, pad, sizeof(pad));
	md5Update(&ctx, inner, sizeof(inner));
	md5Finalize(&ctx);
	memcpy(out, ctx.digest, 16);
}

static void _put16le(uint8_t* p, uint16_t v) {
	p[0] = v;
	p[1] = v >> 8;
}

static void _put16be(uint8_t* p, uint16_t v) {
	p[0] = v >> 8;
	p[1] = v;
}

static void _put32be(uint8_t* p, uint32_t v) {
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static uint16_t _get16be(const uint8_t* p) {
	return (p[0] << 8) | p[1];
}

static uint32_t _get32be(const uint8_t* p) {
	return ((uint32_t) p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

// Bytes 0..11 of every frame: kind, length field, zeros, then the CRC of the first ten.
static void _prefix(uint8_t* buffer, uint8_t kind, uint16_t lengthField) {
	memset(buffer, 0, UDS_FRAME_PREFIX_SIZE);
	buffer[0] = 0x01;
	buffer[1] = kind;
	_put16le(&buffer[2], lengthField);
	_put16le(&buffer[10], udsCrc16(buffer, 10));
}

size_t udsFrameBegin(uint8_t* buffer, const struct UDSPiaHeader* pia) {
	_prefix(buffer, UDS_FRAME_PIA, 0); // the length and its CRC are filled in by udsFrameEnd
	memcpy(&buffer[12], PIA_MAGIC, sizeof(PIA_MAGIC));
	buffer[16] = 0x01;
	buffer[17] = pia->connectionId;
	_put16be(&buffer[18], pia->packetId);
	_put16be(&buffer[20], pia->clock);
	_put16be(&buffer[22], pia->peerClock);
	return UDS_FRAME_HEADER_SIZE;
}

size_t udsFrameAppend(uint8_t* buffer, size_t capacity, size_t pos, const struct UDSMessage* message) {
	size_t padded = (message->length + 3) & ~(size_t) 3;
	size_t total = UDS_MESSAGE_HEADER_SIZE + padded;
	if (pos + total + UDS_TAIL_SIZE > capacity) {
		return 0;
	}
	uint8_t* out = &buffer[pos];
	memset(out, 0, total);
	out[1] = message->sender;
	_put16be(&out[2], message->length);
	_put32be(&out[4], message->destination);
	out[12] = message->protocol;
	out[13] = message->subtype;
	out[15] = message->reliable;
	if (message->length) {
		memcpy(&out[UDS_MESSAGE_HEADER_SIZE], message->payload, message->length);
	}
	return pos + total;
}

size_t udsFrameEnd(uint8_t* buffer, size_t capacity, size_t pos) {
	if (pos + UDS_TAIL_SIZE > capacity) {
		return 0;
	}
	size_t total = pos + UDS_TAIL_SIZE;
	_prefix(buffer, UDS_FRAME_PIA, total - UDS_FRAME_PREFIX_SIZE);
	udsHmacMd5(UDS_PIA_KEY, sizeof(UDS_PIA_KEY), &buffer[UDS_FRAME_PREFIX_SIZE], pos - UDS_FRAME_PREFIX_SIZE,
	           &buffer[pos]);
	return total;
}

size_t udsBuildHello(uint8_t* buffer, uint32_t value) {
	memset(buffer, 0, UDS_HELLO_SIZE);
	_prefix(buffer, UDS_FRAME_HELLO, 0x18);
	buffer[12] = 0x02;
	_put32be(&buffer[16], value);
	buffer[20] = 0x01;
	buffer[28] = 0x01;
	buffer[30] = 0x02;
	return UDS_HELLO_SIZE;
}

size_t udsBuildHelloReply(uint8_t* buffer) {
	memset(buffer, 0, UDS_HELLO_REPLY_SIZE);
	_prefix(buffer, UDS_FRAME_HELLO_REPLY, 0);
	buffer[12] = 0x02;
	return UDS_HELLO_REPLY_SIZE;
}

size_t udsBuildBye(uint8_t* buffer) {
	memset(buffer, 0, UDS_BYE_SIZE);
	_prefix(buffer, UDS_FRAME_BYE, 0);
	return UDS_BYE_SIZE;
}

bool udsFrameParse(const uint8_t* buffer, size_t size, struct UDSFrame* out) {
	memset(out, 0, sizeof(*out));
	if (size < UDS_BYE_SIZE || buffer[0] != 0x01) {
		return false;
	}
	out->size = size;
	out->kind = buffer[1];
	out->crcOk = udsCrc16(buffer, 10) == (buffer[10] | (buffer[11] << 8));
	switch (out->kind) {
	case UDS_FRAME_HELLO:
		if (size >= 20) {
			out->helloValue = _get32be(&buffer[16]);
		}
		return true;
	case UDS_FRAME_HELLO_REPLY:
	case UDS_FRAME_BYE:
		return true;
	case UDS_FRAME_PIA:
		break;
	default:
		return false;
	}
	if (size < UDS_FRAME_HEADER_SIZE + UDS_TAIL_SIZE) {
		return false;
	}
	if ((size_t) (buffer[2] | (buffer[3] << 8)) + UDS_FRAME_PREFIX_SIZE != size) {
		return false;
	}
	if (memcmp(&buffer[12], PIA_MAGIC, sizeof(PIA_MAGIC))) {
		return false;
	}
	out->pia.connectionId = buffer[17];
	out->pia.packetId = _get16be(&buffer[18]);
	out->pia.clock = _get16be(&buffer[20]);
	out->pia.peerClock = _get16be(&buffer[22]);
	out->messages = &buffer[UDS_FRAME_HEADER_SIZE];
	out->messagesSize = size - UDS_FRAME_HEADER_SIZE - UDS_TAIL_SIZE;
	uint8_t tail[16];
	udsHmacMd5(UDS_PIA_KEY, sizeof(UDS_PIA_KEY), &buffer[UDS_FRAME_PREFIX_SIZE], size - UDS_FRAME_PREFIX_SIZE - UDS_TAIL_SIZE,
	           tail);
	out->tailOk = !memcmp(tail, &buffer[size - UDS_TAIL_SIZE], sizeof(tail));
	return true;
}

bool udsMessageNext(const struct UDSFrame* frame, size_t* pos, struct UDSMessage* out) {
	if (frame->kind != UDS_FRAME_PIA || *pos + UDS_MESSAGE_HEADER_SIZE > frame->messagesSize) {
		return false;
	}
	const uint8_t* m = &frame->messages[*pos];
	uint16_t length = _get16be(&m[2]);
	size_t padded = (length + 3) & ~(size_t) 3;
	if (*pos + UDS_MESSAGE_HEADER_SIZE + padded > frame->messagesSize) {
		return false;
	}
	out->sender = m[1];
	out->length = length;
	out->destination = _get32be(&m[4]);
	out->protocol = m[12];
	out->subtype = m[13];
	out->reliable = m[15];
	out->payload = &m[UDS_MESSAGE_HEADER_SIZE];
	*pos += UDS_MESSAGE_HEADER_SIZE + padded;
	return true;
}
