/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/rfu-udp.h>

#include <mgba-util/socket.h>

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <mswsock.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
typedef int RFUAddrLen;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
typedef socklen_t RFUAddrLen;
#endif

/*
 * Datagram layout: [0] 0x52  [1] type  [2..3] source id  [4..5] destination id (0 = anyone)  [6..] payload.
 * Ids are 16-bit adapter ids: the host's is the one it advertises, a client picks its own when it connects.
 */
enum {
	MSG_MAGIC = 0x52,
	MSG_HEADER = 6,
	MSG_MAX = MSG_HEADER + 1 + RFU_BROADCAST_WORDS * 4 + RFU_PACKET_MAX,

	MSG_BEACON = 1, // payload: next free slot (0xFF = full), then the 24 bytes of broadcast data
	MSG_CONNECT = 2,
	MSG_ACCEPT = 3, // payload: client slot
	MSG_REFUSE = 4,
	MSG_DATA = 5,
	MSG_DISCONNECT = 6,
	MSG_KEEPALIVE = 7,

	PORT_BASE = 45600,
	PORT_COUNT = 8,

	BEACON_FRAMES = 8,
	KEEPALIVE_FRAMES = 10,
	CONNECT_RETRY_FRAMES = 6,
	CONNECT_GIVE_UP_FRAMES = 90,
	SILENCE_FRAMES = 240,
	MAX_HOSTS = 16,
};

struct Peer {
	uint16_t id;
	struct sockaddr_in addr;
	unsigned silent;
};

struct GBASIORFUUDP {
	struct GBASIORFUBackend d;
	struct GBASIORFU* rfu;
	Socket sock;
	unsigned basePort;
	unsigned ownPort;
	uint16_t ownId; // the id this adapter uses as a client

	// Host role
	bool hosting;
	bool advertising;
	uint16_t hostId;
	uint32_t broadcast[RFU_BROADCAST_WORDS];
	unsigned beaconCountdown;
	unsigned keepaliveCountdown;
	struct Peer clients[RFU_MAX_CLIENTS];
	struct Peer pending[RFU_MAX_CLIENTS]; // connect requests waiting for the adapter's decision

	// Client role
	bool searching;
	struct Peer hosts[MAX_HOSTS]; // every host heard, so a later connect knows where to send
	bool connecting;
	unsigned connectFrames;
	struct Peer target;
	bool connected;
	struct Peer server;
};

static bool _sameAddr(const struct sockaddr_in* a, const struct sockaddr_in* b) {
	return a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
}

static void _send(struct GBASIORFUUDP* udp, const struct sockaddr_in* to, uint8_t type, uint16_t src, uint16_t dst, const void* payload, size_t length) {
	uint8_t message[MSG_MAX];
	if (length > MSG_MAX - MSG_HEADER) {
		return;
	}
	if (type == MSG_DATA) {
		GBASIORFUTrace(udp->rfu, "UDP    tx data %zu bytes: %02X%02X%02X%02X%02X%02X", length, length > 0 ? ((const uint8_t*) payload)[0] : 0,
		               length > 1 ? ((const uint8_t*) payload)[1] : 0, length > 2 ? ((const uint8_t*) payload)[2] : 0,
		               length > 3 ? ((const uint8_t*) payload)[3] : 0, length > 4 ? ((const uint8_t*) payload)[4] : 0,
		               length > 5 ? ((const uint8_t*) payload)[5] : 0);
	}
	if (type == MSG_DISCONNECT) {
		GBASIORFUTrace(udp->rfu, "UDP    sending DISCONNECT src=%04X dst=%04X", src, dst);
	}
	message[0] = MSG_MAGIC;
	message[1] = type;
	message[2] = src;
	message[3] = src >> 8;
	message[4] = dst;
	message[5] = dst >> 8;
	if (length) {
		memcpy(&message[MSG_HEADER], payload, length);
	}
	sendto(udp->sock, (const char*) message, MSG_HEADER + length, 0, (const struct sockaddr*) to, sizeof(*to));
}

static struct sockaddr_in _loopback(unsigned port) {
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return addr;
}

// Everyone else on this machine: the same message to every port in the range except our own.
static void _sendToAll(struct GBASIORFUUDP* udp, uint8_t type, uint16_t src, const void* payload, size_t length) {
	for (unsigned i = 0; i < PORT_COUNT; ++i) {
		if (udp->basePort + i == udp->ownPort) {
			continue;
		}
		struct sockaddr_in addr = _loopback(udp->basePort + i);
		_send(udp, &addr, type, src, 0, payload, length);
	}
}

static void _clearRoles(struct GBASIORFUUDP* udp, bool notifyPeers) {
	unsigned i;
	if (notifyPeers) {
		for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
			if (udp->clients[i].id) {
				_send(udp, &udp->clients[i].addr, MSG_DISCONNECT, udp->hostId, udp->clients[i].id, NULL, 0);
			}
		}
		if (udp->connected || udp->connecting) {
			const struct Peer* peer = udp->connected ? &udp->server : &udp->target;
			_send(udp, &peer->addr, MSG_DISCONNECT, udp->ownId, peer->id, NULL, 0);
		}
	}
	udp->hosting = false;
	udp->advertising = false;
	udp->searching = false;
	udp->connecting = false;
	udp->connected = false;
	memset(udp->clients, 0, sizeof(udp->clients));
	memset(udp->pending, 0, sizeof(udp->pending));
	memset(udp->hosts, 0, sizeof(udp->hosts));
}

static bool _init(struct GBASIORFUBackend* backend, struct GBASIORFU* rfu) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	udp->rfu = rfu;
	SocketSubsystemInit();

	const char* base = getenv("MGBA_RFU_UDP_PORT");
	udp->basePort = base && atoi(base) > 0 ? atoi(base) : PORT_BASE;

	udp->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (SOCKET_FAILED(udp->sock)) {
		GBASIORFUTrace(rfu, "UDP    socket() failed (%d)", SocketError());
		return true; // the adapter still works, nobody can be reached
	}
	SocketSetBlocking(udp->sock, false);
#ifdef _WIN32
	// Sending to a port nobody listens on must not make later receives fail.
	DWORD off = FALSE, bytes = 0;
	WSAIoctl(udp->sock, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &bytes, NULL, NULL);
#endif

	udp->ownPort = 0;
	for (unsigned i = 0; i < PORT_COUNT; ++i) {
		struct sockaddr_in addr = _loopback(udp->basePort + i);
		if (bind(udp->sock, (struct sockaddr*) &addr, sizeof(addr)) == 0) {
			udp->ownPort = udp->basePort + i;
			break;
		}
	}
	if (!udp->ownPort) {
		GBASIORFUTrace(rfu, "UDP    no free port in %u..%u", udp->basePort, udp->basePort + PORT_COUNT - 1);
		SocketClose(udp->sock);
		udp->sock = INVALID_SOCKET;
		return true;
	}
	udp->ownId = (uint16_t) (0x4000 + (udp->ownPort - udp->basePort) * 0x111 + (rand() & 0xFF) * 0x10 + 1);
	GBASIORFUTrace(rfu, "UDP    listening on 127.0.0.1:%u (client id %04X)", udp->ownPort, udp->ownId);
	return true;
}

static void _deinit(struct GBASIORFUBackend* backend) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	if (!SOCKET_FAILED(udp->sock)) {
		_clearRoles(udp, true);
		SocketClose(udp->sock);
		udp->sock = INVALID_SOCKET;
	}
}

static void _reset(struct GBASIORFUBackend* backend) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	if (!SOCKET_FAILED(udp->sock)) {
		_clearRoles(udp, true);
	}
}

static void _setBroadcast(struct GBASIORFUBackend* backend, const uint32_t data[RFU_BROADCAST_WORDS]) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	memcpy(udp->broadcast, data, sizeof(udp->broadcast));
}

static void _hostStart(struct GBASIORFUBackend* backend, uint16_t deviceId) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	udp->hosting = true;
	udp->advertising = true;
	udp->hostId = deviceId;
	udp->beaconCountdown = 0;
}

static void _hostStop(struct GBASIORFUBackend* backend) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	udp->advertising = false;
}

static void _searchStart(struct GBASIORFUBackend* backend) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	udp->searching = true;
}

static void _searchStop(struct GBASIORFUBackend* backend) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	udp->searching = false;
}

static void _connect(struct GBASIORFUBackend* backend, uint16_t deviceId) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	for (unsigned i = 0; i < MAX_HOSTS; ++i) {
		if (udp->hosts[i].id == deviceId) {
			udp->target = udp->hosts[i];
			udp->connecting = true;
			udp->connectFrames = 0;
			_send(udp, &udp->target.addr, MSG_CONNECT, udp->ownId, deviceId, NULL, 0);
			GBASIORFUTrace(udp->rfu, "UDP    connect request to host %04X", deviceId);
			return;
		}
	}
	GBASIORFUTrace(udp->rfu, "UDP    host %04X is not known", deviceId);
	GBASIORFUConnectResult(udp->rfu, false, deviceId, 0);
}

// The adapter's answer to a client that asked to join us.
static void _connectReply(struct GBASIORFUBackend* backend, uint16_t clientId, bool accepted, unsigned slot) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	for (unsigned i = 0; i < RFU_MAX_CLIENTS; ++i) {
		if (udp->pending[i].id != clientId) {
			continue;
		}
		struct Peer peer = udp->pending[i];
		memset(&udp->pending[i], 0, sizeof(udp->pending[i]));
		if (accepted && slot < RFU_MAX_CLIENTS) {
			peer.silent = 0;
			udp->clients[slot] = peer;
			uint8_t payload = slot;
			_send(udp, &peer.addr, MSG_ACCEPT, udp->hostId, clientId, &payload, 1);
		} else {
			_send(udp, &peer.addr, MSG_REFUSE, udp->hostId, clientId, NULL, 0);
		}
		return;
	}
}

static void _disconnect(struct GBASIORFUBackend* backend, unsigned slotMask) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	if (udp->hosting && slotMask) {
		for (unsigned i = 0; i < RFU_MAX_CLIENTS; ++i) {
			if ((slotMask & (1u << i)) && udp->clients[i].id) {
				_send(udp, &udp->clients[i].addr, MSG_DISCONNECT, udp->hostId, udp->clients[i].id, NULL, 0);
				memset(&udp->clients[i], 0, sizeof(udp->clients[i]));
			}
		}
	} else if (!slotMask && (udp->connected || udp->connecting)) {
		const struct Peer* peer = udp->connected ? &udp->server : &udp->target;
		_send(udp, &peer->addr, MSG_DISCONNECT, udp->ownId, peer->id, NULL, 0);
		udp->connected = false;
		udp->connecting = false;
	}
}

static void _sendData(struct GBASIORFUBackend* backend, const uint8_t* data, size_t length) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	if (SOCKET_FAILED(udp->sock)) {
		return;
	}
	if (udp->connected) {
		_send(udp, &udp->server.addr, MSG_DATA, udp->ownId, udp->server.id, data, length);
	} else if (udp->hosting) {
		for (unsigned i = 0; i < RFU_MAX_CLIENTS; ++i) {
			if (udp->clients[i].id) {
				_send(udp, &udp->clients[i].addr, MSG_DATA, udp->hostId, udp->clients[i].id, data, length);
			}
		}
	}
}

static void _handle(struct GBASIORFUUDP* udp, const uint8_t* message, size_t size, const struct sockaddr_in* from) {
	unsigned i;
	if (size < MSG_HEADER || message[0] != MSG_MAGIC) {
		return;
	}
	uint8_t type = message[1];
	uint16_t src = message[2] | (message[3] << 8);
	uint16_t dst = message[4] | (message[5] << 8);
	const uint8_t* payload = &message[MSG_HEADER];
	size_t length = size - MSG_HEADER;

	switch (type) {
	case MSG_BEACON: {
		if (length != 1 + RFU_BROADCAST_WORDS * 4 || !src) {
			break;
		}
		struct Peer* slot = NULL;
		for (i = 0; i < MAX_HOSTS; ++i) {
			if (udp->hosts[i].id == src) {
				slot = &udp->hosts[i];
				break;
			}
			if (!slot && !udp->hosts[i].id) {
				slot = &udp->hosts[i];
			}
		}
		if (slot) {
			slot->id = src;
			slot->addr = *from;
		}
		if (udp->searching && !(udp->hosting && udp->hostId == src)) {
			uint32_t words[RFU_BROADCAST_WORDS];
			for (i = 0; i < RFU_BROADCAST_WORDS; ++i) {
				const uint8_t* b = &payload[1 + i * 4];
				words[i] = b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t) b[3] << 24);
			}
			GBASIORFUBroadcastReceived(udp->rfu, src, payload[0], words);
		}
		break;
	}
	case MSG_CONNECT:
		if (!udp->hosting || !src || dst != udp->hostId) {
			_send(udp, from, MSG_REFUSE, udp->hostId, src, NULL, 0);
			break;
		}
		// A repeat of a request we already accepted just gets the same answer again.
		for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
			if (udp->clients[i].id == src) {
				uint8_t slot = i;
				udp->clients[i].silent = 0;
				_send(udp, from, MSG_ACCEPT, udp->hostId, src, &slot, 1);
				return;
			}
		}
		for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
			if (udp->pending[i].id == src) {
				return; // already waiting for the adapter's decision
			}
		}
		for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
			if (!udp->pending[i].id) {
				udp->pending[i].id = src;
				udp->pending[i].addr = *from;
				GBASIORFUConnectRequested(udp->rfu, src);
				return;
			}
		}
		_send(udp, from, MSG_REFUSE, udp->hostId, src, NULL, 0);
		break;
	case MSG_ACCEPT:
		if (udp->connecting && src == udp->target.id && dst == udp->ownId && length >= 1) {
			udp->connecting = false;
			udp->connected = true;
			udp->server = udp->target;
			udp->server.addr = *from;
			udp->server.silent = 0;
			GBASIORFUConnectResult(udp->rfu, true, udp->ownId, payload[0]);
		}
		break;
	case MSG_REFUSE:
		if (udp->connecting && src == udp->target.id) {
			udp->connecting = false;
			GBASIORFUConnectResult(udp->rfu, false, src, 0);
		}
		break;
	case MSG_DATA:
	case MSG_KEEPALIVE:
		if (type == MSG_DATA) {
			GBASIORFUTrace(udp->rfu, "UDP    rx data %zu bytes: %02X%02X%02X%02X%02X%02X", length, length > 0 ? payload[0] : 0,
			               length > 1 ? payload[1] : 0, length > 2 ? payload[2] : 0, length > 3 ? payload[3] : 0,
			               length > 4 ? payload[4] : 0, length > 5 ? payload[5] : 0);
		}
		if (udp->connected && src == udp->server.id && _sameAddr(from, &udp->server.addr)) {
			udp->server.silent = 0;
			GBASIORFUDataReceived(udp->rfu, 0, payload, type == MSG_DATA ? length : 0);
		} else if (udp->hosting) {
			for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
				if (udp->clients[i].id == src && _sameAddr(from, &udp->clients[i].addr)) {
					udp->clients[i].silent = 0;
					GBASIORFUDataReceived(udp->rfu, i, payload, type == MSG_DATA ? length : 0);
					break;
				}
			}
		}
		break;
	case MSG_DISCONNECT:
		GBASIORFUTrace(udp->rfu, "UDP    received DISCONNECT src=%04X dst=%04X from port %u", src, dst, ntohs(from->sin_port));
		if (udp->connected && src == udp->server.id) {
			udp->connected = false;
			GBASIORFUDisconnected(udp->rfu, -1);
		} else if (udp->connecting && src == udp->target.id) {
			udp->connecting = false;
			GBASIORFUConnectResult(udp->rfu, false, src, 0);
		} else if (udp->hosting) {
			for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
				if (udp->clients[i].id == src) {
					memset(&udp->clients[i], 0, sizeof(udp->clients[i]));
					GBASIORFUDisconnected(udp->rfu, i);
					break;
				}
			}
		}
		break;
	}
}

// Called often (every adapter tick): take in whatever has arrived.
static void _poll(struct GBASIORFUBackend* backend) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	if (SOCKET_FAILED(udp->sock)) {
		return;
	}
	for (unsigned n = 0; n < 64; ++n) {
		uint8_t message[MSG_MAX + 16];
		struct sockaddr_in from;
		RFUAddrLen fromLength = sizeof(from);
		int size = recvfrom(udp->sock, (char*) message, sizeof(message), 0, (struct sockaddr*) &from, &fromLength);
		if (size < 0) {
			break;
		}
		_handle(udp, message, size, &from);
	}
}

// Once per emulated frame: timers.
static void _frame(struct GBASIORFUBackend* backend) {
	struct GBASIORFUUDP* udp = (struct GBASIORFUUDP*) backend;
	unsigned i;
	if (SOCKET_FAILED(udp->sock)) {
		return;
	}

	if (udp->hosting && !udp->advertising) {
		// The host closed and has nobody left: it is no longer a host at all.
		bool any = false;
		for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
			any = any || udp->clients[i].id;
		}
		udp->hosting = any;
	}

	if (udp->hosting && udp->advertising && !udp->beaconCountdown--) {
		uint8_t payload[1 + RFU_BROADCAST_WORDS * 4];
		payload[0] = GBASIORFUHostNextSlot(udp->rfu);
		for (i = 0; i < RFU_BROADCAST_WORDS * 4; ++i) {
			payload[1 + i] = udp->broadcast[i / 4] >> (8 * (i & 3));
		}
		_sendToAll(udp, MSG_BEACON, udp->hostId, payload, sizeof(payload));
		udp->beaconCountdown = BEACON_FRAMES - 1;
	}

	if (!udp->keepaliveCountdown--) {
		udp->keepaliveCountdown = KEEPALIVE_FRAMES - 1;
		for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
			if (udp->clients[i].id) {
				_send(udp, &udp->clients[i].addr, MSG_KEEPALIVE, udp->hostId, udp->clients[i].id, NULL, 0);
			}
		}
		if (udp->connected) {
			_send(udp, &udp->server.addr, MSG_KEEPALIVE, udp->ownId, udp->server.id, NULL, 0);
		}
	}

	for (i = 0; i < RFU_MAX_CLIENTS; ++i) {
		if (udp->clients[i].id && ++udp->clients[i].silent > SILENCE_FRAMES) {
			memset(&udp->clients[i], 0, sizeof(udp->clients[i]));
			GBASIORFUDisconnected(udp->rfu, i);
		}
	}
	if (udp->connected && ++udp->server.silent > SILENCE_FRAMES) {
		udp->connected = false;
		GBASIORFUTrace(udp->rfu, "UDP    host went silent");
		GBASIORFUDisconnected(udp->rfu, -1);
	}

	if (udp->connecting) {
		++udp->connectFrames;
		if (udp->connectFrames >= CONNECT_GIVE_UP_FRAMES) {
			udp->connecting = false;
			GBASIORFUTrace(udp->rfu, "UDP    host %04X did not answer", udp->target.id);
			GBASIORFUConnectResult(udp->rfu, false, udp->target.id, 0);
		} else if (!(udp->connectFrames % CONNECT_RETRY_FRAMES)) {
			_send(udp, &udp->target.addr, MSG_CONNECT, udp->ownId, udp->target.id, NULL, 0);
		}
	}
}

struct GBASIORFUBackend* GBASIORFUUDPCreate(void) {
	struct GBASIORFUUDP* udp = calloc(1, sizeof(*udp));
	if (!udp) {
		return NULL;
	}
	udp->sock = INVALID_SOCKET;
	udp->d.init = _init;
	udp->d.deinit = _deinit;
	udp->d.reset = _reset;
	udp->d.frame = _frame;
	udp->d.poll = _poll;
	udp->d.setBroadcast = _setBroadcast;
	udp->d.hostStart = _hostStart;
	udp->d.hostStop = _hostStop;
	udp->d.searchStart = _searchStart;
	udp->d.searchStop = _searchStop;
	udp->d.connect = _connect;
	udp->d.connectReply = _connectReply;
	udp->d.disconnect = _disconnect;
	udp->d.sendData = _sendData;
	return &udp->d;
}

void GBASIORFUUDPDestroy(struct GBASIORFUBackend* backend) {
	free(backend);
}
