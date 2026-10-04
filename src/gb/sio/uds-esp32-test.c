/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Test of the framing in uds-esp32.c, no hardware needed. Three frames whose encodings were produced by an independent
 * implementation (the ones the firmware's own host test uses) check that the two sides agree on the byte level; the rest are
 * round trips over COBS block boundaries, corrupt-frame handling and resynchronisation. Build target: uds-esp32-test.
 */
#include <mgba/internal/gb/sio/uds-esp32.h>

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

static size_t fromHex(const char* hex, uint8_t* out) {
	size_t n = strlen(hex) / 2, i;
	for (i = 0; i < n; ++i) {
		unsigned v;
		sscanf(hex + 2 * i, "%2x", &v);
		out[i] = (uint8_t) v;
	}
	return n;
}

static struct UDSEsp32Decoder decoder;
static struct UDSEsp32Frame frame;

static int feedAll(const uint8_t* data, size_t length) {
	int completed = 0;
	size_t i;
	for (i = 0; i < length; ++i) {
		if (udsEsp32DecoderFeed(&decoder, data[i], &frame)) {
			++completed;
		}
	}
	return completed;
}

static void golden(const char* hex, uint8_t type, uint8_t seq, uint8_t flags, const uint8_t* payload, size_t length) {
	uint8_t expected[UDS_ESP32_MAX_ENCODED], actual[UDS_ESP32_MAX_ENCODED];
	size_t expectedLength = fromHex(hex, expected);
	size_t actualLength = udsEsp32Encode(type, seq, flags, payload, length, actual, sizeof(actual));
	CHECK(actualLength == expectedLength && !memcmp(actual, expected, expectedLength), "encoding of type %02X differs from the golden frame", type);
	udsEsp32DecoderInit(&decoder);
	CHECK(feedAll(expected, expectedLength) == 1, "the golden frame did not decode");
	CHECK(frame.type == type && frame.seq == seq && frame.flags == flags && frame.length == length &&
	          (!length || !memcmp(frame.payload, payload, length)),
	      "golden frame fields");
}

int main(void) {
	CHECK(udsEsp32Crc32((const uint8_t*) "123456789", 9) == 0xCBF43926u, "CRC-32 check value");

	golden("04010101010105d33c42ff00", 0x01, 1, 0, NULL, 0);
	static const uint8_t start[] = {6, 0x00, 0x1F, 0x32, 0xAA, 0x00, 0x01};
	golden("0401020202070206041f32aa0601f3387e7a00", 0x02, 2, 0, start, sizeof(start));
	uint8_t rx[42] = {6, 0xC4};
	int i;
	for (i = 0; i < 40; ++i) {
		rx[2 + i] = (uint8_t) i;
	}
	golden("03018303012a0306c42c0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20212223242526271ee9661600", 0x83, 0, 1, rx,
	       sizeof(rx));

	// Round trips over every payload size around the COBS block limit, with runs of zeros and 0xFF
	static uint8_t payload[UDS_ESP32_MAX_PAYLOAD], encoded[UDS_ESP32_MAX_ENCODED];
	size_t length;
	for (length = 0; length <= UDS_ESP32_MAX_PAYLOAD; length += (length < 600 ? 1 : 97)) {
		size_t k;
		for (k = 0; k < length; ++k) {
			payload[k] = (length % 3 == 0) ? 0 : (length % 3 == 1 ? 1 : (uint8_t) (k % 5 ? 0xFF : 0));
		}
		size_t n = udsEsp32Encode(0x06, 7, 0, payload, length, encoded, sizeof(encoded));
		udsEsp32DecoderInit(&decoder);
		CHECK(n > 0 && feedAll(encoded, n) == 1 && frame.length == length && (!length || !memcmp(frame.payload, payload, length)),
		      "round trip of %zu bytes", length);
	}
	CHECK(udsEsp32Encode(0x06, 0, 0, payload, UDS_ESP32_MAX_PAYLOAD + 1, encoded, sizeof(encoded)) == 0, "a payload that is too big");
	CHECK(udsEsp32Encode(0x01, 0, 0, NULL, 0, encoded, 4) == 0, "a buffer that is too small");

	// A corrupt frame is counted and dropped; the next good frame still decodes.
	uint8_t good[UDS_ESP32_MAX_ENCODED], bad[UDS_ESP32_MAX_ENCODED];
	size_t goodLength = udsEsp32Encode(0x09, 3, 0, (const uint8_t*) "abcd", 4, good, sizeof(good));
	memcpy(bad, good, goodLength);
	bad[3] ^= 0x10;
	udsEsp32DecoderInit(&decoder);
	CHECK(feedAll(bad, goodLength) == 0 && decoder.framesBad == 1, "a corrupt frame is dropped and counted");
	CHECK(feedAll(good, goodLength) == 1 && frame.type == 0x09 && !memcmp(frame.payload, "abcd", 4), "the next good frame decodes");
	// Noise, idle delimiters and a frame split across reads
	uint8_t noise[] = {0, 0, 5, 7, 0, 0};
	udsEsp32DecoderInit(&decoder);
	CHECK(feedAll(noise, sizeof(noise)) == 0, "noise decodes to nothing");
	int completed = feedAll(good, goodLength / 2);
	completed += feedAll(good + goodLength / 2, goodLength - goodLength / 2);
	CHECK(completed == 1 && frame.seq == 3, "a frame split across reads");
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
