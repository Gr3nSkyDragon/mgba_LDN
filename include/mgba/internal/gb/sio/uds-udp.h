/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_UDP_H
#define GB_SIO_UDS_UDP_H

#include <mgba-util/common.h>

CXX_GUARD_START

/*
 * The localhost UDP pair that carries the Azahar test bridge's datagrams (uds-room.h). Used only by the Virtual Console
 * wrapper when Wireless Adapter > Local is chosen; the RFU adapter's own Local backend (rfu-udp.c, ports 45600..45607) is a
 * different protocol and is untouched.
 *
 * Azahar listens on port N and sends to N + 1; this side listens on N + 1 and sends to N. N is 45710 unless the environment
 * variable AZAHAR_UDS_BRIDGE (the same variable that switches the bridge on in Azahar) holds a larger number.
 */

struct UDSUdp {
	intptr_t sock; // a SOCKET on Windows
	uint16_t listenPort;
	uint16_t sendPort;
	bool open;
};

// Ports from AZAHAR_UDS_BRIDGE (or the default), as seen from mGBA.
void udsUdpPortsFromEnvironment(uint16_t* listenPort, uint16_t* sendPort);

bool udsUdpOpen(struct UDSUdp* udp, uint16_t listenPort, uint16_t sendPort);
void udsUdpClose(struct UDSUdp* udp);
void udsUdpSend(struct UDSUdp* udp, const uint8_t* datagram, size_t size);
// One waiting datagram, or 0 when there is none.
size_t udsUdpReceive(struct UDSUdp* udp, uint8_t* buffer, size_t capacity);

CXX_GUARD_END

#endif
