/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_JOINER_H
#define GB_SIO_UDS_JOINER_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gb/sio/uds-air-radio.h>
#include <mgba/internal/gb/sio/uds-room.h>
#include <mgba/internal/gb/sio/uds-session.h>
#include <mgba/internal/gb/sio/uds-udp.h>

/*
 * Everything the Virtual Console wrapper needs below the Game Boy link driver, for the Azahar test bridge: the UDP pair, the
 * join (uds-room.c) and the Pia session (uds-session.c) wired together. Poll it regularly; once udsJoinerReady() is true the
 * link driver exchanges Game Boy bytes through udsSessionQueueUnit/PopUnit on `session`.
 *
 * Not connected to mGBA yet: nothing creates one.
 */

struct UDSJoiner {
	struct UDSUdp udp; // the Azahar test bridge's air, or
	struct UDSAirRadio radio; // the real air through the ESP32 board (useRadio)
	bool useRadio;
	struct UDSRoom room;
	struct UDSSession session;
	bool sessionActive; // the session is built once the host has accepted us
	uint32_t nowMs;
	uint16_t name[UDS_NAME_WORDS];
};

// Opens the sockets (from the environment when a port is 0) and starts scanning for a host.
bool udsJoinerOpen(struct UDSJoiner* joiner, const uint16_t name[UDS_NAME_WORDS], uint16_t listenPort, uint16_t sendPort);
// The same on the real air: the ESP32 board on `portName` (NULL: find it), the UDS key file at `keyPath`. False with a reason in `error`.
bool udsJoinerOpenRadio(struct UDSJoiner* joiner, const uint16_t name[UDS_NAME_WORDS], const char* portName, const char* keyPath, char* error,
                        size_t errorSize);
void udsJoinerClose(struct UDSJoiner* joiner);
void udsJoinerPoll(struct UDSJoiner* joiner, uint32_t nowMs);
bool udsJoinerReady(const struct UDSJoiner* joiner);

CXX_GUARD_END

#endif
