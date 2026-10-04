/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Test of uds-ccmp.c: the published AES and CCM test vectors first (FIPS-197 appendix C.1, RFC 3610 packet vector 1), then
 * the frame builders and parser with made-up keys. Golden frames from Azahar's logs are checked by uds-ccmp-golden (separate).
 * Build target: uds-ccmp-test. Exit status 0 when every check passes. No real key is used or printed.
 */
#include <mgba/internal/gb/sio/uds-ccmp.h>

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond, ...) \
	do { \
		++checks; \
		if (!(cond)) { \
			++failures; \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

static void testAes(void) {
	// FIPS-197 C.1: key 000102..0f, plaintext 00112233..ff
	uint8_t key[16], in[16], out[16];
	int i;
	for (i = 0; i < 16; ++i) {
		key[i] = (uint8_t) i;
		in[i] = (uint8_t) (i * 0x11);
	}
	struct UDSAes aes;
	udsAesInit(&aes, key);
	udsAesEncrypt(&aes, in, out);
	static const uint8_t want[16] = {0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30, 0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A};
	CHECK(!memcmp(out, want, 16), "FIPS-197 C.1 vector");

	// FIPS-197 appendix B: key 2b7e1516 28aed2a6 abf71588 09cf4f3c, plaintext 3243f6a8 885a308d 313198a2 e0370734
	static const uint8_t key2[16] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6, 0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};
	static const uint8_t in2[16] = {0x32, 0x43, 0xF6, 0xA8, 0x88, 0x5A, 0x30, 0x8D, 0x31, 0x31, 0x98, 0xA2, 0xE0, 0x37, 0x07, 0x34};
	static const uint8_t want2[16] = {0x39, 0x25, 0x84, 0x1D, 0x02, 0xDC, 0x09, 0xFB, 0xDC, 0x11, 0x85, 0x97, 0x19, 0x6A, 0x0B, 0x32};
	udsAesInit(&aes, key2);
	udsAesEncrypt(&aes, in2, out);
	CHECK(!memcmp(out, want2, 16), "FIPS-197 appendix B vector");
}

static void testCcm(void) {
	// RFC 3610 packet vector 1: M = 8, L = 2
	uint8_t key[16], plain[23], out[23 + 8], back[23];
	int i;
	for (i = 0; i < 16; ++i) {
		key[i] = (uint8_t) (0xC0 + i);
	}
	for (i = 0; i < 23; ++i) {
		plain[i] = (uint8_t) (8 + i);
	}
	static const uint8_t nonce[13] = {0x00, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
	static const uint8_t aad[8] = {0, 1, 2, 3, 4, 5, 6, 7};
	static const uint8_t want[31] = {0x58, 0x8C, 0x97, 0x9A, 0x61, 0xC6, 0x63, 0xD2, 0xF0, 0x66, 0xD0, 0xC2, 0xC0, 0xF9, 0x89, 0x80,
	                                 0x6D, 0x5F, 0x6B, 0x61, 0xDA, 0xC3, 0x84, 0x17, 0xE8, 0xD1, 0x2C, 0xFD, 0xF9, 0x26, 0xE0};
	udsCcmEncrypt(key, nonce, aad, sizeof(aad), plain, sizeof(plain), out);
	CHECK(!memcmp(out, want, sizeof(want)), "RFC 3610 packet vector 1");
	CHECK(udsCcmDecrypt(key, nonce, aad, sizeof(aad), out, sizeof(plain), back) && !memcmp(back, plain, sizeof(plain)),
	      "decrypting the RFC 3610 vector");
	out[3] ^= 1;
	CHECK(!udsCcmDecrypt(key, nonce, aad, sizeof(aad), out, sizeof(plain), back), "a flipped ciphertext bit must fail");
	out[3] ^= 1;
	out[28] ^= 1;
	CHECK(!udsCcmDecrypt(key, nonce, aad, sizeof(aad), out, sizeof(plain), back), "a flipped tag bit must fail");
}

static void testFrames(void) {
	uint8_t slotKey[16], dataKey[16], dataKey2[16];
	int i;
	for (i = 0; i < 16; ++i) {
		slotKey[i] = (uint8_t) (0x10 + 3 * i);
	}
	static const uint8_t host[6] = {0xB8, 0xAE, 0x6E, 0xA8, 0xD0, 0x10};
	static const uint8_t station[6] = {0x02, 0x47, 0x42, 0x01, 0x02, 0x03};
	static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	static const uint8_t phrase[12] = {'T', 'R', 'L', '_', 'N', 'E', 'T', 'W', 'O', 'R', 'K', 0};
	udsCcmpDeriveKey(slotKey, phrase, sizeof(phrase), 0x00171010, 0x76E82DC4, host, 1, dataKey);
	udsCcmpDeriveKey(slotKey, phrase, sizeof(phrase), 0x00171010, 0x76E82DC4, host, 1, dataKey2);
	CHECK(!memcmp(dataKey, dataKey2, 16), "the data key is deterministic");
	udsCcmpDeriveKey(slotKey, phrase, sizeof(phrase), 0x00171010, 0x76E82DC5, host, 1, dataKey2);
	CHECK(memcmp(dataKey, dataKey2, 16), "the network id changes the data key");
	udsCcmpDeriveKey(slotKey, phrase, 11, 0x00171010, 0x76E82DC4, host, 1, dataKey2);
	CHECK(memcmp(dataKey, dataKey2, 16), "the passphrase's terminating NUL counts");

	static const uint8_t payload[40] = {0xAA, 0xAA, 0x03, 0, 0, 0, 0x87, 0x6D, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
	                                    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
	enum UDSDsMode modes[3] = {UDS_DS_NONE, UDS_DS_TO, UDS_DS_FROM};
	unsigned m;
	for (m = 0; m < 3; ++m) {
		uint8_t frame[256], plain[256];
		size_t plainLength = 0;
		struct UDSDataFrameInfo info;
		const uint8_t* transmitter = m == 2 ? host : station;
		const uint8_t* destination = m == 0 ? broadcast : m == 1 ? host : station;
		size_t size = udsBuildDataFrame(frame, sizeof(frame), dataKey, payload, sizeof(payload), transmitter, destination, host,
		                                modes[m], 0x0123456789ULL, 0x0ABC);
		CHECK(size == UDS_DATA_OVERHEAD + sizeof(payload), "frame size %zu", size);
		CHECK(udsOpenDataFrame(dataKey, frame, size, plain, &plainLength, &info) && plainLength == sizeof(payload) &&
		          !memcmp(plain, payload, sizeof(payload)),
		      "mode %u: the frame opens to its payload", m);
		CHECK(info.packetNumber == 0x0123456789ULL && info.sequenceControl == (0x0ABC << 4), "mode %u: packet and sequence number", m);
		uint16_t ds = info.frameControl & 0x0300;
		CHECK(ds == (m == 0 ? 0 : m == 1 ? 0x0100 : 0x0200), "mode %u: DS bits %04X", m, ds);
		CHECK(info.frameControl & 0x4000, "protected");
		frame[size - 1] ^= 1;
		CHECK(!udsOpenDataFrame(dataKey, frame, size, plain, &plainLength, NULL), "mode %u: a changed tag is refused", m);
		frame[size - 1] ^= 1;
		frame[5] ^= 1; // an address is part of the AAD
		CHECK(!udsOpenDataFrame(dataKey, frame, size, plain, &plainLength, NULL), "mode %u: a changed address is refused", m);
		frame[5] ^= 1;
		CHECK(!udsOpenDataFrame(dataKey2, frame, size, plain, &plainLength, NULL), "mode %u: another network's key is refused", m);
	}

	// Management frames and the association request body
	uint8_t body[30], frame[64];
	CHECK(udsBuildAssocRequestBody(body, 0x76E82DC4) == 30, "association request body size");
	static const uint8_t wantBody[30] = {0x31, 0x04, 0x01, 0x00, 0x00, 0x08, '7', '6', 'E', '8', '2', 'D', 'C', '4', 0x01, 0x08,
	                                     0x82, 0x84, 0x8B, 0x0C, 0x12, 0x96, 0x18, 0x24, 0x32, 0x04, 0x30, 0x48, 0x60, 0x6C};
	CHECK(!memcmp(body, wantBody, 30), "association request body");
	size_t size = udsBuildMgmtFrame(frame, sizeof(frame), UDS_FC_ASSOC_REQUEST, station, host, host, 5, body, 30);
	CHECK(size == 54 && frame[0] == 0x00 && frame[1] == 0x00 && !memcmp(&frame[4], host, 6) && !memcmp(&frame[10], station, 6) &&
	          !memcmp(&frame[16], host, 6) && frame[22] == 0x50 && frame[23] == 0x00 && !memcmp(&frame[24], body, 30),
	      "management frame layout");
	uint8_t auth[6] = {0, 0, 1, 0, 0, 0};
	size = udsBuildMgmtFrame(frame, sizeof(frame), UDS_FC_AUTH, station, host, host, 0, auth, sizeof(auth));
	CHECK(size == 30 && frame[0] == 0xB0 && frame[1] == 0x00, "authentication frame control");
}

int main(void) {
	testAes();
	testCcm();
	testFrames();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
