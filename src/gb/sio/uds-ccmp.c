/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-ccmp.h>

#include <mgba-util/md5.h>

#include <string.h>

// AES-128 --------------------------------------------------------------------------------------------------------------------

static uint8_t sSbox[256];
static bool sSboxReady;

// The S-box from its definition (multiplicative inverse in GF(2^8), then the affine map), so there is no table to mistype.
static void _buildSbox(void) {
	uint8_t p = 1, q = 1;
	do {
		p = (uint8_t) (p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0)); // p *= 3
		q ^= (uint8_t) (q << 1);
		q ^= (uint8_t) (q << 2);
		q ^= (uint8_t) (q << 4);
		if (q & 0x80) {
			q ^= 0x09;
		}
		uint8_t x = (uint8_t) (q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^
		                       ((q << 4) | (q >> 4)));
		sSbox[p] = x ^ 0x63;
	} while (p != 1);
	sSbox[0] = 0x63;
	sSboxReady = true;
}

static uint8_t _xtime(uint8_t x) {
	return (uint8_t) ((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}

void udsAesInit(struct UDSAes* aes, const uint8_t key[16]) {
	if (!sSboxReady) {
		_buildSbox();
	}
	memcpy(aes->roundKeys[0], key, 16);
	uint8_t rcon = 1;
	unsigned round;
	for (round = 1; round <= 10; ++round) {
		const uint8_t* prev = aes->roundKeys[round - 1];
		uint8_t* cur = aes->roundKeys[round];
		cur[0] = prev[0] ^ sSbox[prev[13]] ^ rcon;
		cur[1] = prev[1] ^ sSbox[prev[14]];
		cur[2] = prev[2] ^ sSbox[prev[15]];
		cur[3] = prev[3] ^ sSbox[prev[12]];
		unsigned i;
		for (i = 4; i < 16; ++i) {
			cur[i] = prev[i] ^ cur[i - 4];
		}
		rcon = _xtime(rcon);
	}
}

void udsAesEncrypt(const struct UDSAes* aes, const uint8_t in[16], uint8_t out[16]) {
	uint8_t s[16];
	unsigned i, round;
	for (i = 0; i < 16; ++i) {
		s[i] = in[i] ^ aes->roundKeys[0][i];
	}
	for (round = 1; round <= 10; ++round) {
		uint8_t t[16];
		// SubBytes and ShiftRows (the state is column-major: s[4 * column + row])
		for (i = 0; i < 16; ++i) {
			unsigned column = i / 4, row = i % 4;
			t[i] = sSbox[s[4 * ((column + row) % 4) + row]];
		}
		if (round < 10) {
			unsigned c;
			for (c = 0; c < 4; ++c) {
				uint8_t* col = &t[4 * c];
				uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
				uint8_t all = a0 ^ a1 ^ a2 ^ a3;
				s[4 * c + 0] = a0 ^ all ^ _xtime(a0 ^ a1);
				s[4 * c + 1] = a1 ^ all ^ _xtime(a1 ^ a2);
				s[4 * c + 2] = a2 ^ all ^ _xtime(a2 ^ a3);
				s[4 * c + 3] = a3 ^ all ^ _xtime(a3 ^ a0);
			}
		} else {
			memcpy(s, t, 16);
		}
		for (i = 0; i < 16; ++i) {
			s[i] ^= aes->roundKeys[round][i];
		}
	}
	memcpy(out, s, 16);
}

// CCM (M = 8, L = 2) -----------------------------------------------------------------------------------------------------------

static void _xor16(uint8_t* a, const uint8_t* b, size_t n) {
	size_t i;
	for (i = 0; i < n; ++i) {
		a[i] ^= b[i];
	}
}

// CBC-MAC over B0, the length-prefixed AAD and the message, zero padded to blocks. Returns the first 8 bytes.
static void _ccmMac(const struct UDSAes* aes, const uint8_t nonce[UDS_CCM_NONCE], const uint8_t* aad, size_t aadLength,
                    const uint8_t* message, size_t length, uint8_t tag[UDS_CCM_TAG]) {
	uint8_t x[16], block[16];
	block[0] = (uint8_t) ((aadLength ? 0x40 : 0) | (((UDS_CCM_TAG - 2) / 2) << 3) | 1);
	memcpy(&block[1], nonce, UDS_CCM_NONCE);
	block[14] = (uint8_t) (length >> 8);
	block[15] = (uint8_t) length;
	udsAesEncrypt(aes, block, x);
	if (aadLength) {
		// aadLength < 0xFF00: a two-byte length, then the AAD, in whole blocks
		uint8_t buf[2 + 64];
		size_t total = 2 + aadLength;
		size_t offset = 0;
		if (total > sizeof(buf)) {
			total = sizeof(buf); // 802.11 AAD is 22 bytes; this is not a general CCM for long headers
		}
		buf[0] = (uint8_t) (aadLength >> 8);
		buf[1] = (uint8_t) aadLength;
		memcpy(&buf[2], aad, total - 2);
		while (offset < total) {
			size_t n = total - offset < 16 ? total - offset : 16;
			memset(block, 0, 16);
			memcpy(block, &buf[offset], n);
			_xor16(x, block, 16);
			udsAesEncrypt(aes, x, x);
			offset += n;
		}
	}
	size_t offset = 0;
	while (offset < length) {
		size_t n = length - offset < 16 ? length - offset : 16;
		memset(block, 0, 16);
		memcpy(block, &message[offset], n);
		_xor16(x, block, 16);
		udsAesEncrypt(aes, x, x);
		offset += n;
	}
	memcpy(tag, x, UDS_CCM_TAG);
}

// out = in xor the CTR keystream starting at counter 1; the keystream of counter 0 goes to s0.
static void _ccmCtr(const struct UDSAes* aes, const uint8_t nonce[UDS_CCM_NONCE], const uint8_t* in, size_t length,
                    uint8_t* out, uint8_t s0[16]) {
	uint8_t a[16], stream[16];
	a[0] = 1; // L - 1
	memcpy(&a[1], nonce, UDS_CCM_NONCE);
	a[14] = 0;
	a[15] = 0;
	udsAesEncrypt(aes, a, s0);
	size_t offset = 0;
	unsigned counter = 1;
	while (offset < length) {
		a[14] = (uint8_t) (counter >> 8);
		a[15] = (uint8_t) counter;
		udsAesEncrypt(aes, a, stream);
		size_t n = length - offset < 16 ? length - offset : 16;
		size_t i;
		for (i = 0; i < n; ++i) {
			out[offset + i] = in[offset + i] ^ stream[i];
		}
		offset += n;
		++counter;
	}
}

void udsCcmEncrypt(const uint8_t key[16], const uint8_t nonce[UDS_CCM_NONCE], const uint8_t* aad, size_t aadLength,
                   const uint8_t* plain, size_t length, uint8_t* out) {
	struct UDSAes aes;
	udsAesInit(&aes, key);
	uint8_t tag[UDS_CCM_TAG], s0[16];
	_ccmMac(&aes, nonce, aad, aadLength, plain, length, tag);
	_ccmCtr(&aes, nonce, plain, length, out, s0);
	size_t i;
	for (i = 0; i < UDS_CCM_TAG; ++i) {
		out[length + i] = tag[i] ^ s0[i];
	}
}

bool udsCcmDecrypt(const uint8_t key[16], const uint8_t nonce[UDS_CCM_NONCE], const uint8_t* aad, size_t aadLength,
                   const uint8_t* in, size_t length, uint8_t* out) {
	struct UDSAes aes;
	udsAesInit(&aes, key);
	uint8_t tag[UDS_CCM_TAG], s0[16];
	_ccmCtr(&aes, nonce, in, length, out, s0);
	_ccmMac(&aes, nonce, aad, aadLength, out, length, tag);
	uint8_t diff = 0;
	size_t i;
	for (i = 0; i < UDS_CCM_TAG; ++i) {
		diff |= (uint8_t) (in[length + i] ^ (tag[i] ^ s0[i]));
	}
	return diff == 0;
}

// The data key -----------------------------------------------------------------------------------------------------------------

void udsCcmpDeriveKey(const uint8_t slotKey[16], const uint8_t* passphrase, size_t passphraseLength, uint32_t commId,
                      uint32_t networkId, const uint8_t hostMac[6], uint8_t id, uint8_t out[16]) {
	uint8_t passphraseHash[16], counterInput[16], counter[16], stream[16];
	md5Buffer(passphrase, passphraseLength, passphraseHash);
	// Azahar's DataFrameCryptoCTR: comm id and network id as little-endian numbers, the host MAC, the id as a 16-bit little-endian.
	counterInput[0] = (uint8_t) commId;
	counterInput[1] = (uint8_t) (commId >> 8);
	counterInput[2] = (uint8_t) (commId >> 16);
	counterInput[3] = (uint8_t) (commId >> 24);
	counterInput[4] = (uint8_t) networkId;
	counterInput[5] = (uint8_t) (networkId >> 8);
	counterInput[6] = (uint8_t) (networkId >> 16);
	counterInput[7] = (uint8_t) (networkId >> 24);
	memcpy(&counterInput[8], hostMac, 6);
	counterInput[14] = id;
	counterInput[15] = 0;
	md5Buffer(counterInput, 16, counter);
	struct UDSAes aes;
	udsAesInit(&aes, slotKey);
	udsAesEncrypt(&aes, counter, stream); // AES-CTR over one block: the first keystream block is E(counter)
	size_t i;
	for (i = 0; i < 16; ++i) {
		out[i] = passphraseHash[i] ^ stream[i];
	}
}

// Frames -----------------------------------------------------------------------------------------------------------------------

static void _put16(uint8_t* p, unsigned v) {
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
}

static void _aad(uint8_t out[22], uint16_t frameControl, const uint8_t a1[6], const uint8_t a2[6], const uint8_t a3[6],
                 uint16_t sequenceControl) {
	_put16(&out[0], frameControl & 0xC78F);
	memcpy(&out[2], a1, 6);
	memcpy(&out[8], a2, 6);
	memcpy(&out[14], a3, 6);
	_put16(&out[20], sequenceControl & 0x000F);
}

static void _nonce(uint8_t out[UDS_CCM_NONCE], const uint8_t transmitter[6], uint64_t pn) {
	out[0] = 0; // priority
	memcpy(&out[1], transmitter, 6);
	int i;
	for (i = 0; i < 6; ++i) {
		out[7 + i] = (uint8_t) (pn >> (8 * (5 - i)));
	}
}

size_t udsBuildDataFrame(uint8_t* out, size_t capacity, const uint8_t key[16], const uint8_t* payload, size_t length,
                         const uint8_t transmitter[6], const uint8_t destination[6], const uint8_t bssid[6],
                         enum UDSDsMode mode, uint64_t packetNumber, uint16_t sequence) {
	if (capacity < UDS_DATA_OVERHEAD + length) {
		return 0;
	}
	uint16_t frameControl = 0x0008 | 0x4000;
	const uint8_t *a1, *a2, *a3;
	switch (mode) {
	case UDS_DS_TO:
		frameControl |= 0x0100;
		a1 = bssid;
		a2 = transmitter;
		a3 = destination;
		break;
	case UDS_DS_FROM:
		frameControl |= 0x0200;
		a1 = destination;
		a2 = bssid;
		a3 = transmitter;
		break;
	default:
		a1 = destination;
		a2 = transmitter;
		a3 = bssid;
		break;
	}
	uint16_t sequenceControl = (uint16_t) ((sequence & 0x0FFF) << 4);
	_put16(&out[0], frameControl);
	_put16(&out[2], 0);
	memcpy(&out[4], a1, 6);
	memcpy(&out[10], a2, 6);
	memcpy(&out[16], a3, 6);
	_put16(&out[22], sequenceControl);
	// CCMP header: PN0, PN1, reserved, key id with the extended-IV bit, PN2..PN5
	out[24] = (uint8_t) packetNumber;
	out[25] = (uint8_t) (packetNumber >> 8);
	out[26] = 0;
	out[27] = 0x20;
	out[28] = (uint8_t) (packetNumber >> 16);
	out[29] = (uint8_t) (packetNumber >> 24);
	out[30] = (uint8_t) (packetNumber >> 32);
	out[31] = (uint8_t) (packetNumber >> 40);
	// The nonce uses the transmitter address (A2 on the air, which for a FromDS frame is the BSSID).
	uint8_t nonce[UDS_CCM_NONCE], aad[22];
	_nonce(nonce, a2, packetNumber);
	_aad(aad, frameControl, a1, a2, a3, sequenceControl);
	udsCcmEncrypt(key, nonce, aad, sizeof(aad), payload, length, &out[UDS_80211_HEADER + UDS_CCMP_HEADER]);
	return UDS_DATA_OVERHEAD + length;
}

bool udsOpenDataFrame(const uint8_t key[16], const uint8_t* frame, size_t length, uint8_t* plain, size_t* plainLength,
                      struct UDSDataFrameInfo* info) {
	if (length < UDS_DATA_OVERHEAD) {
		return false;
	}
	uint16_t frameControl = (uint16_t) (frame[0] | (frame[1] << 8));
	// Data frame (type 2), protected, no QoS subtype, both DS bits clear or one of them set
	if ((frameControl & 0x0C) != 0x08 || !(frameControl & 0x4000) || (frameControl & 0x0080) ||
	    ((frameControl & 0x0300) == 0x0300)) {
		return false;
	}
	uint16_t sequenceControl = (uint16_t) (frame[22] | (frame[23] << 8));
	const uint8_t* ccmp = &frame[UDS_80211_HEADER];
	if (!(ccmp[3] & 0x20)) {
		return false;
	}
	uint64_t pn = (uint64_t) ccmp[0] | ((uint64_t) ccmp[1] << 8) | ((uint64_t) ccmp[4] << 16) | ((uint64_t) ccmp[5] << 24) |
	              ((uint64_t) ccmp[6] << 32) | ((uint64_t) ccmp[7] << 40);
	uint8_t nonce[UDS_CCM_NONCE], aad[22];
	_nonce(nonce, &frame[10], pn);
	_aad(aad, frameControl, &frame[4], &frame[10], &frame[16], sequenceControl);
	size_t body = length - UDS_DATA_OVERHEAD;
	if (!udsCcmDecrypt(key, nonce, aad, sizeof(aad), &frame[UDS_80211_HEADER + UDS_CCMP_HEADER], body, plain)) {
		return false;
	}
	*plainLength = body;
	if (info) {
		info->frameControl = frameControl;
		memcpy(info->a1, &frame[4], 6);
		memcpy(info->a2, &frame[10], 6);
		memcpy(info->a3, &frame[16], 6);
		info->sequenceControl = sequenceControl;
		info->packetNumber = pn;
	}
	return true;
}

size_t udsBuildMgmtFrame(uint8_t* out, size_t capacity, uint16_t frameControl, const uint8_t transmitter[6],
                         const uint8_t destination[6], const uint8_t bssid[6], uint16_t sequence, const uint8_t* body,
                         size_t bodyLength) {
	if (capacity < UDS_80211_HEADER + bodyLength) {
		return 0;
	}
	_put16(&out[0], frameControl);
	_put16(&out[2], 0);
	memcpy(&out[4], destination, 6);
	memcpy(&out[10], transmitter, 6);
	memcpy(&out[16], bssid, 6);
	_put16(&out[22], (sequence & 0x0FFF) << 4);
	if (bodyLength) {
		memcpy(&out[UDS_80211_HEADER], body, bodyLength);
	}
	return UDS_80211_HEADER + bodyLength;
}

size_t udsBuildAssocRequestBody(uint8_t out[30], uint32_t networkId) {
	static const char hex[] = "0123456789ABCDEF";
	size_t pos = 0;
	_put16(&out[pos], 0x0431); // capabilities
	pos += 2;
	_put16(&out[pos], 1); // listen interval
	pos += 2;
	out[pos++] = 0; // SSID
	out[pos++] = 8;
	int shift;
	for (shift = 28; shift >= 0; shift -= 4) {
		out[pos++] = (uint8_t) hex[(networkId >> shift) & 0x0F];
	}
	static const uint8_t rates[] = {0x82, 0x84, 0x8B, 0x0C, 0x12, 0x96, 0x18, 0x24};
	out[pos++] = 1;
	out[pos++] = sizeof(rates);
	memcpy(&out[pos], rates, sizeof(rates));
	pos += sizeof(rates);
	static const uint8_t extended[] = {0x30, 0x48, 0x60, 0x6C};
	out[pos++] = 50;
	out[pos++] = sizeof(extended);
	memcpy(&out[pos], extended, sizeof(extended));
	pos += sizeof(extended);
	return pos;
}
