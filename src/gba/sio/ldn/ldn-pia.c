/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ldn-pia.h"

#include "ldn.h"

#include <mgba-util/crc32.h>

#include <string.h>

const uint8_t kLdnPiaMagic[4] = {0x32, 0xAB, 0x98, 0x64};

// FireRed/LeafGreen's Virtual Console app's fixed 16-byte Pia game key (the same for every Gen 3 title the
// container hosts - it belongs to the emulator/container, not the ROM, like the LDN passphrase (kLdnGbaPassphrase)
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
	// project's own send-side padding in rfu-broadcast.c's _piaSendRaw). ZSTD_decompress rejects trailing bytes that
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
