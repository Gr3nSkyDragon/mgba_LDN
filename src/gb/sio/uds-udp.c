/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-udp.h>
#include <mgba/internal/gb/sio/uds-room.h>

#include <mgba-util/socket.h>

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <mswsock.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
typedef int UDSAddrLen;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
typedef socklen_t UDSAddrLen;
#endif

static struct sockaddr_in _loopback(unsigned port) {
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return addr;
}

void udsUdpPortsFromEnvironment(uint16_t* listenPort, uint16_t* sendPort) {
	unsigned base = UDS_BRIDGE_DEFAULT_PORT;
	const char* value = getenv("AZAHAR_UDS_BRIDGE");
	if (value && atoi(value) > 1 && atoi(value) < 65535) {
		base = atoi(value);
	}
	*listenPort = base + 1;
	*sendPort = base;
}

bool udsUdpOpen(struct UDSUdp* udp, uint16_t listenPort, uint16_t sendPort) {
	memset(udp, 0, sizeof(*udp));
	SocketSubsystemInit();
	Socket sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (SOCKET_FAILED(sock)) {
		return false;
	}
	SocketSetBlocking(sock, false);
#ifdef _WIN32
	// Sending to a port nobody listens on must not make later receives fail.
	DWORD off = FALSE, bytes = 0;
	WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &bytes, NULL, NULL);
#endif
	struct sockaddr_in addr = _loopback(listenPort);
	if (bind(sock, (struct sockaddr*) &addr, sizeof(addr)) != 0) {
		SocketClose(sock);
		return false;
	}
	udp->sock = (intptr_t) sock;
	udp->listenPort = listenPort;
	udp->sendPort = sendPort;
	udp->open = true;
	return true;
}

void udsUdpClose(struct UDSUdp* udp) {
	if (udp->open) {
		SocketClose((Socket) udp->sock);
	}
	udp->open = false;
}

void udsUdpSend(struct UDSUdp* udp, const uint8_t* datagram, size_t size) {
	if (!udp->open) {
		return;
	}
	struct sockaddr_in to = _loopback(udp->sendPort);
	sendto((Socket) udp->sock, (const char*) datagram, size, 0, (const struct sockaddr*) &to, sizeof(to));
}

size_t udsUdpReceive(struct UDSUdp* udp, uint8_t* buffer, size_t capacity) {
	if (!udp->open) {
		return 0;
	}
	struct sockaddr_in from;
	UDSAddrLen fromLength = sizeof(from);
	int size = recvfrom((Socket) udp->sock, (char*) buffer, capacity, 0, (struct sockaddr*) &from, &fromLength);
	return size > 0 ? (size_t) size : 0;
}
