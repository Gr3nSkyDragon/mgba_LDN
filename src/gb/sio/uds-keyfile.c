/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-keyfile.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UDS_SLOT 0x2D

// The 3DS key generator's constant. It is not console specific (the same on every 3DS), so a file that has KeyX and KeyY
// but no generatorConstant still gives the key. A generatorConstant in the file takes precedence.
static const uint8_t sGeneratorConstant[16] = {0x1F, 0xF9, 0xE9, 0xAA, 0xC5, 0xFE, 0x04, 0x08,
                                               0x02, 0x45, 0x91, 0xDC, 0x5D, 0x52, 0x76, 0x8A};

static void _lrot128(const uint8_t in[16], unsigned rot, uint8_t out[16]) {
	rot %= 128;
	unsigned byteShift = rot / 8;
	unsigned bitShift = rot % 8;
	unsigned i;
	for (i = 0; i < 16; ++i) {
		unsigned a = (i + byteShift) % 16;
		unsigned b = (i + byteShift + 1) % 16;
		out[i] = (uint8_t) (((in[a] << bitShift) | (in[b] >> (8 - bitShift))) & 0xFF);
	}
}

static void _add128(const uint8_t a[16], const uint8_t b[16], uint8_t out[16]) {
	unsigned carry = 0;
	int i;
	for (i = 15; i >= 0; --i) {
		unsigned sum = a[i] + b[i] + carry;
		carry = sum >> 8;
		out[i] = (uint8_t) sum;
	}
}

void udsKeyDerive(const uint8_t keyX[16], const uint8_t keyY[16], const uint8_t generator[16], uint8_t out[16]) {
	uint8_t rotated[16], mixed[16], sum[16];
	unsigned i;
	_lrot128(keyX, 2, rotated);
	for (i = 0; i < 16; ++i) {
		mixed[i] = rotated[i] ^ keyY[i];
	}
	_add128(mixed, generator, sum);
	_lrot128(sum, 87, out);
}

static int _hexValue(int c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

// 32 hex digits at the start of the value (anything after them, like a trailing comment, is ignored).
static bool _parseKey(const char* value, size_t length, uint8_t out[16]) {
	if (length < 32) {
		return false;
	}
	size_t i;
	for (i = 0; i < 16; ++i) {
		int hi = _hexValue((unsigned char) value[2 * i]);
		int lo = _hexValue((unsigned char) value[2 * i + 1]);
		if (hi < 0 || lo < 0) {
			return false;
		}
		out[i] = (uint8_t) ((hi << 4) | lo);
	}
	return true;
}

// "slot0x<hex>Key<type>": returns the type character and the slot, or 0.
static char _slotName(const char* name, size_t length, unsigned* slot) {
	static const char prefix[] = "slot0x";
	if (length < sizeof(prefix) - 1 + 1 + 3 + 1 || strncmp(name, prefix, sizeof(prefix) - 1) != 0) {
		return 0;
	}
	size_t pos = sizeof(prefix) - 1;
	unsigned value = 0;
	size_t digits = 0;
	while (pos < length && _hexValue((unsigned char) name[pos]) >= 0 && digits < 4) {
		value = (value << 4) | (unsigned) _hexValue((unsigned char) name[pos]);
		++pos;
		++digits;
	}
	if (!digits || pos + 4 != length || strncmp(&name[pos], "Key", 3) != 0) {
		return 0;
	}
	*slot = value;
	return name[pos + 3];
}

enum UDSKeyStatus udsKeyTextLoad(const char* text, size_t length, uint8_t key[16]) {
	uint8_t keyN[16], keyX[16], keyY[16], generator[16];
	bool haveN = false, haveX = false, haveY = false, haveGen = false, bad = false;
	size_t pos = 0;
	while (pos < length) {
		size_t end = pos;
		while (end < length && text[end] != '\n' && text[end] != '\r') {
			++end;
		}
		const char* line = &text[pos];
		size_t size = end - pos;
		pos = end + 1;
		while (size && isspace((unsigned char) *line)) {
			++line;
			--size;
		}
		if (!size || line[0] == '#' || line[0] == ';' || (size > 1 && line[0] == '/' && line[1] == '/')) {
			continue;
		}
		const char* equals = memchr(line, '=', size);
		if (!equals) {
			continue;
		}
		size_t nameLength = (size_t) (equals - line);
		while (nameLength && isspace((unsigned char) line[nameLength - 1])) {
			--nameLength;
		}
		const char* value = equals + 1;
		size_t valueLength = size - (size_t) (value - line);
		while (valueLength && isspace((unsigned char) *value)) {
			++value;
			--valueLength;
		}

		unsigned slot = 0;
		char type = _slotName(line, nameLength, &slot);
		if (type && slot == UDS_SLOT && (type == 'N' || type == 'X' || type == 'Y')) {
			uint8_t* target = type == 'N' ? keyN : type == 'X' ? keyX : keyY;
			bool* have = type == 'N' ? &haveN : type == 'X' ? &haveX : &haveY;
			if (_parseKey(value, valueLength, target)) {
				*have = true;
			} else {
				bad = true;
			}
		} else if ((nameLength == 17 && strncmp(line, "generatorConstant", 17) == 0) ||
		           (nameLength == 9 && strncmp(line, "generator", 9) == 0)) {
			if (_parseKey(value, valueLength, generator)) {
				haveGen = true;
			} else {
				bad = true;
			}
		}
	}

	enum UDSKeyStatus status;
	if (haveN) {
		memcpy(key, keyN, 16);
		status = UDS_KEY_OK_NORMAL;
	} else if (haveX && haveY) {
		udsKeyDerive(keyX, keyY, haveGen ? generator : sGeneratorConstant, key);
		status = UDS_KEY_OK_DERIVED;
	} else if (bad) {
		status = UDS_KEY_BAD_VALUE;
	} else if (haveX || haveY) {
		status = UDS_KEY_INCOMPLETE;
	} else {
		status = UDS_KEY_NOT_FOUND;
	}
	// Do not leave key material on the stack.
	memset(keyN, 0, sizeof(keyN));
	memset(keyX, 0, sizeof(keyX));
	memset(keyY, 0, sizeof(keyY));
	memset(generator, 0, sizeof(generator));
	return status;
}

enum UDSKeyStatus udsKeyFileLoad(const char* path, uint8_t key[16]) {
	if (!path || !*path) {
		return UDS_KEY_NO_FILE;
	}
	FILE* file = fopen(path, "rb");
	if (!file) {
		return UDS_KEY_NO_FILE;
	}
	// A key file is a few kilobytes at most; a full aes_keys.txt is about 10 KiB.
	char* text = malloc(1 << 20);
	if (!text) {
		fclose(file);
		return UDS_KEY_NO_FILE;
	}
	size_t length = fread(text, 1, 1 << 20, file);
	fclose(file);
	enum UDSKeyStatus status = udsKeyTextLoad(text, length, key);
	memset(text, 0, length);
	free(text);
	return status;
}

bool udsKeyFileWrite(const char* path, const uint8_t key[16]) {
	static const char hex[] = "0123456789ABCDEF";
	char line[128];
	size_t pos = (size_t) snprintf(line, sizeof(line), "# UDS data key (key slot 0x2D) made by mGBA. Keep it private.\nslot0x2DKeyN=");
	size_t i;
	for (i = 0; i < 16; ++i) {
		line[pos++] = hex[key[i] >> 4];
		line[pos++] = hex[key[i] & 0x0F];
	}
	line[pos++] = '\n';
	FILE* file = fopen(path, "wb");
	if (!file) {
		memset(line, 0, sizeof(line));
		return false;
	}
	bool ok = fwrite(line, 1, pos, file) == pos;
	ok = fclose(file) == 0 && ok;
	memset(line, 0, sizeof(line));
	return ok;
}

bool udsKeyStatusOk(enum UDSKeyStatus status) {
	return status == UDS_KEY_OK_NORMAL || status == UDS_KEY_OK_DERIVED;
}

const char* udsKeyStatusText(enum UDSKeyStatus status) {
	switch (status) {
	case UDS_KEY_OK_NORMAL:
		return "UDS key loaded (slot0x2DKeyN)";
	case UDS_KEY_OK_DERIVED:
		return "UDS key made from slot0x2DKeyX and slot0x2DKeyY";
	case UDS_KEY_NO_FILE:
		return "no UDS key file set, or it cannot be read";
	case UDS_KEY_NOT_FOUND:
		return "the file has no slot0x2D key";
	case UDS_KEY_INCOMPLETE:
		return "the file has slot0x2DKeyX or slot0x2DKeyY but not both, and no slot0x2DKeyN";
	case UDS_KEY_BAD_VALUE:
		return "a slot 0x2D or generator line is not 32 hex digits";
	}
	return "unknown";
}
