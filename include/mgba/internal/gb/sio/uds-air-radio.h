/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_AIR_RADIO_H
#define GB_SIO_UDS_AIR_RADIO_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gb/sio/uds-ccmp.h>
#include <mgba/internal/gb/sio/uds-esp32.h>
#include <mgba/internal/gb/sio/uds-room.h>

/*
 * Stage 2, milestone R3 (doc/uds-wrapper-plan.md): the real air for the joiner. It sits where the UDP bridge sits: uds-room.c hands it
 * the packets it would send to Azahar, and it hands back the packets it received, in the same datagram form (uds-room.h). Between the
 * two it does what the bridge does not need to:
 *
 *   room packet                      802.11 on the air (through the ESP32 board)
 *   beacon (host)             <--    beacon with the Nintendo vendor element, scanned on channels 1, 6 and 11
 *   authentication SEQ1       -->    open-system authentication frame to the host
 *   authentication SEQ2       <--    the host's reply; this layer then sends the association request
 *   association response      <--    the host's reply
 *   data (EAPoL start, Pia)   -->    protected data frame, to the host, ToDS, CCMP with the network's data key
 *   data (EAPoL reply, Pia)   <--    protected data frame from the host, decrypted
 *
 * The data key is derived per network from the 3DS UDS data key (slot 0x2D, uds-keyfile.h), the passphrase `TRL_NETWORK\0` and the
 * beacon's comm id, network id and id, and the host's MAC (uds-ccmp.h). The board is started with its decoy hardware MAC, as Azahar
 * does, so frames for our address reach the capture path; the hardware then does not acknowledge them and the host retransmits, which
 * is why received frames are replay-checked by packet number.
 *
 * Single threaded: udsAirRadioPoll is called from the same loop as udsRoomPoll.
 */

#define UDS_AIR_MAX_HOSTS 8

typedef void (*UDSAirDeliver)(void* context, const uint8_t* datagram, size_t size);

struct UDSAirHost {
	bool valid;
	uint8_t mac[6];
	uint8_t channel;
	uint32_t commId;
	uint32_t networkId;
	uint8_t id;
};

enum UDSAirState {
	UDS_AIR_CLOSED,
	UDS_AIR_BOOTING, // the port is open; Hello is sent until the board answers (it resets when the port opens)
	UDS_AIR_READY, // the radio is started: scanning, joining, exchanging frames
	UDS_AIR_FAILED, // the board did not answer, or the port failed; `error` says which
};

struct UDSAirRadio {
	struct UDSEsp32 esp;
	bool open;
	enum UDSAirState state;
	uint32_t bootMs;
	uint32_t lastHelloMs;
	char error[160];
	uint8_t mac[6];
	uint8_t slotKey[16];
	UDSAirDeliver deliver;
	void* context;

	struct UDSAirHost hosts[UDS_AIR_MAX_HOSTS];

	// the host we are joining (chosen when the room first sends it a frame)
	bool haveHost;
	struct UDSAirHost host;
	uint8_t dataKey[16];
	bool assocSent;
	uint64_t txPacketNumber;
	uint16_t txSequence;
	uint64_t lastRxPacketNumber;
	bool haveRxPacketNumber;
	uint32_t lastHostFrameMs;

	uint32_t nowMs; // the time of the poll in progress (a received frame is handled inside it)

	// channel hopping while no host is chosen
	uint8_t channel;
	unsigned hopIndex;
	uint32_t lastHopMs;

	// counters, for the status line and the probe
	unsigned beaconsSeen;
	unsigned framesSent;
	unsigned framesReceived; // delivered to the room
	unsigned droppedNoKey;
	unsigned droppedDecrypt;
	unsigned droppedReplay;
	unsigned droppedOther;
	unsigned txFailed;
};

// Reads the key file and opens the board's port (portName NULL or empty: find it). Does not wait: the board needs a few seconds to boot,
// and udsAirRadioPoll finishes the start (Hello, then Start) without blocking; until `state` is UDS_AIR_READY frames are not sent.
// Returns false with a reason in `error` when the key file or the port is the problem.
bool udsAirRadioOpen(struct UDSAirRadio* radio, const char* portName, const char* keyPath, const uint8_t mac[6], UDSAirDeliver deliver,
                     void* context, char* error, size_t errorSize);
void udsAirRadioClose(struct UDSAirRadio* radio);
bool udsAirRadioReady(const struct UDSAirRadio* radio);
void udsAirRadioPoll(struct UDSAirRadio* radio, uint32_t nowMs);
// A datagram from the room (uds-room.h format).
void udsAirRadioSend(struct UDSAirRadio* radio, const uint8_t* datagram, size_t size);

CXX_GUARD_END

#endif
