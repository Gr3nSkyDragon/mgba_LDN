/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_ROOM_H
#define GB_SIO_UDS_ROOM_H

#include <mgba-util/common.h>

CXX_GUARD_START

/*
 * The joiner's link layer for the Azahar test bridge (doc/uds-wrapper-plan.md, layer L2 "Azahar bridge").
 *
 * Azahar's nwm::UDS passes around plaintext "WifiPackets": a type, a channel, two MAC addresses and a body. With
 * AZAHAR_UDS_BRIDGE set, Azahar sends each one to this program as one UDP datagram and accepts the same back, so no radio,
 * 802.11 header or CCMP is involved. This module speaks that representation as the joiner:
 *
 *   beacon (host)  ->  authentication SEQ1  ->  authentication SEQ2 + association response (host)
 *                  ->  EAPoL start  ->  EAPoL "logoff" (host: the assigned node id and the node list)
 *                  ->  SecureData frames on channel 243 carrying Pia, both ways
 *
 * The real air needs the same sequence with true 802.11 frames and CCMP (milestone M6); the beacon parsing, the EAPoL bodies
 * and the SecureData header below are the real ones, only the management frames and the encryption differ.
 *
 * Like the Pia session this module owns no socket and no clock: datagrams go out through a callback and every entry point is
 * told the time.
 */

#define UDS_BRIDGE_HEADER_SIZE 20
#define UDS_BRIDGE_MAX_DATAGRAM 4096
#define UDS_BRIDGE_MAGIC "UDSB"
#define UDS_BRIDGE_VERSION 1
#define UDS_BRIDGE_DEFAULT_PORT 45710 // Azahar listens here and sends to +1; this program does the reverse
#define UDS_NAME_WORDS 10
#define UDS_PIA_CHANNEL 243
#define UDS_SECURE_HEADER_SIZE 14
#define UDS_LLC_SIZE 8
#define UDS_PIA_COMM_ID 0x00171010 // WLAN communication id of the Game Boy Virtual Console Pokemon titles

enum UDSPacketType {
	UDS_PACKET_BEACON = 0,
	UDS_PACKET_DATA = 1,
	UDS_PACKET_AUTH = 2,
	UDS_PACKET_ASSOC_RESPONSE = 3,
	UDS_PACKET_DEAUTH = 4,
	UDS_PACKET_NODE_MAP = 5,
};

struct UDSRoomPacket {
	uint8_t type;
	uint8_t channel;
	uint8_t transmitter[6];
	uint8_t destination[6];
	const uint8_t* data; // points into the datagram when decoded, into the caller's buffer when encoding
	size_t size;
};

size_t udsRoomEncode(uint8_t* out, size_t capacity, const struct UDSRoomPacket* packet);
bool udsRoomDecode(const uint8_t* datagram, size_t size, struct UDSRoomPacket* packet);

enum UDSRoomState {
	UDS_ROOM_SCAN, // waiting for a beacon of the right network
	UDS_ROOM_AUTH, // authentication SEQ1 sent
	UDS_ROOM_EAPOL, // associated, EAPoL start sent
	UDS_ROOM_JOINED, // the host assigned a node id; Pia may flow
};

// What the beacon and the join taught us about the host.
struct UDSRoomHost {
	uint8_t mac[6];
	uint8_t channel;
	uint32_t commId;
	uint32_t networkId;
	uint8_t id; // the network info's id byte (part of the data key's counter)
	uint8_t appData[16]; // the first 16 bytes of the beacon's application data
	uint8_t appDataSize;
	uint16_t nodeId; // ours, assigned by the host
	uint8_t connectedNodes;
};

typedef void (*UDSRoomSend)(void* context, const uint8_t* datagram, size_t size);
typedef void (*UDSRoomJoined)(void* context, const struct UDSRoomHost* host);
typedef void (*UDSRoomPia)(void* context, const uint8_t* payload, size_t size);

struct UDSRoom {
	uint8_t mac[6]; // our address
	uint16_t name[UDS_NAME_WORDS]; // UTF-16 player name
	uint64_t friendCodeSeed;
	uint32_t wantCommId; // 0 accepts any network

	UDSRoomSend send;
	UDSRoomJoined joined;
	UDSRoomPia pia;
	void* context;

	enum UDSRoomState state;
	struct UDSRoomHost host;
	uint16_t associationId;
	uint16_t secureSequence;
	uint32_t stepMs; // when the current state was entered or its message last sent
	uint32_t keepaliveMs;
	unsigned beaconsSeen;
	unsigned packetsSent;
	unsigned packetsReceived;
};

void udsRoomInit(struct UDSRoom* room, const uint8_t mac[6], const uint16_t name[UDS_NAME_WORDS], UDSRoomSend send,
                 UDSRoomJoined joined, UDSRoomPia pia, void* context);
void udsRoomReceive(struct UDSRoom* room, uint32_t nowMs, const uint8_t* datagram, size_t size);
void udsRoomPoll(struct UDSRoom* room, uint32_t nowMs);

// Wraps one Pia frame (a UDS channel-243 payload) in a SecureData frame to the host. False until the join is complete.
bool udsRoomSendPia(struct UDSRoom* room, const uint8_t* frame, size_t size);

// Finds the Nintendo network-info tag in a beacon body (the 12 fixed bytes, then tagged parameters; for an 802.11 beacon frame that is
// the part after the 24-byte header) and fills the comm id, network id and application data of `host`. False when there is none.
bool udsRoomParseBeacon(const uint8_t* body, size_t size, struct UDSRoomHost* host);

// Pure helpers, exposed for the test.
size_t udsBuildAuth(uint8_t out[6], unsigned sequence);
size_t udsBuildEapolStart(uint8_t* out, size_t capacity, uint16_t associationId, uint64_t friendCodeSeed,
                          const uint16_t name[UDS_NAME_WORDS]);
size_t udsBuildSecureData(uint8_t* out, size_t capacity, const uint8_t* payload, size_t size, uint8_t channel,
                          uint16_t destination, uint16_t source, uint16_t sequence, bool management);

CXX_GUARD_END

#endif
