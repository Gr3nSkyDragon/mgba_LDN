/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/rfu-wrapper.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/internal/gba/video.h>

// Word values from pret/pokeruby and pokeemerald link.c / link.h.
enum {
	MASTER_HANDSHAKE = 0x8FFF,
	SLAVE_HANDSHAKE = 0xB9A0,

	LINKCMD_SEND_LINK_TYPE = 0x2222,
	LINKCMD_READY_EXIT_STANDBY = 0x2FFE,
	LINKCMD_SEND_HELD_KEYS_OLD = 0x4444,
	LINKCMD_READY_CLOSE_LINK = 0x5FFF,
	LINKCMD_CONT_BLOCK = 0x8888,
	LINKCMD_INIT_BLOCK = 0xBBBB,
	LINKCMD_SEND_HELD_KEYS = 0xCAFE,
	LINKCMD_SEND_BLOCK_REQ = 0xCCCC,
};

enum {
	STATE_HANDSHAKE,
	STATE_PACKETS,
};

enum {
	CMD_WORDS = RFU_WRAPPER_CMD_LENGTH,
	// The game's master starts one transfer per frame and the rest of the packet from a timer (197 ticks of 64
	// cycles after each transfer interrupt); the driver does the same so the game's queues see the cable's timing.
	TRANSFER_GAP = 197 * 64,
	IDLE_POLL = 16384,

	BLOCK_CHUNK_BYTES = (CMD_WORDS - 1) * 2,
	LINK_PLAYER_BLOCK_SIZE = 0x3C,
	BLOCK_START_DELAY = 3, // packets between the block announcement and its data (LinkCB_BlockSendBegin)
};

static bool GBASIORFUWrapperInit(struct GBASIODriver* driver);
static void GBASIORFUWrapperDeinit(struct GBASIODriver* driver);
static void GBASIORFUWrapperReset(struct GBASIODriver* driver);
static void GBASIORFUWrapperSetMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static bool GBASIORFUWrapperHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode);
static int GBASIORFUWrapperConnectedDevices(struct GBASIODriver* driver);
static int GBASIORFUWrapperDeviceId(struct GBASIODriver* driver);
static uint16_t GBASIORFUWrapperWriteSIOCNT(struct GBASIODriver* driver, uint16_t value);
static void GBASIORFUWrapperFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]);

static void _tick(struct mTiming* timing, void* context, uint32_t cyclesLate);
static void _frameTick(struct mTiming* timing, void* context, uint32_t cyclesLate);

static const char* _commandName(uint16_t cmd) {
	switch (cmd) {
	case LINKCMD_SEND_LINK_TYPE:
		return "SEND_LINK_TYPE";
	case LINKCMD_READY_EXIT_STANDBY:
		return "READY_EXIT_STANDBY";
	case LINKCMD_SEND_HELD_KEYS_OLD:
		return "SEND_HELD_KEYS(0x4444)";
	case LINKCMD_READY_CLOSE_LINK:
		return "READY_CLOSE_LINK";
	case LINKCMD_CONT_BLOCK:
		return "CONT_BLOCK";
	case LINKCMD_INIT_BLOCK:
		return "INIT_BLOCK";
	case LINKCMD_SEND_HELD_KEYS:
		return "SEND_HELD_KEYS";
	case LINKCMD_SEND_BLOCK_REQ:
		return "SEND_BLOCK_REQ";
	case 0x5555:
	case 0x5566:
		return "DUMMY";
	case 0xAAAA:
		return "BLENDER_NO_PBLOCK_SPACE";
	case 0xAABB:
		return "READY_TO_TRADE";
	case 0xABCD:
		return "READY_FINISH_TRADE";
	case 0xBBCC:
		return "READY_CANCEL_TRADE";
	case 0xCCDD:
		return "START_TRADE";
	case 0xDCBA:
		return "CONFIRM_FINISH_TRADE";
	case 0xDDDD:
		return "SET_MONS_TO_TRADE";
	case 0xDDEE:
		return "PLAYER_CANCEL_TRADE";
	case 0xEEAA:
		return "REQUEST_CANCEL";
	case 0xEEBB:
		return "BOTH_CANCEL_TRADE";
	case 0xEECC:
		return "PARTNER_CANCEL_TRADE";
	case 0xEFFF:
		return "NONE";
	}
	return "?";
}

static void _traceCommand(struct GBASIORFUWrapper* wrapper, const char* who, const uint16_t command[CMD_WORDS]) {
	GBASIOCableTrace(wrapper->d.p, "%s %04X %s  %04X %04X %04X %04X %04X %04X %04X", who, command[0],
	                 _commandName(command[0]), command[1], command[2], command[3], command[4], command[5], command[6],
	                 command[7]);
}

// --- The built-in stub peer: a minimal FireRed at the far end of the cable -----------------------------------------
//
// It plays the master side of the cable club's setup, in the order pokeruby's cable_club.c expects from a master:
//   1. LINKCMD 0x2222 (only the master asks for the player data exchange; LinkCB_RequestPlayerDataExchange)
//   2. every player answers with its LinkPlayer block, carrying the sender's own link type; the game accepts the
//      exchange only when all blocks agree, so the stub waits for the game's block, reads its link type and answers
//      with a block of the same type
//   3. LINKCMD 0xCCCC 2 (sub_8007E9C(2)): every player sends its 100-byte trainer card block; the game leaves its
//      "awaiting link-up" screen when it holds a card from every player

enum {
	STUB_ANNOUNCE_PACKETS = 30, // packets after the link is up before the request goes out
	STUB_BLOCK_SIZE_TRAINER_CARD = 100,

	STUB_IDLE = 0,
	STUB_ANNOUNCE_DELAY,
	STUB_WAIT_GAME_BLOCK,
	STUB_SENDING_PLAYER,
	STUB_DONE,
};

static void _stubStartBlock(struct GBASIORFUWrapper* wrapper, unsigned size) {
	struct GBASIORFUWrapperStub* stub = &wrapper->stub;
	stub->txSize = size;
	stub->sending = true;
	stub->pos = 0;
	stub->delay = -1; // announce (INIT_BLOCK) first
}

static void _stubStartLinkPlayerBlock(struct GBASIORFUWrapper* wrapper) {
	struct GBASIORFUWrapperStub* stub = &wrapper->stub;
	static const char magic[] = "GameFreak inc.";
	memset(stub->block, 0, sizeof(stub->block));
	memcpy(&stub->block[0], magic, sizeof(magic)); // 14 characters and the terminator; the 16th byte stays zero
	uint8_t* player = &stub->block[16];
	// struct LinkPlayer (link.h): version, lp_field_2, trainerId, name[11], gender, linkType, id, language
	player[0] = 0x04;
	player[1] = 0x40; // version 0x4000 + 4: FireRed
	player[2] = 0x00;
	player[3] = 0x80; // lp_field_2: what FRLG sets (national dex)
	player[4] = 0x17;
	player[5] = 0x67; // trainer id 0x6717
	memset(&player[8], 0xFF, 11);
	player[8] = 0xC1; // "GSD" in the Gen 3 character set
	player[9] = 0xCD;
	player[10] = 0xBE;
	player[0x13] = 0; // gender
	player[0x14] = stub->linkType & 0xFF;
	player[0x15] = (stub->linkType >> 8) & 0xFF;
	player[0x16] = (stub->linkType >> 16) & 0xFF;
	player[0x17] = (stub->linkType >> 24) & 0xFF;
	player[0x18] = 0; // id: the virtual master
	player[0x19] = 0;
	player[0x1A] = 2; // language: English
	player[0x1B] = 0;
	memcpy(&stub->block[16 + 28], magic, sizeof(magic));
	_stubStartBlock(wrapper, LINK_PLAYER_BLOCK_SIZE);
}

static void _stubStartTrainerCardBlock(struct GBASIORFUWrapper* wrapper) {
	struct GBASIORFUWrapperStub* stub = &wrapper->stub;
	memset(stub->block, 0, sizeof(stub->block));
	// struct TrainerCard (pokeruby trainer_card.h): only what a card needs to be shown without complaint.
	stub->block[0x01] = 1; // stars
	stub->block[0x02] = 1; // hasPokedex
	stub->block[0x0E] = 0x17;
	stub->block[0x0F] = 0x67; // trainerId
	memset(&stub->block[0x30], 0xFF, 8);
	stub->block[0x30] = 0xC1; // "GSD"
	stub->block[0x31] = 0xCD;
	stub->block[0x32] = 0xBE;
	_stubStartBlock(wrapper, STUB_BLOCK_SIZE_TRAINER_CARD);
}

static void _stubGameCommand(void* context, const uint16_t command[CMD_WORDS]) {
	struct GBASIORFUWrapper* wrapper = context;
	struct GBASIORFUWrapperStub* stub = &wrapper->stub;
	switch (command[0]) {
	case LINKCMD_INIT_BLOCK:
		if (stub->stage == STUB_WAIT_GAME_BLOCK) {
			stub->rxSize = command[1];
			stub->rxPos = 0;
		}
		break;
	case LINKCMD_CONT_BLOCK:
		if (stub->stage == STUB_WAIT_GAME_BLOCK && stub->rxSize) {
			unsigned i;
			for (i = 0; i < CMD_WORDS - 1; ++i) {
				unsigned offset = stub->rxPos + 2 * i;
				if (offset + 1 < sizeof(stub->rxBlock)) {
					stub->rxBlock[offset] = command[i + 1] & 0xFF;
					stub->rxBlock[offset + 1] = command[i + 1] >> 8;
				}
			}
			stub->rxPos += BLOCK_CHUNK_BYTES;
			if (stub->rxPos >= stub->rxSize) {
				// LinkPlayerBlock: 16 bytes of magic, then struct LinkPlayer whose linkType is at +0x14.
				const uint8_t* type = &stub->rxBlock[16 + 0x14];
				stub->linkType = type[0] | (type[1] << 8) | (type[2] << 16) | ((uint32_t) type[3] << 24);
				GBASIOCableTrace(wrapper->d.p, "STUB the game's LinkPlayer block is in: link type %04X", stub->linkType);
				_stubStartLinkPlayerBlock(wrapper);
				stub->stage = STUB_SENDING_PLAYER;
			}
		}
		break;
	case LINKCMD_READY_EXIT_STANDBY:
	case LINKCMD_READY_CLOSE_LINK:
		// The barrier waits for every player to say the same; the virtual player agrees at once.
		stub->pendingBarrier = command[0];
		break;
	default:
		break;
	}
}

static bool _stubNextCommand(void* context, uint16_t command[CMD_WORDS]) {
	struct GBASIORFUWrapper* wrapper = context;
	struct GBASIORFUWrapperStub* stub = &wrapper->stub;
	if (stub->stage == STUB_IDLE) {
		stub->stage = STUB_ANNOUNCE_DELAY;
		stub->announceDelay = STUB_ANNOUNCE_PACKETS;
	}
	if (stub->stage == STUB_ANNOUNCE_DELAY) {
		if (--stub->announceDelay > 0) {
			return false;
		}
		stub->stage = STUB_WAIT_GAME_BLOCK;
		command[0] = LINKCMD_SEND_LINK_TYPE;
		command[1] = 0x1133; // the link type the cable club's trade uses; the game does not check the argument
		return true;
	}
	if (stub->sending) {
		if (stub->delay < 0) {
			command[0] = LINKCMD_INIT_BLOCK;
			command[1] = stub->txSize;
			command[2] = 0 + 128; // the virtual master is player 0
			stub->delay = BLOCK_START_DELAY;
			return true;
		}
		if (stub->delay > 0) {
			--stub->delay;
			return false;
		}
		command[0] = LINKCMD_CONT_BLOCK;
		unsigned i;
		for (i = 0; i < CMD_WORDS - 1; ++i) {
			unsigned offset = stub->pos + 2 * i;
			command[i + 1] = stub->block[offset] | (stub->block[offset + 1] << 8);
		}
		stub->pos += BLOCK_CHUNK_BYTES;
		if (stub->pos >= stub->txSize) {
			stub->sending = false;
		}
		return true;
	}
	if (stub->stage == STUB_SENDING_PLAYER) {
		// Our LinkPlayer block is out; now ask everyone (ourselves included) for the trainer card.
		stub->stage = STUB_DONE;
		command[0] = LINKCMD_SEND_BLOCK_REQ;
		command[1] = 2;
		_stubStartTrainerCardBlock(wrapper);
		return true;
	}
	if (stub->pendingBarrier) {
		command[0] = stub->pendingBarrier;
		stub->pendingBarrier = 0;
		return true;
	}
	return false;
}

static void _stubReset(void* context) {
	struct GBASIORFUWrapper* wrapper = context;
	memset(&wrapper->stub, 0, sizeof(wrapper->stub));
}

static const struct GBASIORFUWrapperPeer sStubPeer = {
	.gameCommand = _stubGameCommand,
	.nextCommand = _stubNextCommand,
	.reset = _stubReset,
};

// --- Driver ---------------------------------------------------------------------------------------------------------

void GBASIORFUWrapperCreate(struct GBASIORFUWrapper* wrapper, const char* connection) {
	memset(wrapper, 0, sizeof(*wrapper));
	wrapper->connection = connection ? connection : "stub";
	wrapper->d.init = GBASIORFUWrapperInit;
	wrapper->d.deinit = GBASIORFUWrapperDeinit;
	wrapper->d.reset = GBASIORFUWrapperReset;
	wrapper->d.setMode = GBASIORFUWrapperSetMode;
	wrapper->d.handlesMode = GBASIORFUWrapperHandlesMode;
	wrapper->d.connectedDevices = GBASIORFUWrapperConnectedDevices;
	wrapper->d.deviceId = GBASIORFUWrapperDeviceId;
	wrapper->d.writeSIOCNT = GBASIORFUWrapperWriteSIOCNT;
	wrapper->d.finishMultiplayer = GBASIORFUWrapperFinishMultiplayer;

	wrapper->tick.context = wrapper;
	wrapper->tick.name = "GBA RFU Wrapper Tick";
	wrapper->tick.callback = _tick;
	wrapper->tick.priority = 0x81;

	wrapper->frameTick.context = wrapper;
	wrapper->frameTick.name = "GBA RFU Wrapper Frame";
	wrapper->frameTick.callback = _frameTick;
	wrapper->frameTick.priority = 0x82;

	wrapper->peer = sStubPeer;
	wrapper->peer.context = wrapper;
}

void GBASIORFUWrapperDestroy(struct GBASIORFUWrapper* wrapper) {
	if (wrapper->air && wrapper->airDestroy) {
		wrapper->airDestroy(wrapper->air);
	}
	wrapper->air = NULL;
}

static void _frameTick(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBASIORFUWrapper* wrapper = context;
	if (wrapper->peer.frame) {
		wrapper->peer.frame(wrapper->peer.context);
	}
	mTimingSchedule(timing, &wrapper->frameTick, VIDEO_TOTAL_LENGTH - cyclesLate);
}

void GBASIORFUWrapperSetPeer(struct GBASIORFUWrapper* wrapper, const struct GBASIORFUWrapperPeer* peer) {
	wrapper->peer = peer ? *peer : sStubPeer;
	if (!peer) {
		wrapper->peer.context = wrapper;
	}
}

static void _restart(struct GBASIORFUWrapper* wrapper) {
	wrapper->state = STATE_HANDSHAKE;
	wrapper->handshakeReplies = 0;
	wrapper->slot = 0;
	wrapper->sum = 0;
	wrapper->sumToSend = 0;
	memset(wrapper->txCommand, 0, sizeof(wrapper->txCommand));
	memset(wrapper->rxCommand, 0, sizeof(wrapper->rxCommand));
	wrapper->frameStart = 0;
	if (wrapper->peer.reset) {
		wrapper->peer.reset(wrapper->peer.context);
	}
}

static void _schedule(struct GBASIORFUWrapper* wrapper, int32_t cycles) {
	struct mTiming* timing = &wrapper->d.p->p->timing;
	mTimingDeschedule(timing, &wrapper->tick);
	mTimingSchedule(timing, &wrapper->tick, cycles > 0 ? cycles : 1);
}

static void _ensureTick(struct GBASIORFUWrapper* wrapper) {
	struct mTiming* timing = &wrapper->d.p->p->timing;
	if (!mTimingIsScheduled(timing, &wrapper->tick)) {
		mTimingSchedule(timing, &wrapper->tick, IDLE_POLL);
	}
}

static bool GBASIORFUWrapperInit(struct GBASIODriver* driver) {
	struct GBASIORFUWrapper* wrapper = (struct GBASIORFUWrapper*) driver;
	_restart(wrapper);
	_schedule(wrapper, IDLE_POLL);
	mTimingDeschedule(&driver->p->p->timing, &wrapper->frameTick);
	mTimingSchedule(&driver->p->p->timing, &wrapper->frameTick, VIDEO_TOTAL_LENGTH);
	GBASIOCableTrace(driver->p, "DRIVER attached: cable wrapper, connection \"%s\" (stub peer: a FireRed at the far end)",
	                 wrapper->connection);
	return true;
}

static void GBASIORFUWrapperDeinit(struct GBASIODriver* driver) {
	struct GBASIORFUWrapper* wrapper = (struct GBASIORFUWrapper*) driver;
	if (driver->p) {
		mTimingDeschedule(&driver->p->p->timing, &wrapper->tick);
		mTimingDeschedule(&driver->p->p->timing, &wrapper->frameTick);
		mTimingDeschedule(&driver->p->p->timing, &driver->p->completeEvent);
	}
	GBASIOCableTrace(driver->p, "DRIVER detached");
}

static void GBASIORFUWrapperReset(struct GBASIODriver* driver) {
	struct GBASIORFUWrapper* wrapper = (struct GBASIORFUWrapper*) driver;
	// GBAReset clears the timing wheel, so the tick has to be armed again.
	_restart(wrapper);
	_schedule(wrapper, IDLE_POLL);
	mTimingDeschedule(&driver->p->p->timing, &wrapper->frameTick);
	mTimingSchedule(&driver->p->p->timing, &wrapper->frameTick, VIDEO_TOTAL_LENGTH);
	GBASIOCableTrace(driver->p, "RESET");
}

static void GBASIORFUWrapperSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	struct GBASIORFUWrapper* wrapper = (struct GBASIORFUWrapper*) driver;
	// The game closes the link by leaving multiplayer mode and opens it again by entering it: each entry is a fresh link.
	_restart(wrapper);
	_ensureTick(wrapper);
	GBASIOCableTrace(driver->p, "MODE %d%s", (int) mode, mode == GBA_SIO_MULTI ? " (multiplayer: link opening)" : "");
}

static bool GBASIORFUWrapperHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	return mode == GBA_SIO_MULTI;
}

static int GBASIORFUWrapperConnectedDevices(struct GBASIODriver* driver) {
	UNUSED(driver);
	return 1; // the virtual master
}

static int GBASIORFUWrapperDeviceId(struct GBASIODriver* driver) {
	UNUSED(driver);
	return 1; // the game is player 1
}

static uint16_t GBASIORFUWrapperWriteSIOCNT(struct GBASIODriver* driver, uint16_t value) {
	struct GBASIORFUWrapper* wrapper = (struct GBASIORFUWrapper*) driver;
	_ensureTick(wrapper);
	return GBASIOMultiplayerFillReady(value);
}

static uint16_t _masterWord(struct GBASIORFUWrapper* wrapper) {
	if (wrapper->state == STATE_HANDSHAKE) {
		return MASTER_HANDSHAKE;
	}
	if (wrapper->slot == 0) {
		return wrapper->sumToSend;
	}
	return wrapper->txCommand[wrapper->slot - 1];
}

static void _tick(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(timing);
	UNUSED(cyclesLate);
	struct GBASIORFUWrapper* wrapper = context;
	struct GBASIO* sio = wrapper->d.p;
	if (sio->mode != GBA_SIO_MULTI || GBASIOMultiplayerIsBusy(sio->siocnt)) {
		_schedule(wrapper, IDLE_POLL);
		return;
	}
	if (!wrapper->frameStart) {
		wrapper->frameStart = mTimingCurrentTime(&sio->p->timing);
	}
	wrapper->masterWord = _masterWord(wrapper);
	wrapper->slaveWord = sio->p->memory.io[GBA_REG(SIOMLT_SEND)];
	sio->siocnt |= 0x80;
	mTimingDeschedule(&sio->p->timing, &sio->completeEvent);
	mTimingSchedule(&sio->p->timing, &sio->completeEvent, GBASIOTransferCycles(GBA_SIO_MULTI, sio->siocnt, 1));
}

static void _packetDone(struct GBASIORFUWrapper* wrapper) {
	++wrapper->packets;
	if (wrapper->packets % 300 == 0) {
		GBASIOCableTrace(wrapper->d.p, "PACKETS %u siocnt=%04X", wrapper->packets, wrapper->d.p->siocnt);
	}
	bool any = false;
	unsigned i;
	for (i = 0; i < CMD_WORDS; ++i) {
		any = any || wrapper->rxCommand[i];
	}
	if (any) {
		_traceCommand(wrapper, "GAME ->", wrapper->rxCommand);
		if (wrapper->peer.gameCommand) {
			wrapper->peer.gameCommand(wrapper->peer.context, wrapper->rxCommand);
		}
	}
}

static void _loadNextCommand(struct GBASIORFUWrapper* wrapper) {
	memset(wrapper->txCommand, 0, sizeof(wrapper->txCommand));
	if (wrapper->peer.nextCommand && wrapper->peer.nextCommand(wrapper->peer.context, wrapper->txCommand)) {
		_traceCommand(wrapper, "PEER ->", wrapper->txCommand);
	}
}

static void GBASIORFUWrapperFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
	struct GBASIORFUWrapper* wrapper = (struct GBASIORFUWrapper*) driver;
	struct GBASIO* sio = driver->p;
	data[0] = wrapper->masterWord;
	data[1] = wrapper->slaveWord;
	data[2] = 0xFFFF;
	data[3] = 0xFFFF;

	int32_t delay = TRANSFER_GAP;
	if (wrapper->state == STATE_HANDSHAKE) {
		// The game counts a connection after two identical answers in a row; the master starts one per frame.
		if ((wrapper->slaveWord & ~3) == SLAVE_HANDSHAKE) {
			++wrapper->handshakeReplies;
			if (wrapper->handshakeReplies == 1) {
				GBASIOCableTrace(sio, "HANDSHAKE %04X %04X: the game answered", wrapper->masterWord, wrapper->slaveWord);
			}
		} else {
			wrapper->handshakeReplies = 0;
		}
		if (wrapper->handshakeReplies >= 2) {
			GBASIOCableTrace(sio, "LINK established (2 players: virtual master + game)");
			wrapper->state = STATE_PACKETS;
			wrapper->slot = 0;
			wrapper->sum = 0;
			wrapper->sumToSend = 0;
		}
		wrapper->frameStart += VIDEO_TOTAL_LENGTH;
		delay = wrapper->frameStart - mTimingCurrentTime(&sio->p->timing);
	} else if (wrapper->slot == 0) {
		// The checksum transfer is over; the eight command words follow.
		_loadNextCommand(wrapper);
		wrapper->sum = 0;
		wrapper->slot = 1;
	} else {
		unsigned index = wrapper->slot - 1;
		wrapper->sum += wrapper->masterWord + wrapper->slaveWord;
		wrapper->rxCommand[index] = wrapper->slaveWord;
		if (wrapper->slot == CMD_WORDS) {
			wrapper->sumToSend = wrapper->sum;
			wrapper->slot = 0;
			_packetDone(wrapper);
			// The next packet starts with the next frame, like the game's master (one packet per vblank).
			wrapper->frameStart += VIDEO_TOTAL_LENGTH;
			delay = wrapper->frameStart - mTimingCurrentTime(&sio->p->timing);
		} else {
			++wrapper->slot;
		}
	}
	while (delay <= 0) {
		delay += VIDEO_TOTAL_LENGTH;
		wrapper->frameStart += VIDEO_TOTAL_LENGTH;
	}
	_schedule(wrapper, delay);
}
