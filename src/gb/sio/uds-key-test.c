/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Test of the key-file reader (uds-keyfile.c) with made-up values, never real keys. Build target: uds-key-test.
 * With the environment variables MGBA_UDS_KEYFILE (the file mGBA's setting points at) and MGBA_UDS_KEYFILE_FULL (a full
 * aes_keys.txt) set, it also checks that the file loads and that the two files give the same key. It prints no key material.
 */
#include <mgba/internal/gb/sio/uds-keyfile.h>

#include <stdio.h>
#include <stdlib.h>
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

#define LOAD(text, key) udsKeyTextLoad(text, strlen(text), key)

static void testDerive(void) {
	// X = Y = 0 and generator = 1: the sum is 1, rotated left by 87 bits that is bit 87, which is bit 7 of byte 5.
	uint8_t zero[16] = {0}, one[16] = {0}, out[16];
	one[15] = 1;
	udsKeyDerive(zero, zero, one, out);
	uint8_t want[16] = {0};
	want[5] = 0x80;
	CHECK(!memcmp(out, want, 16), "rotate of the generator");

	// KeyX = 1 << 126 (top byte 0x40): rotated left by 2 it becomes 1 << 128, i.e. wraps to bit 0 of the last byte.
	uint8_t x[16] = {0};
	x[0] = 0x40;
	udsKeyDerive(x, zero, zero, out);
	memset(want, 0, 16);
	want[5] = 0x80; // the sum is 1 again
	CHECK(!memcmp(out, want, 16), "KeyX rotation wraps around");

	// Y is xored in, the generator added with carry across bytes: Y = 0xFF in the last byte, generator = 1 -> 0x100.
	uint8_t y[16] = {0};
	y[15] = 0xFF;
	udsKeyDerive(zero, y, one, out);
	// the sum is 0x100: bit 8; rotated left by 87 it is bit 95: byte 15 - 11 = 4, bit 7
	memset(want, 0, 16);
	want[4] = 0x80;
	CHECK(!memcmp(out, want, 16), "carry through the 128-bit add");
}

static void testParse(void) {
	uint8_t key[16];
	static const char normal[] = "# comment\r\n; another\r\nslot0x2DKeyN=000102030405060708090A0B0C0D0E0F\r\nslot0x2DKeyX=FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\r\n";
	CHECK(LOAD(normal, key) == UDS_KEY_OK_NORMAL, "KeyN preferred");
	CHECK(key[0] == 0x00 && key[1] == 0x01 && key[15] == 0x0F, "KeyN value");

	static const char lower[] = "slot0x2dKeyN = 000102030405060708090a0b0c0d0e0f  # trailing comment\n";
	CHECK(LOAD(lower, key) == UDS_KEY_OK_NORMAL && key[10] == 0x0A, "lower-case slot number and hex, spaces around =");

	static const char derived[] =
	    "slot0x2DKeyX=00000000000000000000000000000000\n"
	    "slot0x2DKeyY=00000000000000000000000000000000\n"
	    "generatorConstant=00000000000000000000000000000001\n";
	CHECK(LOAD(derived, key) == UDS_KEY_OK_DERIVED && key[5] == 0x80 && key[0] == 0, "derived from X, Y and generatorConstant");

	static const char alias[] =
	    "slot0x2DKeyX=00000000000000000000000000000000\n"
	    "slot0x2DKeyY=00000000000000000000000000000000\n"
	    "generator=00000000000000000000000000000001\n";
	CHECK(LOAD(alias, key) == UDS_KEY_OK_DERIVED && key[5] == 0x80, "the name `generator` is accepted too");

	static const char otherSlot[] =
	    "slot0x2CKeyN=000102030405060708090A0B0C0D0E0F\n"
	    "slot0x31KeyX=000102030405060708090A0B0C0D0E0F\n"
	    "slot0x31KeyY=000102030405060708090A0B0C0D0E0F\n"
	    "generatorConstant=00000000000000000000000000000001\n";
	CHECK(LOAD(otherSlot, key) == UDS_KEY_NOT_FOUND, "other slots are not mistaken for 0x2D");

	// Without a generator line the built-in constant is used: the result must equal the explicit derivation with it.
	static const char noGen[] =
	    "slot0x2DKeyX=000102030405060708090A0B0C0D0E0F\nslot0x2DKeyY=F0E0D0C0B0A090807060504030201000\n";
	static const uint8_t builtin[16] = {0x1F, 0xF9, 0xE9, 0xAA, 0xC5, 0xFE, 0x04, 0x08, 0x02, 0x45, 0x91, 0xDC, 0x5D, 0x52, 0x76, 0x8A};
	static const uint8_t xs[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
	static const uint8_t ys[16] = {0xF0, 0xE0, 0xD0, 0xC0, 0xB0, 0xA0, 0x90, 0x80, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10, 0x00};
	uint8_t explicitKey[16];
	udsKeyDerive(xs, ys, builtin, explicitKey);
	CHECK(LOAD(noGen, key) == UDS_KEY_OK_DERIVED && !memcmp(key, explicitKey, 16), "no generator line: the built-in constant is used");

	// A generator line in the file wins over the built-in one.
	static const char ownGen[] =
	    "slot0x2DKeyX=000102030405060708090A0B0C0D0E0F\nslot0x2DKeyY=F0E0D0C0B0A090807060504030201000\n"
	    "generatorConstant=00000000000000000000000000000001\n";
	CHECK(LOAD(ownGen, key) == UDS_KEY_OK_DERIVED && memcmp(key, explicitKey, 16), "a generatorConstant line overrides the built-in one");

	static const char onlyX[] = "slot0x2DKeyX=00000000000000000000000000000000\ngeneratorConstant=00000000000000000000000000000001\n";
	CHECK(LOAD(onlyX, key) == UDS_KEY_INCOMPLETE, "KeyY missing");
	static const char onlyY[] = "slot0x2DKeyY=00000000000000000000000000000000\n";
	CHECK(LOAD(onlyY, key) == UDS_KEY_INCOMPLETE, "KeyX missing");

	static const char bad[] = "slot0x2DKeyN=0001020304\n";
	CHECK(LOAD(bad, key) == UDS_KEY_BAD_VALUE, "a short value");

	static const char nonHex[] = "slot0x2DKeyN=ZZ0102030405060708090A0B0C0D0E0F\n";
	CHECK(LOAD(nonHex, key) == UDS_KEY_BAD_VALUE, "a non-hex value");

	CHECK(LOAD("", key) == UDS_KEY_NOT_FOUND, "an empty file");
	CHECK(udsKeyFileLoad("", key) == UDS_KEY_NO_FILE && udsKeyFileLoad(NULL, key) == UDS_KEY_NO_FILE, "no path");
	CHECK(udsKeyFileLoad("this/file/does/not/exist.txt", key) == UDS_KEY_NO_FILE, "a missing file");

	// A bad line for another key does not spoil a good KeyN.
	static const char mixed[] = "slot0x2DKeyX=oops\nslot0x2DKeyN=000102030405060708090A0B0C0D0E0F\n";
	CHECK(LOAD(mixed, key) == UDS_KEY_OK_NORMAL, "a bad KeyX does not matter when KeyN is there");
}

static void testWrite(void) {
	// A key made from KeyX and KeyY, written out and read back, is the same key.
	static const char derived[] =
	    "slot0x2DKeyX=000102030405060708090A0B0C0D0E0F\nslot0x2DKeyY=F0E0D0C0B0A090807060504030201000\n";
	uint8_t made[16], back[16];
	CHECK(LOAD(derived, made) == UDS_KEY_OK_DERIVED, "made from X and Y");
	const char* path = "uds-key-test-written.txt";
	CHECK(udsKeyFileWrite(path, made), "write");
	CHECK(udsKeyFileLoad(path, back) == UDS_KEY_OK_NORMAL && !memcmp(made, back, 16), "the written file loads as KeyN, same key");
	CHECK(!udsKeyFileWrite("this/directory/does/not/exist/key.txt", made), "a write to a missing directory fails");
	remove(path);
	memset(made, 0, sizeof(made));
	memset(back, 0, sizeof(back));
}

static void testRealFiles(void) {
	const char* exported = getenv("MGBA_UDS_KEYFILE");
	const char* full = getenv("MGBA_UDS_KEYFILE_FULL");
	uint8_t a[16], b[16];
	if (exported && *exported) {
		enum UDSKeyStatus status = udsKeyFileLoad(exported, a);
		CHECK(udsKeyStatusOk(status), "MGBA_UDS_KEYFILE: %s", udsKeyStatusText(status));
		printf("MGBA_UDS_KEYFILE: %s\n", udsKeyStatusText(status));
	}
	if (full && *full) {
		enum UDSKeyStatus status = udsKeyFileLoad(full, b);
		CHECK(udsKeyStatusOk(status), "MGBA_UDS_KEYFILE_FULL: %s", udsKeyStatusText(status));
		printf("MGBA_UDS_KEYFILE_FULL: %s\n", udsKeyStatusText(status));
		if (exported && *exported && udsKeyStatusOk(status)) {
			CHECK(!memcmp(a, b, 16), "the exported key and the one made from the full file differ");
			printf("exported key %s the key made from the full file\n", memcmp(a, b, 16) ? "DIFFERS from" : "matches");
		}
	}
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
}

int main(void) {
	testDerive();
	testParse();
	testWrite();
	testRealFiles();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
