/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_KEYFILE_H
#define GB_SIO_UDS_KEYFILE_H

#include <mgba-util/common.h>

CXX_GUARD_START

/*
 * The 3DS UDS data key (AES key slot 0x2D) for the real-air side of the Virtual Console wrapper. It is a Nintendo secret that is
 * the same on every console; it is never shipped with mGBA and never logged. The user supplies it as a file in the format
 * Citra and Azahar read (one `name=hex` line per key, `#` or `;` or `//` start a comment), and Settings > BIOS points at it.
 *
 * The file may be
 *   - an Azahar export with the finished key:      slot0x2DKeyN=<32 hex digits>
 *   - or a full aes_keys.txt: slot0x2DKeyX and slot0x2DKeyY plus generatorConstant (or generator), from which the key is made as
 *     Azahar does: normal = ((KeyX <<< 2) xor KeyY) + generator, rotated left 87, all as 128-bit big-endian values.
 * Everything else in the file is ignored; if the finished key and the three ingredients are both present the finished key is used.
 */

enum UDSKeyStatus {
	UDS_KEY_OK_NORMAL, // slot0x2DKeyN was in the file
	UDS_KEY_OK_DERIVED, // made from KeyX, KeyY and the generator constant
	UDS_KEY_NO_FILE, // empty path or the file cannot be read
	UDS_KEY_NOT_FOUND, // no slot 0x2D key in the file
	UDS_KEY_INCOMPLETE, // KeyX or KeyY or the generator constant is missing, and there is no KeyN
	UDS_KEY_BAD_VALUE, // a line for slot 0x2D (or the generator) is not 32 hex digits
};

enum UDSKeyStatus udsKeyFileLoad(const char* path, uint8_t key[16]);
// The same, from the text of a file (for tests).
enum UDSKeyStatus udsKeyTextLoad(const char* text, size_t length, uint8_t key[16]);
const char* udsKeyStatusText(enum UDSKeyStatus status);
bool udsKeyStatusOk(enum UDSKeyStatus status);

// The 128-bit arithmetic of the key generator, exposed for the test.
void udsKeyDerive(const uint8_t keyX[16], const uint8_t keyY[16], const uint8_t generator[16], uint8_t out[16]);

CXX_GUARD_END

#endif
