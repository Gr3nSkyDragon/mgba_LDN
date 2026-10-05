/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-room.h>

#include <string.h>

enum {
	ETHERTYPE_SECURE = 0x876D,
	ETHERTYPE_EAPOL = 0x888E,
	EAPOL_START = 0x0201,
	EAPOL_LOGOFF = 0x0202,
	EAPOL_START_SIZE = 0x30,
	EAPOL_LOGOFF_MIN = 24, // up to the node list
	CONNECTION_CLIENT = 1,
	HOST_NODE = 1,
	BROADCAST_NODE = 0xFFFF,
	MANAGEMENT_CHANNEL = 3,

	BEACON_FIXED = 12, // timestamp, interval, capabilities
	TAG_VENDOR = 221,
	NETWORK_INFO_TYPE = 21,
	NETWORK_INFO_MIN = 0x34, // tag body up to the application data
	NET_COMM_ID = 4,
	NET_NETWORK_ID = 12,
	NET_APPDATA_SIZE = 0x33,
	NET_APPDATA = 0x34,

	RESEND_MS = 1000,
	GIVE_UP_MS = 10000,
	KEEPALIVE_MS = 1100,
};

static const uint8_t kLlc[UDS_LLC_SIZE - 2] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};

static uint16_t _be16(const uint8_t* p) {
	return (uint16_t) ((p[0] << 8) | p[1]);
}

static uint32_t _be32(const uint8_t* p) {
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static void _put16(uint8_t* p, unsigned v) {
	p[0] = v >> 8;
	p[1] = v;
}

size_t udsRoomEncode(uint8_t* out, size_t capacity, const struct UDSRoomPacket* packet) {
	if (capacity < UDS_BRIDGE_HEADER_SIZE + packet->size) {
		return 0;
	}
	memcpy(out, UDS_BRIDGE_MAGIC, 4);
	out[4] = UDS_BRIDGE_VERSION;
	out[5] = packet->type;
	out[6] = packet->channel;
	out[7] = 0;
	memcpy(&out[8], packet->transmitter, 6);
	memcpy(&out[14], packet->destination, 6);
	if (packet->size) {
		memcpy(&out[UDS_BRIDGE_HEADER_SIZE], packet->data, packet->size);
	}
	return UDS_BRIDGE_HEADER_SIZE + packet->size;
}

bool udsRoomDecode(const uint8_t* datagram, size_t size, struct UDSRoomPacket* packet) {
	if (size < UDS_BRIDGE_HEADER_SIZE || memcmp(datagram, UDS_BRIDGE_MAGIC, 4) || datagram[4] != UDS_BRIDGE_VERSION) {
		return false;
	}
	packet->type = datagram[5];
	packet->channel = datagram[6];
	memcpy(packet->transmitter, &datagram[8], 6);
	memcpy(packet->destination, &datagram[14], 6);
	packet->data = &datagram[UDS_BRIDGE_HEADER_SIZE];
	packet->size = size - UDS_BRIDGE_HEADER_SIZE;
	return true;
}

size_t udsBuildAuth(uint8_t out[6], unsigned sequence) {
	// algorithm 0 (open system), sequence, status 0; all little-endian
	out[0] = 0;
	out[1] = 0;
	out[2] = sequence;
	out[3] = 0;
	out[4] = 0;
	out[5] = 0;
	return 6;
}

size_t udsBuildEapolStart(uint8_t* out, size_t capacity, uint16_t associationId, uint64_t friendCodeSeed,
                          const uint16_t name[UDS_NAME_WORDS]) {
	if (capacity < UDS_LLC_SIZE + EAPOL_START_SIZE) {
		return 0;
	}
	memset(out, 0, UDS_LLC_SIZE + EAPOL_START_SIZE);
	memcpy(out, kLlc, sizeof(kLlc));
	_put16(&out[6], ETHERTYPE_EAPOL);
	uint8_t* body = &out[UDS_LLC_SIZE];
	_put16(&body[0], EAPOL_START);
	_put16(&body[2], associationId);
	_put16(&body[4], CONNECTION_CLIENT);
	body[6] = 0x01; // retail hardware sets these; see Azahar's GenerateEAPoLStartFrame
	int i;
	for (i = 0; i < 8; ++i) {
		body[8 + i] = friendCodeSeed >> (56 - 8 * i);
	}
	for (i = 0; i < UDS_NAME_WORDS; ++i) {
		_put16(&body[16 + 2 * i], name[i]);
	}
	// node id (40..41) stays 0: the host assigns it. The last six bytes are a fixed firmware constant.
	static const uint8_t trailer[6] = {0x15, 0x00, 0x04, 0xE9, 0x13, 0x00};
	memcpy(&body[42], trailer, sizeof(trailer));
	return UDS_LLC_SIZE + EAPOL_START_SIZE;
}

size_t udsBuildSecureData(uint8_t* out, size_t capacity, const uint8_t* payload, size_t size, uint8_t channel,
                          uint16_t destination, uint16_t source, uint16_t sequence, bool management) {
	size_t total = UDS_LLC_SIZE + UDS_SECURE_HEADER_SIZE + size;
	if (capacity < total) {
		return 0;
	}
	memcpy(out, kLlc, sizeof(kLlc));
	_put16(&out[6], ETHERTYPE_SECURE);
	uint8_t* h = &out[UDS_LLC_SIZE];
	_put16(&h[0], size + UDS_SECURE_HEADER_SIZE); // protocol size
	_put16(&h[2], 1); // packet count
	_put16(&h[4], size + UDS_SECURE_HEADER_SIZE - 4); // securedata size
	h[6] = management ? 1 : 0;
	h[7] = channel;
	_put16(&h[8], sequence);
	_put16(&h[10], destination);
	_put16(&h[12], source);
	if (size) {
		memcpy(&h[UDS_SECURE_HEADER_SIZE], payload, size);
	}
	return total;
}

void udsRoomInit(struct UDSRoom* room, const uint8_t mac[6], const uint16_t name[UDS_NAME_WORDS], UDSRoomSend send,
                 UDSRoomJoined joined, UDSRoomPia pia, void* context) {
	memset(room, 0, sizeof(*room));
	memcpy(room->mac, mac, 6);
	memcpy(room->name, name, sizeof(room->name));
	room->friendCodeSeed = 0x0000A5A5A5A5A5A5ULL;
	room->wantCommId = UDS_PIA_COMM_ID;
	room->wantCommMask = 0xFFFFFFFFu;
	room->send = send;
	room->joined = joined;
	room->pia = pia;
	room->context = context;
	room->state = UDS_ROOM_SCAN;
}

static void _send(struct UDSRoom* room, uint8_t type, const uint8_t* dest, const uint8_t* data, size_t size) {
	struct UDSRoomPacket packet;
	memset(&packet, 0, sizeof(packet));
	packet.type = type;
	packet.channel = room->host.channel;
	memcpy(packet.transmitter, room->mac, 6);
	memcpy(packet.destination, dest, 6);
	packet.data = data;
	packet.size = size;
	uint8_t datagram[UDS_BRIDGE_MAX_DATAGRAM];
	size_t length = udsRoomEncode(datagram, sizeof(datagram), &packet);
	if (length && room->send) {
		++room->packetsSent;
		room->send(room->context, datagram, length);
	}
}

static void _sendAuth(struct UDSRoom* room, uint32_t nowMs) {
	uint8_t body[6];
	udsBuildAuth(body, 1);
	_send(room, UDS_PACKET_AUTH, room->host.mac, body, sizeof(body));
	room->stepMs = nowMs;
}

static void _sendEapolStart(struct UDSRoom* room, uint32_t nowMs) {
	uint8_t body[UDS_LLC_SIZE + EAPOL_START_SIZE];
	size_t size = udsBuildEapolStart(body, sizeof(body), room->associationId, room->friendCodeSeed, room->name);
	_send(room, UDS_PACKET_DATA, room->host.mac, body, size);
	room->stepMs = nowMs;
}

// Finds the Nintendo network-info tag in a beacon body and fills the host description from it.
bool udsRoomParseBeacon(const uint8_t* data, size_t size, struct UDSRoomHost* host) {
	size_t pos = BEACON_FIXED;
	while (pos + 2 <= size) {
		uint8_t id = data[pos];
		size_t length = data[pos + 1];
		if (pos + 2 + length > size) {
			return false;
		}
		const uint8_t* body = &data[pos + 2];
		if (id == TAG_VENDOR && length >= NETWORK_INFO_MIN && body[3] == NETWORK_INFO_TYPE) {
			host->commId = _be32(&body[NET_COMM_ID]);
			host->networkId = _be32(&body[NET_NETWORK_ID]);
			host->id = body[8];
			size_t appSize = body[NET_APPDATA_SIZE];
			if (NET_APPDATA + appSize > length) {
				return false;
			}
			host->appDataSize = appSize;
			memset(host->appData, 0, sizeof(host->appData));
			memcpy(host->appData, &body[NET_APPDATA], appSize < sizeof(host->appData) ? appSize : sizeof(host->appData));
			return true;
		}
		pos += 2 + length;
	}
	return false;
}

static void _handleSecureData(struct UDSRoom* room, const struct UDSRoomPacket* packet) {
	if (room->state != UDS_ROOM_JOINED || packet->size < UDS_LLC_SIZE + UDS_SECURE_HEADER_SIZE) {
		return;
	}
	const uint8_t* h = &packet->data[UDS_LLC_SIZE];
	size_t protocolSize = _be16(&h[0]);
	if (_be16(&h[2]) != 1 || protocolSize < UDS_SECURE_HEADER_SIZE ||
	    _be16(&h[4]) + 4u != protocolSize || UDS_LLC_SIZE + protocolSize > packet->size) {
		return; // aggregated or malformed; the Azahar host sends neither
	}
	if (h[6] || h[7] != UDS_PIA_CHANNEL) {
		return; // management traffic, or another channel
	}
	uint16_t destination = _be16(&h[10]);
	if (destination != room->host.nodeId && destination != BROADCAST_NODE) {
		return;
	}
	if (room->pia) {
		room->pia(room->context, &h[UDS_SECURE_HEADER_SIZE], protocolSize - UDS_SECURE_HEADER_SIZE);
	}
}

void udsRoomLeave(struct UDSRoom* room, uint32_t nowMs, uint32_t holdMs) {
	if (room->state == UDS_ROOM_SCAN) {
		return;
	}
	static const uint8_t reason[2] = {0x03, 0x00}; // station is leaving
	_send(room, UDS_PACKET_DEAUTH, room->host.mac, reason, sizeof(reason));
	room->state = UDS_ROOM_SCAN;
	room->scanResumeMs = nowMs + holdMs;
}

void udsRoomReceive(struct UDSRoom* room, uint32_t nowMs, const uint8_t* datagram, size_t size) {
	struct UDSRoomPacket packet;
	if (!udsRoomDecode(datagram, size, &packet)) {
		return;
	}
	++room->packetsReceived;

	if (packet.type == UDS_PACKET_BEACON) {
		++room->beaconsSeen;
		if (room->state != UDS_ROOM_SCAN || (room->scanResumeMs && (int32_t) (nowMs - room->scanResumeMs) < 0)) {
			return;
		}
		struct UDSRoomHost host;
		memset(&host, 0, sizeof(host));
		if (!udsRoomParseBeacon(packet.data, packet.size, &host) || (room->wantCommId && (host.commId & room->wantCommMask) != (room->wantCommId & room->wantCommMask))) {
			return;
		}
		memcpy(host.mac, packet.transmitter, 6);
		host.channel = packet.channel;
		room->host = host;
		room->state = UDS_ROOM_AUTH;
		_sendAuth(room, nowMs);
		return;
	}

	// Everything else must come from the host we picked.
	if (room->state == UDS_ROOM_SCAN || memcmp(packet.transmitter, room->host.mac, 6)) {
		return;
	}

	switch (packet.type) {
	case UDS_PACKET_ASSOC_RESPONSE:
		if (room->state == UDS_ROOM_AUTH && packet.size >= 6) {
			unsigned status = packet.data[2] | (packet.data[3] << 8);
			if (status != 0) {
				room->state = UDS_ROOM_SCAN;
				return;
			}
			room->associationId = (packet.data[4] | (packet.data[5] << 8)) & 0x3FFF;
			room->state = UDS_ROOM_EAPOL;
			_sendEapolStart(room, nowMs);
		}
		break;
	case UDS_PACKET_DEAUTH:
		room->state = UDS_ROOM_SCAN;
		break;
	case UDS_PACKET_DATA:
		if (packet.size < UDS_LLC_SIZE + 2) {
			break;
		}
		if (_be16(&packet.data[6]) == ETHERTYPE_EAPOL) {
			const uint8_t* body = &packet.data[UDS_LLC_SIZE];
			if (room->state == UDS_ROOM_EAPOL && packet.size >= UDS_LLC_SIZE + EAPOL_LOGOFF_MIN &&
			    _be16(body) == EAPOL_LOGOFF) {
				room->host.nodeId = _be16(&body[4]);
				room->host.connectedNodes = body[18];
				room->state = UDS_ROOM_JOINED;
				room->keepaliveMs = nowMs;
				if (room->joined) {
					room->joined(room->context, &room->host);
				}
			}
		} else if (_be16(&packet.data[6]) == ETHERTYPE_SECURE) {
			_handleSecureData(room, &packet);
		}
		break;
	default:
		break;
	}
}

void udsRoomPoll(struct UDSRoom* room, uint32_t nowMs) {
	switch (room->state) {
	case UDS_ROOM_AUTH:
	case UDS_ROOM_EAPOL:
		if (nowMs - room->stepMs >= GIVE_UP_MS) {
			room->state = UDS_ROOM_SCAN;
		} else if (nowMs - room->stepMs >= RESEND_MS) {
			if (room->state == UDS_ROOM_AUTH) {
				_sendAuth(room, nowMs);
			} else {
				_sendEapolStart(room, nowMs);
			}
		}
		break;
	case UDS_ROOM_JOINED:
		// A retail client sends a one-byte management packet on channel 3 about once a second; a retail host answers it
		// (and may drop a client that stays silent), so keep doing it even though the Azahar host only consumes it.
		if (nowMs - room->keepaliveMs >= KEEPALIVE_MS) {
			static const uint8_t zero = 0;
			uint8_t frame[UDS_LLC_SIZE + UDS_SECURE_HEADER_SIZE + 1];
			size_t size = udsBuildSecureData(frame, sizeof(frame), &zero, 1, MANAGEMENT_CHANNEL, HOST_NODE,
			                                 room->host.nodeId, room->secureSequence++, true);
			_send(room, UDS_PACKET_DATA, room->host.mac, frame, size);
			room->keepaliveMs = nowMs;
		}
		break;
	default:
		break;
	}
}

bool udsRoomSendPia(struct UDSRoom* room, const uint8_t* frame, size_t size) {
	if (room->state != UDS_ROOM_JOINED) {
		return false;
	}
	uint8_t body[UDS_BRIDGE_MAX_DATAGRAM - UDS_BRIDGE_HEADER_SIZE];
	size_t length = udsBuildSecureData(body, sizeof(body), frame, size, UDS_PIA_CHANNEL, HOST_NODE, room->host.nodeId,
	                                   room->secureSequence++, false);
	if (!length) {
		return false;
	}
	_send(room, UDS_PACKET_DATA, room->host.mac, body, length);
	return true;
}
