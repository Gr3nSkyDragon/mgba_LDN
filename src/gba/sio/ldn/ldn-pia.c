/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ldn-pia.h"

#include "ldn.h"

#include <mgba-util/crc32.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

const uint8_t kLdnPiaMagic[4] = {0x32, 0xAB, 0x98, 0x64};

// FireRed/LeafGreen's Virtual Console app's fixed 16-byte Pia game key (the same for every Gen 3 title the
// container hosts - it belongs to the emulator/container, not the ROM, like the LDN GBA_APP_PASSPHRASE already
// in ldn.c). Source: pokeldn/ldn/crypto.py's FRLG_GAME_KEY.
static const uint8_t kFrlgGameKey[16] = {
    0x83, 0xca, 0x7f, 0xab, 0x73, 0x4c, 0x34, 0x63, 0x3b, 0x10, 0x18, 0x35, 0x26, 0xc1, 0xe8, 0x5b,
};

void LdnPiaHeaderPack(const struct LdnPiaHeader* header, uint8_t out[21]) {
	memcpy(out, kLdnPiaMagic, 4);
	out[4] = header->enc;
	out[5] = header->flags;
	out[6] = (uint8_t) (header->dst >> 8);
	out[7] = (uint8_t) header->dst;
	out[8] = (uint8_t) (header->src >> 8);
	out[9] = (uint8_t) header->src;
	out[10] = (uint8_t) (header->pktid >> 8);
	out[11] = (uint8_t) header->pktid;
	out[12] = header->footer;
	memcpy(&out[13], header->nonce8, LDN_PIA_NONCE_SIZE);
}

void LdnPiaHeaderUnpack(const uint8_t* datagram, struct LdnPiaHeader* out) {
	out->enc = datagram[4];
	out->flags = datagram[5];
	out->dst = (uint16_t) ((datagram[6] << 8) | datagram[7]);
	out->src = (uint16_t) ((datagram[8] << 8) | datagram[9]);
	out->pktid = (uint16_t) ((datagram[10] << 8) | datagram[11]);
	out->footer = datagram[12];
	memcpy(out->nonce8, &datagram[13], LDN_PIA_NONCE_SIZE);
}

bool LdnPiaIsPia(const uint8_t* datagram, size_t length) {
	return length >= LDN_PIA_CIPHERTEXT_OFFSET && !memcmp(datagram, kLdnPiaMagic, 4);
}

void LdnPiaCryptoInit(struct LdnPiaCrypto* crypto, const uint8_t ssid[16]) {
	LdnAesEcbEncryptBlock(kFrlgGameKey, ssid, crypto->sessionKey);
	crypto->netId = crc32(0, &ssid[1], 15);
}

static void _nonce12(const struct LdnPiaCrypto* crypto, const uint8_t ip[4], const uint8_t headerNonce8[8], uint8_t out[12]) {
	uint32_t ipBe = ((uint32_t) ip[0] << 24) | ((uint32_t) ip[1] << 16) | ((uint32_t) ip[2] << 8) | ip[3];
	uint32_t four = crypto->netId ^ ipBe;
	out[0] = (uint8_t) (four >> 24);
	out[1] = (uint8_t) (four >> 16);
	out[2] = (uint8_t) (four >> 8);
	out[3] = (uint8_t) four;
	memcpy(&out[4], headerNonce8, 8);
}

bool LdnPiaDecrypt(const struct LdnPiaCrypto* crypto, const uint8_t* datagram, size_t length, const uint8_t srcIp[4], uint8_t* outPlain,
                   size_t* outLength) {
	if (!LdnPiaIsPia(datagram, length)) {
		return false;
	}
	struct LdnPiaHeader header;
	LdnPiaHeaderUnpack(datagram, &header);
	uint8_t nonce12[12];
	_nonce12(crypto, srcIp, header.nonce8, nonce12);
	const uint8_t* tag = &datagram[LDN_PIA_HEADER_SIZE + LDN_PIA_NONCE_SIZE];
	const uint8_t* cipher = &datagram[LDN_PIA_CIPHERTEXT_OFFSET];
	size_t cipherLength = length - LDN_PIA_CIPHERTEXT_OFFSET;
	if (!LdnAesGcmDecryptTag(crypto->sessionKey, nonce12, NULL, 0, tag, LDN_PIA_TAG_SIZE, cipher, cipherLength, outPlain)) {
		return false;
	}
	*outLength = cipherLength;
	return true;
}

// Declared locally rather than pulling in the vendored library's own enormous header content (which the build
// never needs beyond these functions) - matches `src/third-party/zstd/zstdlib.c`'s own public declarations
// exactly (checked against the vendored file directly, not guessed).
extern size_t ZSTD_decompress(void* dst, size_t dstCapacity, const void* src, size_t srcSize);
extern unsigned long long ZSTD_getFrameContentSize(const void* src, size_t srcSize);
extern size_t ZSTD_findFrameCompressedSize(const void* src, size_t srcSize);
extern unsigned ZSTD_isError(size_t result);
extern size_t ZSTD_compressBound(size_t srcSize);

typedef struct ZSTD_CCtx_s ZSTD_CCtx;
extern ZSTD_CCtx* ZSTD_createCCtx(void);
extern size_t ZSTD_freeCCtx(ZSTD_CCtx* cctx);
extern size_t ZSTD_CCtx_setParameter(ZSTD_CCtx* cctx, int param, int value);
extern size_t ZSTD_compress2(ZSTD_CCtx* cctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);

// ZSTD_cParameter enum values, hardcoded to avoid pulling zstdlib.c's full header - checked against the vendored
// file's own `enum ZSTD_cParameter` directly, not guessed.
enum {
	kZstdCCompressionLevel = 100,
	kZstdCContentSizeFlag = 200,
	kZstdCChecksumFlag = 201,
};

static const uint8_t kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};

bool LdnPiaDecompress(const uint8_t* data, size_t length, uint8_t* out, size_t* outLength) {
	if (length < 4 || memcmp(data, kZstdMagic, 4)) {
		size_t copy = length < *outLength ? length : *outLength;
		if (copy) {
			memcpy(out, data, copy);
		}
		*outLength = length;
		return true;
	}
	// `data` is a Pia MESSAGE payload, not a lone zstd file: the real datagram appends a plaintext footer and/or
	// 0xFF padding (out to a 16-byte boundary) after the zstd frame itself (see LdnPiaHeader.footer and this
	// project's own send-side padding in ldn-pia-join.c's _sendRaw). ZSTD_decompress rejects trailing bytes that
	// don't parse as a further valid frame, so the exact compressed-frame length must be found first and only
	// that many bytes handed to it - passing the whole padded blob (live-confirmed) fails every time.
	size_t frameSize = ZSTD_findFrameCompressedSize(data, length);
	if (ZSTD_isError(frameSize) || frameSize > length) {
		return false;
	}
	size_t result = ZSTD_decompress(out, *outLength, data, frameSize);
	if (ZSTD_isError(result)) {
		return false;
	}
	*outLength = result;
	return true;
}

bool LdnPiaCompress(const uint8_t* data, size_t length, uint8_t* out, size_t* outLength) {
	// Must match the real console's own encoder settings, not just produce SOME valid zstd frame: the plain
	// one-shot ZSTD_compress() API defaults to writing the content size into the frame header (Single_Segment
	// framing), but every message this project has observed from the real Switch omits it (Frame_Content_Size
	// flag=00, Single_Segment_flag=0 - see LdnPiaDecompress's frame-parsing notes). A frame the host's own
	// decoder doesn't recognize as matching its expected shape is a real, live-suspected reason a compressed
	// Session join could be silently ignored - level 4 (not the default 3) and no content size/checksum are the
	// exact settings pokeldn/frlgsim's own `crypto.py` documents as verified byte-identical against real
	// Switch-produced frames (`ZSTD_LEVEL = 4`, `write_content_size=False`, no dictionary/checksum).
	ZSTD_CCtx* cctx = ZSTD_createCCtx();
	if (!cctx) {
		return false;
	}
	ZSTD_CCtx_setParameter(cctx, kZstdCCompressionLevel, 4);
	ZSTD_CCtx_setParameter(cctx, kZstdCContentSizeFlag, 0);
	ZSTD_CCtx_setParameter(cctx, kZstdCChecksumFlag, 0);
	size_t result = ZSTD_compress2(cctx, out, *outLength, data, length);
	ZSTD_freeCCtx(cctx);
	if (ZSTD_isError(result)) {
		return false;
	}
	*outLength = result;
	return true;
}

bool LdnPiaEncrypt(const struct LdnPiaCrypto* crypto, const uint8_t* plaintext, size_t length, const uint8_t srcIp[4],
                   const struct LdnPiaHeader* header, uint8_t* outDatagram, size_t* outLength) {
	uint8_t nonce12[12];
	_nonce12(crypto, srcIp, header->nonce8, nonce12);
	uint8_t tag[LDN_PIA_TAG_SIZE];
	uint8_t* cipher = &outDatagram[LDN_PIA_CIPHERTEXT_OFFSET];
	if (!LdnAesGcmEncryptTag(crypto->sessionKey, nonce12, NULL, 0, plaintext, length, cipher, tag, LDN_PIA_TAG_SIZE)) {
		return false;
	}
	uint8_t headerBytes[21];
	LdnPiaHeaderPack(header, headerBytes);
	memcpy(outDatagram, headerBytes, 21);
	memcpy(&outDatagram[LDN_PIA_HEADER_SIZE + LDN_PIA_NONCE_SIZE], tag, LDN_PIA_TAG_SIZE);
	*outLength = LDN_PIA_CIPHERTEXT_OFFSET + length;
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The raw UDP :12345 socket - see the header comment for why this is hand-built Ethernet/IPv4/UDP over a raw
// AF_PACKET socket rather than a normal bound UDP socket: ldnd's own SENDTO operation has no destination-address
// parameter (confirmed by reading the daemon's own C source, `daemon.c`/`backend.h`: `sendto_(fd, buf, len,
// flags)` is a plain `send()`, and the backend vtable has no `connect` entry at all), so there is no way to reach
// an arbitrary UDP peer through it except by injecting a complete frame ourselves - the same technique
// ldn-monitor.c already uses for raw 802.11 frames, just at the IP layer instead.
// ---------------------------------------------------------------------------------------------------------------

enum {
	AF_PACKET_LINUX = 17,
	SOCK_RAW_LINUX = 3,
	ETH_P_IP_LINUX = 0x0800,
	IPPROTO_UDP_LINUX = 17,

	PIA_QUEUE_DEPTH = 16,
};

static char sSocketError[160] = "";

const char* LdnPiaSocketLastError(void) {
	return sSocketError;
}

static void _socketFail(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vsnprintf(sSocketError, sizeof(sSocketError), format, args);
	va_end(args);
}

#ifdef _WIN32

struct PiaDatagram {
	uint8_t ip[4];
	uint8_t payload[LDN_PIA_MAX_DATAGRAM];
	size_t length;
};

struct LdnPiaSocket {
	struct LdndConnection* conn; // not owned - see LdnPiaSocketOpen
	uint32_t ifIndex;
	uint8_t ourMac[6];
	uint32_t packetSocket;

	CRITICAL_SECTION queueLock;
	struct PiaDatagram queue[PIA_QUEUE_DEPTH];
	size_t queueHead, queueCount;
};

// Standard Internet checksum (RFC 1071): one's-complement sum of 16-bit words, folded to 16 bits, complemented.
static uint16_t _checksum(const uint8_t* data, size_t length, uint32_t seed) {
	uint32_t sum = seed;
	for (size_t i = 0; i + 1 < length; i += 2) {
		sum += (uint32_t) ((data[i] << 8) | data[i + 1]);
	}
	if (length & 1) {
		sum += (uint32_t) (data[length - 1] << 8);
	}
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}
	return (uint16_t) ~sum;
}

static bool _onFrame(struct LdnPiaSocket* socket, const uint8_t* data, size_t length, uint8_t outIp[4], const uint8_t** outPayload,
                     size_t* outPayloadLength) {
	// Ethernet(14) + IPv4(>=20, no options assumed beyond what IHL states) + UDP(8).
	if (length < 14 + 20 + 8) {
		return false;
	}
	if (data[12] != (uint8_t) (ETH_P_IP_LINUX >> 8) || data[13] != (uint8_t) ETH_P_IP_LINUX) {
		return false;
	}
	const uint8_t* ip = &data[14];
	if ((ip[0] >> 4) != 4) {
		return false; // not IPv4
	}
	size_t ihl = (size_t) (ip[0] & 0x0F) * 4;
	if (ihl < 20 || length < 14 + ihl + 8 || ip[9] != IPPROTO_UDP_LINUX) {
		return false;
	}
	const uint8_t* udp = ip + ihl;
	size_t udpAvailable = length - 14 - ihl;
	uint16_t dstPort = (uint16_t) ((udp[2] << 8) | udp[3]);
	if (dstPort != LDN_PIA_PORT) {
		return false;
	}
	uint16_t udpLength = (uint16_t) ((udp[4] << 8) | udp[5]);
	if (udpLength < 8 || udpLength > udpAvailable) {
		return false;
	}
	memcpy(outIp, &ip[12], 4);
	*outPayload = udp + 8;
	*outPayloadLength = udpLength - 8;
	return true;
}

static bool _route(void* context, uint32_t socketId, const uint8_t* data, size_t length) {
	struct LdnPiaSocket* socket = context;
	if (socketId != socket->packetSocket) {
		return false;
	}
	uint8_t ip[4];
	const uint8_t* payload;
	size_t payloadLength;
	if (_onFrame(socket, data, length, ip, &payload, &payloadLength)) {
		EnterCriticalSection(&socket->queueLock);
		if (socket->queueCount < PIA_QUEUE_DEPTH) {
			size_t slot = (socket->queueHead + socket->queueCount) % PIA_QUEUE_DEPTH;
			struct PiaDatagram* dg = &socket->queue[slot];
			memcpy(dg->ip, ip, 4);
			size_t copy = payloadLength < sizeof(dg->payload) ? payloadLength : sizeof(dg->payload);
			memcpy(dg->payload, payload, copy);
			dg->length = copy;
			++socket->queueCount;
		}
		// A full queue silently drops the newest datagram (Pia's own reliable layer, above this, is what
		// recovers from loss - this queue is not the place to grow unbounded).
		LeaveCriticalSection(&socket->queueLock);
	}
	return true; // claimed: this is the only owner of a raw AF_PACKET socket on the station interface
}

struct LdnPiaSocket* LdnPiaSocketOpen(struct LdndConnection* conn, uint32_t ifIndex, const uint8_t ourMac[6]) {
	struct LdnPiaSocket* socket = calloc(1, sizeof(*socket));
	if (!socket) {
		_socketFail("out of memory");
		return NULL;
	}
	socket->conn = conn;
	socket->ifIndex = ifIndex;
	memcpy(socket->ourMac, ourMac, 6);
	InitializeCriticalSection(&socket->queueLock);

	if (!LdndAddDataCallback(conn, _route, socket)) {
		_socketFail("too many listeners already registered on this ldnd connection");
		DeleteCriticalSection(&socket->queueLock);
		free(socket);
		return NULL;
	}

	uint32_t socketId;
	int error = LdndSocket(conn, AF_PACKET_LINUX, SOCK_RAW_LINUX, ((ETH_P_IP_LINUX & 0xFF) << 8) | (ETH_P_IP_LINUX >> 8), &socketId);
	if (error) {
		_socketFail("could not open a raw packet socket (error %d)", error);
		goto fail;
	}
	socket->packetSocket = socketId;

	uint8_t address[20] = {0}; // struct sockaddr_ll
	address[0] = AF_PACKET_LINUX;
	address[2] = ETH_P_IP_LINUX >> 8;
	address[3] = ETH_P_IP_LINUX & 0xFF;
	address[4] = (uint8_t) ifIndex;
	address[5] = (uint8_t) (ifIndex >> 8);
	address[6] = (uint8_t) (ifIndex >> 16);
	address[7] = (uint8_t) (ifIndex >> 24);
	error = LdndBind(conn, socketId, address, sizeof(address));
	if (error) {
		_socketFail("could not bind the raw packet socket to the station interface (error %d)", error);
		goto fail;
	}
	error = LdndStart(conn, socketId);
	if (error) {
		_socketFail("could not start the raw packet socket (error %d)", error);
		goto fail;
	}
	return socket;

fail:
	LdnPiaSocketClose(socket);
	return NULL;
}

void LdnPiaSocketClose(struct LdnPiaSocket* socket) {
	if (!socket) {
		return;
	}
	if (socket->packetSocket) {
		LdndCloseSocket(socket->conn, socket->packetSocket);
	}
	LdndRemoveDataCallback(socket->conn, _route, socket);
	DeleteCriticalSection(&socket->queueLock);
	free(socket);
}

// Pure frame construction (no I/O) - separated out so it can be exercised by a standalone test harness without a
// live ldnd connection. `frame` must be at least `14 + 20 + 8 + length` bytes; returns that total length.
static size_t _buildUdpFrame(const uint8_t destMac[6], const uint8_t srcMac[6], const uint8_t srcIp[4], const uint8_t destIp[4],
                             const uint8_t* payload, size_t length, uint8_t* frame) {
	size_t udpLength = 8 + length;
	size_t ipLength = 20 + udpLength;

	// Ethernet header.
	memcpy(&frame[0], destMac, 6);
	memcpy(&frame[6], srcMac, 6);
	frame[12] = (uint8_t) (ETH_P_IP_LINUX >> 8);
	frame[13] = (uint8_t) ETH_P_IP_LINUX;

	// IPv4 header.
	uint8_t* ip = &frame[14];
	ip[0] = 0x45; // version 4, IHL 5 (20 bytes, no options)
	ip[1] = 0; // DSCP/ECN
	ip[2] = (uint8_t) (ipLength >> 8);
	ip[3] = (uint8_t) ipLength;
	ip[4] = 0;
	ip[5] = 0; // identification (fragmentation is never expected on this link)
	ip[6] = 0x40; // flags: don't fragment
	ip[7] = 0;
	ip[8] = 64; // TTL
	ip[9] = IPPROTO_UDP_LINUX;
	ip[10] = 0;
	ip[11] = 0; // header checksum, filled below
	memcpy(&ip[12], srcIp, 4);
	memcpy(&ip[16], destIp, 4);
	uint16_t ipChecksum = _checksum(ip, 20, 0);
	ip[10] = (uint8_t) (ipChecksum >> 8);
	ip[11] = (uint8_t) ipChecksum;

	// UDP header.
	uint8_t* udp = ip + 20;
	udp[0] = (uint8_t) (LDN_PIA_PORT >> 8);
	udp[1] = (uint8_t) LDN_PIA_PORT;
	udp[2] = (uint8_t) (LDN_PIA_PORT >> 8);
	udp[3] = (uint8_t) LDN_PIA_PORT;
	udp[4] = (uint8_t) (udpLength >> 8);
	udp[5] = (uint8_t) udpLength;
	udp[6] = 0;
	udp[7] = 0; // checksum, filled below
	memcpy(udp + 8, payload, length);

	// UDP checksum: the IPv4 pseudo-header (src+dst+zero+protocol+UDP length) followed by the UDP header+payload.
	uint8_t pseudo[12];
	memcpy(&pseudo[0], srcIp, 4);
	memcpy(&pseudo[4], destIp, 4);
	pseudo[8] = 0;
	pseudo[9] = IPPROTO_UDP_LINUX;
	pseudo[10] = (uint8_t) (udpLength >> 8);
	pseudo[11] = (uint8_t) udpLength;
	uint32_t seed = 0;
	for (int i = 0; i < 12; i += 2) {
		seed += (uint32_t) ((pseudo[i] << 8) | pseudo[i + 1]);
	}
	uint16_t udpChecksum = _checksum(udp, udpLength, seed);
	if (udpChecksum == 0) {
		udpChecksum = 0xFFFF; // an all-zero result means "no checksum"; avoid that ambiguity
	}
	udp[6] = (uint8_t) (udpChecksum >> 8);
	udp[7] = (uint8_t) udpChecksum;

	return 14 + ipLength;
}

int LdnPiaSocketSend(struct LdnPiaSocket* socket, const uint8_t destMac[6], const uint8_t srcIp[4], const uint8_t destIp[4],
                     const uint8_t* payload, size_t length) {
	size_t frameLength = 14 + 20 + 8 + length;
	uint8_t* frame = malloc(frameLength);
	if (!frame) {
		_socketFail("out of memory");
		return LDND_ERR_ARGS;
	}
	_buildUdpFrame(destMac, socket->ourMac, srcIp, destIp, payload, length, frame);

	int error = LdndSendTo(socket->conn, socket->packetSocket, 0, frame, frameLength);
	free(frame);
	if (error) {
		_socketFail("could not send (error %d)", error);
	}
	return error;
}

int LdnPiaSocketSendArp(struct LdnPiaSocket* socket, const uint8_t* destMac, unsigned opcode, const uint8_t srcIp[4], const uint8_t targetIp[4],
                        const uint8_t* targetMac) {
	static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	uint8_t frame[42] = {0};
	memcpy(&frame[0], destMac ? destMac : kBroadcast, 6);
	memcpy(&frame[6], socket->ourMac, 6);
	frame[12] = 0x08;
	frame[13] = 0x06;
	frame[14] = 0x00; // hardware type: Ethernet
	frame[15] = 0x01;
	frame[16] = 0x08; // protocol type: IPv4
	frame[17] = 0x00;
	frame[18] = 6; // hardware address length
	frame[19] = 4; // protocol address length
	frame[20] = (uint8_t) (opcode >> 8);
	frame[21] = (uint8_t) opcode;
	memcpy(&frame[22], socket->ourMac, 6); // sender hardware address
	memcpy(&frame[28], srcIp, 4); // sender protocol address
	if (targetMac) {
		memcpy(&frame[32], targetMac, 6);
	}
	memcpy(&frame[38], targetIp, 4);
	int error = LdndSendTo(socket->conn, socket->packetSocket, 0, frame, sizeof(frame));
	if (error) {
		_socketFail("could not send ARP (error %d)", error);
	}
	return error;
}

bool LdnPiaSocketPoll(struct LdnPiaSocket* socket, uint8_t outIp[4], uint8_t* outPayload, size_t* inOutLength) {
	bool found = false;
	EnterCriticalSection(&socket->queueLock);
	if (socket->queueCount) {
		struct PiaDatagram* dg = &socket->queue[socket->queueHead];
		memcpy(outIp, dg->ip, 4);
		size_t copy = dg->length < *inOutLength ? dg->length : *inOutLength;
		memcpy(outPayload, dg->payload, copy);
		*inOutLength = dg->length;
		socket->queueHead = (socket->queueHead + 1) % PIA_QUEUE_DEPTH;
		--socket->queueCount;
		found = true;
	}
	LeaveCriticalSection(&socket->queueLock);
	return found;
}

#else // !_WIN32

struct LdnPiaSocket* LdnPiaSocketOpen(struct LdndConnection* conn, uint32_t ifIndex, const uint8_t ourMac[6]) {
	(void) conn;
	(void) ifIndex;
	(void) ourMac;
	_socketFail("not supported on this platform");
	return NULL;
}

void LdnPiaSocketClose(struct LdnPiaSocket* socket) {
	(void) socket;
}

int LdnPiaSocketSend(struct LdnPiaSocket* socket, const uint8_t destMac[6], const uint8_t srcIp[4], const uint8_t destIp[4],
                     const uint8_t* payload, size_t length) {
	(void) socket;
	(void) destMac;
	(void) srcIp;
	(void) destIp;
	(void) payload;
	(void) length;
	return LDND_ERR_ARGS;
}

int LdnPiaSocketSendArp(struct LdnPiaSocket* socket, const uint8_t* destMac, unsigned opcode, const uint8_t srcIp[4], const uint8_t targetIp[4],
                        const uint8_t* targetMac) {
	(void) socket; (void) destMac; (void) opcode; (void) srcIp; (void) targetIp; (void) targetMac;
	return LDND_ERR_ARGS;
}

bool LdnPiaSocketPoll(struct LdnPiaSocket* socket, uint8_t outIp[4], uint8_t* outPayload, size_t* inOutLength) {
	(void) socket;
	(void) outIp;
	(void) outPayload;
	(void) inOutLength;
	return false;
}

#endif
