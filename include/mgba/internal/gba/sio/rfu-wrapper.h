/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_RFU_WRAPPER_H
#define GBA_SIO_RFU_WRAPPER_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/timing.h>
#include <mgba/gba/interface.h>

/*
 * RFU Cable Wrapper: lets a game that only knows the link cable (Ruby/Sapphire) talk to a partner that is not on the
 * cable at all. The game sees a real cable link with a second player; the second player is a virtual GBA that this
 * driver plays, and whatever the virtual player says or hears is handed to a "peer" (in the end, the wireless side:
 * an FRLG/Emerald wireless partner).
 *
 * Cable side (this file): the game is player 1 (a slave), the driver is player 0 (the master) and owns the clock. It
 * runs the multiplayer handshake (8FFF/B9A0) and then the game link layer's packet stream: every packet is one
 * transfer that carries a checksum, then eight 16-bit words (a command and seven arguments), each transfer moving one
 * word from each player. Commands are broadcast: the game sees its own words come back, exactly like the cable.
 * The word values and packet layout are taken from pret/pokeruby and pokeemerald link.c and verified against a real
 * cable capture (rfu-wrapper-trace.log of two windows in mGBA's own multiplayer).
 *
 * Peer side: the driver hands the peer each complete game command and asks it for the virtual player's next one.
 * Nothing wireless is attached yet; the built-in stub peer behaves like a minimal FireRed on the other end of the
 * cable (link type, player data exchange) and is what the Local/Broadcast/ESP32 connections currently all use.
 */

enum {
	RFU_WRAPPER_CMD_LENGTH = 8,
};

struct GBASIORFUWrapper;

struct GBASIORFUWrapperPeer {
	void* context;
	// The game (cable player 1) finished sending a packet with a non-zero command.
	void (*gameCommand)(void* context, const uint16_t command[RFU_WRAPPER_CMD_LENGTH]);
	// Fill the virtual player's next command. Return false (or leave it zero) to send nothing this packet.
	bool (*nextCommand)(void* context, uint16_t command[RFU_WRAPPER_CMD_LENGTH]);
	// The cable link was (re)started from scratch (the game opened it, or closed it and opened it again).
	void (*reset)(void* context);
	// Once per emulated frame, whatever the cable is doing (the wireless side keeps running while the game's link is
	// closed). Optional.
	void (*frame)(void* context);
};

struct GBASIORFUWrapperStub {
	int stage;
	int announceDelay;
	unsigned rxSize;
	unsigned rxPos;
	uint8_t rxBlock[64];
	uint32_t linkType;
	uint16_t pendingBarrier;
	bool sending;
	int delay;
	unsigned pos;
	unsigned txSize;
	uint8_t block[256];
};

struct GBASIORFUWrapper {
	struct GBASIODriver d;
	struct mTimingEvent tick;
	struct mTimingEvent frameTick;
	void* air; // the wireless side (rfu-wrapper-air.c), when the connection has one
	void (*airDestroy)(void* air);
	const char* connection;
	struct GBASIORFUWrapperPeer peer;
	struct GBASIORFUWrapperStub stub;

	int state;
	int handshakeReplies;
	int slot; // 0: the checksum transfer, 1-8: the command words
	uint16_t txCommand[RFU_WRAPPER_CMD_LENGTH];
	uint16_t rxCommand[RFU_WRAPPER_CMD_LENGTH];
	uint16_t sum;
	uint16_t sumToSend;
	uint16_t masterWord;
	uint16_t slaveWord;
	uint32_t frameStart;
	unsigned packets;
};

// connection is only used in the trace for now ("local", "broadcast", "esp32": all stubs).
void GBASIORFUWrapperCreate(struct GBASIORFUWrapper* wrapper, const char* connection);
void GBASIORFUWrapperDestroy(struct GBASIORFUWrapper* wrapper);

// Replace the built-in stub peer.
void GBASIORFUWrapperSetPeer(struct GBASIORFUWrapper* wrapper, const struct GBASIORFUWrapperPeer* peer);

CXX_GUARD_END

#endif
