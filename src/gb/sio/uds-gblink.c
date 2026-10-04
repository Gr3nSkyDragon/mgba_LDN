/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-gblink.h>

#include <mgba/core/core.h>
#include <mgba/debugger/debugger.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/io.h>
#include <mgba/internal/gb/sio/uds-joiner.h>
#include <mgba/internal/sm83/sm83.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define GBVC_SYNC_SIXTIES 5 // after the host's answer: this many more 60|nybble, then GBVC_SYNC_ZEROS of 00
#define GBVC_SYNC_ZEROS 5
#define GBVC_SYNC_TIMEOUT_MS 20000
#define GBVC_SYNC_TAIL_MAX 40 // most units followed after the first 12
#define GBVC_SYNC_TAIL_QUIET_MS 250 // the host's burst is over when it has sent nothing for this long
#define GBVC_POLL_CYCLES 4096 // about 1 ms
#define GBVC_USING_INTERNAL_CLOCK 0x02
#define GBVC_FIRST_UNIT_BYTE 0x00 // the joiner's unit at index -2001; the host's is EF

struct VCAddr {
	int bank;
	uint16_t addr;
};

// Addresses in the pret builds (docs/hook_table); Red and Blue are laid out identically.
struct VCGame {
	const char* title; // the first 11 characters of the header title (see _findGame)
	struct VCAddr fakeBegin; // Link_fake_connection_status
	struct VCAddr fakeEnd; // Wireless_prompt
	struct VCAddr syncEntry; // Wireless_WaitLinkTransfer, the first instruction of Serial_SyncAndExchangeNybble
	uint16_t syncReturn; // Wireless_WaitLinkTransfer_ret: the `ret` of that routine
	uint16_t delayFrame;
	uint16_t sendNybble; // wSerialExchangeNybbleSendData
	uint16_t receiveNybble; // wSerialExchangeNybbleReceiveData
	uint16_t syncReceive; // wSerialSyncAndExchangeNybbleReceiveData
	uint16_t unknownCounter; // wUnknownSerialCounter
	uint16_t connectionStatus; // hSerialConnectionStatus
	uint16_t menuEntry; // Serial_ExchangeLinkMenuSelection (bank 0)
	uint16_t menuReturn; // its final `ret`
	uint16_t menuSend; // wLinkMenuSelectionSendBuffer (two bytes)
	uint16_t menuReceive; // wLinkMenuSelectionReceiveBuffer (two bytes)
};

static const struct VCGame sGames[] = {
	{"POKEMON RED", {1, 0x7202}, {1, 0x7260}, {0, 0x227F}, 0x22C2, 0x20AF, 0xCC42, 0xCC3E, 0xCC3D, 0xCC47, 0xFFAA, 0x2247, 0x226D, 0xCC42, 0xCC3D},
	{"POKEMON BLU", {1, 0x7202}, {1, 0x7260}, {0, 0x227F}, 0x22C2, 0x20AF, 0xCC42, 0xCC3E, 0xCC3D, 0xCC47, 0xFFAA, 0x2247, 0x226D, 0xCC42, 0xCC3D},
	{"POKEMON YEL", {1, 0x7077}, {1, 0x70D8}, {0, 0x20DB}, 0x211E, 0x1E64, 0xCC42, 0xCC3E, 0xCC3D, 0xCC47, 0xFFAA, 0x20A3, 0x20C9, 0xCC42, 0xCC3D},
};

// Recognised, but there is no Gen 2 driver yet (different link code and a different key, see the plan's open questions).
static const char* const sGen2Titles[] = {"POKEMON_GLD", "POKEMON_SLV", "PM_CRYSTAL"};

// What the hooked ROM routine is doing. While one is active the serial device does not pair transfers.
enum HleKind {
	HLE_NONE,
	HLE_SYNC, // Serial_SyncAndExchangeNybble
	HLE_MENU, // Serial_ExchangeLinkMenuSelection
};

enum SyncPhase {
	SYNC_IDLE,
	SYNC_WAIT, // exchanging 60|nybble units until the host's 6x arrives
	SYNC_AFTER, // the ROM's two shorter loops after that
	SYNC_TAIL, // following the host's burst to its end
};

struct GBVCLink {
	struct GBSIODriver d;
	struct mDebuggerModule module;
	struct mTimingEvent event;
	struct mCore* core;
	struct GB* gb;
	const struct VCGame* game;
	struct UDSJoiner joiner;

	bool attachedDriver;
	bool attachedModule;
	ssize_t fakeBeginId;
	ssize_t fakeEndId;
	ssize_t syncId;
	ssize_t menuId;

	uint32_t startMs;
	bool fake; // between Link_fake_connection_status and Wireless_prompt
	bool started; // the Pia session is up and the first unit has been sent
	bool discardFirst; // the host's first unit (EF) is not a transfer

	bool txPending; // a transfer is waiting for the peer's unit
	uint8_t txByte;
	unsigned txUnits;
	unsigned rxUnits;

	enum SyncPhase syncPhase;
	unsigned syncSentN;
	unsigned syncRecvN;
	uint32_t syncTailMs;
	unsigned syncTail;
	enum HleKind hle;
	bool menuSent;
	uint32_t menuStartMs;
	unsigned menuGot;
	uint8_t menuBytes[3];
	uint32_t syncStartMs;
	int syncNybble;
	uint16_t savedBC, savedDE, savedHL;

	int lastRoom;
	int lastSession;
	FILE* trace;
};

static uint32_t _nowMs(void) {
#ifdef _WIN32
	return (uint32_t) GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

static void _trace(struct GBVCLink* link, const char* format, ...) {
	if (!link->trace) {
		return;
	}
	va_list args;
	va_start(args, format);
	fprintf(link->trace, "%8u ", _nowMs() - link->startMs);
	vfprintf(link->trace, format, args);
	fputc('\n', link->trace);
	fflush(link->trace);
	va_end(args);
}

// The header title is 16 bytes at 0x134, but mGBA's own game info cuts it to 11 for newer headers, and a title can be
// longer than the 11 that are always there ("POKEMON BLUE" is 12, "POKEMON YELLOW" 14). Match on the first 11, read straight
// from the header, as the link tracer does.
#define VC_TITLE_KEY 11

static void _headerTitle(struct mCore* core, char out[VC_TITLE_KEY + 1]) {
	size_t i;
	memset(out, 0, VC_TITLE_KEY + 1);
	for (i = 0; i < VC_TITLE_KEY; ++i) {
		uint32_t ch = core->rawRead8(core, 0x134 + i, 0);
		if (ch < 0x20 || ch >= 0x7F) {
			break;
		}
		out[i] = (char) ch;
	}
}

static const struct VCGame* _findGame(struct mCore* core, const char* title) {
	UNUSED(core);
	size_t i;
	for (i = 0; i < sizeof(sGames) / sizeof(sGames[0]); ++i) {
		if (strncmp(title, sGames[i].title, VC_TITLE_KEY) == 0) {
			return &sGames[i];
		}
	}
	return NULL;
}

// Guest access --------------------------------------------------------------------------------------------------------

static uint8_t _read8(struct GBVCLink* link, uint16_t address) {
	return link->core->rawRead8(link->core, address, -1);
}

static void _write8(struct GBVCLink* link, uint16_t address, uint8_t value) {
	link->core->rawWrite8(link->core, address, -1, value);
}

static void _setPC(struct GBVCLink* link, uint16_t address) {
	link->core->writeRegister(link->core, "pc", address);
}

// Pushes `address` as a return address and continues at `target`, as if the code had called it.
static void _call(struct GBVCLink* link, uint16_t target, uint16_t returnAddress) {
	struct SM83Core* cpu = link->core->cpu;
	uint16_t sp = cpu->sp - 2;
	_write8(link, sp, returnAddress & 0xFF);
	_write8(link, sp + 1, returnAddress >> 8);
	cpu->sp = sp;
	_setPC(link, target);
}

// The UDS side --------------------------------------------------------------------------------------------------------

static bool _ready(const struct GBVCLink* link) {
	return link->started && link->joiner.sessionActive && link->joiner.session.state == UDS_STATE_JOINED;
}

static void _queueUnit(struct GBVCLink* link, uint8_t byte) {
	if (!udsSessionQueueUnit(&link->joiner.session, byte)) {
		_trace(link, "tx window full, byte %02X dropped", byte);
		return;
	}
	++link->txUnits;
}

static void _flush(struct GBVCLink* link) {
	udsSessionFlush(&link->joiner.session, link->joiner.nowMs);
}

static bool _popUnit(struct GBVCLink* link, uint8_t* byte) {
	if (!udsSessionPopUnit(&link->joiner.session, byte)) {
		return false;
	}
	++link->rxUnits;
	return true;
}

static void _logStates(struct GBVCLink* link) {
	int room = link->joiner.room.state;
	if (room != link->lastRoom) {
		static const char* const names[] = {"scanning for a host", "authenticating", "associated, EAPoL start sent", "joined"};
		link->lastRoom = room;
		mLOG(GB_SIO, INFO, "Virtual Console: %s (network %08x, beacons %u)", names[room], link->joiner.room.host.networkId,
		     link->joiner.room.beaconsSeen);
		_trace(link, "room: %s", names[room]);
	}
	int session = link->joiner.sessionActive ? (int) link->joiner.session.state : -1;
	if (session != link->lastSession) {
		static const char* const names[] = {"idle", "exchanging setup messages", "joined, game stream may start", "closed"};
		link->lastSession = session;
		if (session >= 0) {
			mLOG(GB_SIO, INFO, "Virtual Console: Pia session %s (frames in %u, out %u)", names[session],
			     link->joiner.session.framesReceived, link->joiner.session.framesSent);
			_trace(link, "session: %s", names[session]);
		}
	}
}

// Transfer completion -------------------------------------------------------------------------------------------------

static void _finishTransfer(struct GBVCLink* link, uint8_t byte) {
	struct GBSIO* sio = &link->gb->sio;
	link->txPending = false;
	sio->pendingSB = byte;
	if (GBRegisterSCIsEnable(link->gb->memory.io[GB_REG_SC])) {
		sio->remainingBits = 8;
		mTimingDeschedule(&link->gb->timing, &sio->event);
		mTimingSchedule(&link->gb->timing, &sio->event, 0);
	}
}

static void _poll(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBVCLink* link = context;
	uint32_t now = _nowMs() - link->startMs;
	udsJoinerPoll(&link->joiner, now);
	_logStates(link);

	if (!link->started && udsJoinerReady(&link->joiner)) {
		link->started = true;
		link->discardFirst = true;
		_queueUnit(link, GBVC_FIRST_UNIT_BYTE);
		_flush(link);
		_trace(link, "link up: first unit %02X sent", GBVC_FIRST_UNIT_BYTE);
		mLOG(GB_SIO, INFO, "Virtual Console: link up");
	} else if (link->started && !_ready(link)) {
		// The host went away (bye, silence). Let the game's pending transfer finish on an idle line.
		link->started = false;
		link->syncPhase = SYNC_IDLE;
		mLOG(GB_SIO, INFO, "Virtual Console: link lost");
		_trace(link, "link lost");
		if (link->txPending) {
			_finishTransfer(link, 0xFF);
		}
	}

	if (link->started) {
		uint8_t byte;
		if (link->discardFirst && udsSessionUnitsWaiting(&link->joiner.session)) {
			_popUnit(link, &byte);
			link->discardFirst = false;
			_trace(link, "host first unit %02X discarded", byte);
		}
		if (link->txPending && !link->discardFirst && link->hle == HLE_NONE && _popUnit(link, &byte)) {
			_trace(link, "xfer tx %02X rx %02X", link->txByte, byte);
			_finishTransfer(link, byte);
		}
		_flush(link);
	}

	mTimingSchedule(timing, &link->event, GBVC_POLL_CYCLES - cyclesLate);
}

// The serial device ---------------------------------------------------------------------------------------------------

static bool _driverInit(struct GBSIODriver* driver) {
	UNUSED(driver);
	return true;
}

static void _driverDeinit(struct GBSIODriver* driver) {
	UNUSED(driver);
}

static void _driverWriteSB(struct GBSIODriver* driver, uint8_t value) {
	UNUSED(driver);
	UNUSED(value);
}

static uint8_t _driverWriteSC(struct GBSIODriver* driver, uint8_t value) {
	struct GBVCLink* link = (struct GBVCLink*) driver;
	// Enable and internal clock: a transfer the game starts as master. (External-clock starts have no peer clocking
	// them: both VC consoles are the internal side, so they are left alone.)
	if ((value & 0x81) == 0x81 && !link->fake && _ready(link)) {
		struct GBSIO* sio = &link->gb->sio;
		// Take the transfer away from the built-in shifter; the poll finishes it when the peer's unit arrives.
		mTimingDeschedule(&link->gb->timing, &sio->event);
		sio->remainingBits = 0;
		if (!link->txPending) {
			link->txByte = link->gb->memory.io[GB_REG_SB];
			link->txPending = true;
			_queueUnit(link, link->txByte);
			_flush(link);
		}
	}
	return value;
}

// Hooks ---------------------------------------------------------------------------------------------------------------

static void _hookFakeBegin(struct GBVCLink* link) {
	// What the VC does here: the receptionist's "Please wait" is passed by claiming the internal clock.
	_write8(link, link->game->connectionStatus, GBVC_USING_INTERNAL_CLOCK);
	link->fake = true;
	_trace(link, "hook: Link_fake_connection_status");
}

static void _hookFakeEnd(struct GBVCLink* link) {
	link->fake = false;
	_trace(link, "hook: Wireless_prompt");
}

// Lets the game run one frame and come back to the hooked instruction. DelayFrame returns to `entry`.
static void _yield(struct GBVCLink* link, uint16_t entry) {
	_call(link, link->game->delayFrame, entry);
}

static void _saveRegs(struct GBVCLink* link) {
	struct SM83Core* cpu = link->core->cpu;
	link->savedBC = cpu->bc;
	link->savedDE = cpu->de;
	link->savedHL = cpu->hl;
}

static void _syncFinish(struct GBVCLink* link, int nybble) {
	const struct VCGame* game = link->game;
	struct SM83Core* cpu = link->core->cpu;
	_write8(link, game->receiveNybble, nybble);
	_write8(link, game->syncReceive, nybble);
	// The registers the ROM's own routine would leave: B is 0 (its loops end on `dec b`), A holds the nybble, Z is set.
	cpu->bc = link->savedBC & 0x00FF;
	cpu->de = link->savedDE;
	cpu->hl = link->savedHL;
	cpu->a = nybble;
	cpu->f.packed = 0x80;
	_setPC(link, game->syncReturn);
	link->syncPhase = SYNC_IDLE;
	link->hle = HLE_NONE;
	_trace(link, "sync done: nybble %X, %u host units followed past the first 12", nybble, link->syncTail);
}

// Serial_SyncAndExchangeNybble, as the ROM's own loops would run it against the host, one frame at a time:
//   1. exchange 60|nybble units, one for each host unit, until a unit from the host is a 6x byte (the ROM's loop1: how long
//      this lasts is how long the host takes to arrive; Azahar's host sent 164 units of 60 in one wait)
//   2. five more 60|nybble and five 00 (the ROM's loop2 and loop3, which the VC shortens: 7 + 5 in a retail pair)
//   3. keep answering the host's burst one unit at a time for as long as it is still sending sync units (00 or 6x), so that
//      both sides leave the sync having sent the same number of units, whatever length the host used.
// Every unit of the host's that is read is matched by one unit of ours, so the two streams stay paired.
static void _syncSend(struct GBVCLink* link, uint8_t byte) {
	_queueUnit(link, byte);
	++link->syncSentN;
}

static bool _syncPop(struct GBVCLink* link, uint8_t* byte) {
	if (link->syncRecvN >= link->syncSentN || link->discardFirst || !_popUnit(link, byte)) {
		return false;
	}
	++link->syncRecvN;
	return true;
}

static void _hookSync(struct GBVCLink* link) {
	const struct VCGame* game = link->game;
	uint32_t now = _nowMs() - link->startMs;
	uint8_t byte;

	if (link->syncPhase == SYNC_IDLE) {
		_saveRegs(link);
		link->hle = HLE_SYNC;
		link->syncStartMs = now;
		link->syncSentN = link->syncRecvN = 0;
		link->syncTail = 0;
		link->syncNybble = -1;
		link->syncPhase = SYNC_WAIT;
		_trace(link, "hook: nybble sync, link %s", _ready(link) ? "up" : "not up yet");
	}

	if (_ready(link)) {
		uint8_t nybble = _read8(link, game->sendNybble) & 0x0F;
		if (link->syncPhase == SYNC_WAIT) {
			// Phase 1. One unit out for each unit in; nothing is read before it has been paired with one of ours.
			unsigned guard;
			for (guard = 0; guard < 400; ++guard) {
				if (link->syncSentN == link->syncRecvN) {
					_syncSend(link, 0x60 | nybble);
				}
				if (!_syncPop(link, &byte)) {
					break;
				}
				if ((byte & 0xF0) == 0x60) {
					link->syncNybble = byte & 0x0F;
					break;
				}
			}
			_flush(link);
			if (link->syncNybble >= 0) {
				unsigned i;
				for (i = 0; i < GBVC_SYNC_SIXTIES; ++i) {
					_syncSend(link, 0x60 | nybble);
				}
				for (i = 0; i < GBVC_SYNC_ZEROS; ++i) {
					_syncSend(link, 0x00);
				}
				_flush(link);
				link->syncPhase = SYNC_AFTER;
				_trace(link, "sync: host answered %X after %u units; sending %d more %X and %d 00", link->syncNybble,
				       link->syncRecvN, GBVC_SYNC_SIXTIES, nybble, GBVC_SYNC_ZEROS);
			}
		}
		if (link->syncPhase == SYNC_AFTER) {
			while (link->syncRecvN < link->syncSentN && _syncPop(link, &byte)) {
			}
			if (link->syncRecvN >= link->syncSentN) {
				link->syncPhase = SYNC_TAIL;
				link->syncTailMs = now;
			}
		}
		if (link->syncPhase == SYNC_TAIL) {
			// A unit that is not 00 or 6x belongs to what the host does next: leave it.
			while (link->syncTail < GBVC_SYNC_TAIL_MAX && udsSessionPeekUnit(&link->joiner.session, &byte) &&
			       (byte == 0x00 || (byte & 0xF0) == 0x60)) {
				_syncSend(link, 0x00);
				_syncPop(link, &byte);
				++link->syncTail;
				link->syncTailMs = now;
			}
			_flush(link);
			bool another = udsSessionPeekUnit(&link->joiner.session, &byte);
			// Done when the host has moved on (a different unit is waiting), or has been quiet for a moment.
			if ((another && byte != 0x00 && (byte & 0xF0) != 0x60) || link->syncTail >= GBVC_SYNC_TAIL_MAX ||
			    now - link->syncTailMs > GBVC_SYNC_TAIL_QUIET_MS) {
				_syncFinish(link, link->syncNybble);
				return;
			}
		}
	}

	if (now - link->syncStartMs > GBVC_SYNC_TIMEOUT_MS && link->syncPhase == SYNC_WAIT) {
		// Nobody answered: the ROM's own way out is the counter at FFFF ("link closed because of inactivity").
		_write8(link, game->unknownCounter, 0xFF);
		_write8(link, game->unknownCounter + 1, 0xFF);
		_trace(link, "sync timed out");
		_syncFinish(link, 0xFF);
		return;
	}

	_yield(link, game->syncEntry.addr);
}

// Serial_ExchangeLinkMenuSelection. The ROM exchanges three bytes per call (the first is discarded, the next two are kept)
// and wants a "D0 class" byte in either kept slot. Which of the host's bytes lands in which slot depends on how the two
// loops line up, and the host leaves the menu as soon as it sees our A press, so a missed byte is never repeated. Here a
// call sends the selection three times and keeps the last D0-class byte of the three the host sent, in both slots, which
// does not depend on the alignment. The host reads the same constant selection whichever of our units it looks at.
static void _menuFinish(struct GBVCLink* link) {
	const struct VCGame* game = link->game;
	struct SM83Core* cpu = link->core->cpu;
	int valid = -1;
	unsigned i;
	for (i = 0; i < 3; ++i) {
		if ((link->menuBytes[i] & 0xF0) == 0xD0) {
			valid = link->menuBytes[i];
		}
	}
	uint8_t first = valid >= 0 ? valid : link->menuBytes[1];
	uint8_t second = valid >= 0 ? valid : link->menuBytes[2];
	_write8(link, game->menuReceive, first);
	_write8(link, game->menuReceive + 1, second);
	cpu->bc = link->savedBC;
	cpu->de = link->savedDE;
	cpu->hl = link->savedHL;
	_setPC(link, game->menuReturn);
	link->hle = HLE_NONE;
	_trace(link, "menu done: sent %02X, host sent %02X %02X %02X -> %02X %02X", _read8(link, game->menuSend),
	       link->menuBytes[0], link->menuBytes[1], link->menuBytes[2], first, second);
}

static void _hookMenu(struct GBVCLink* link) {
	const struct VCGame* game = link->game;
	uint32_t now = _nowMs() - link->startMs;

	if (link->hle != HLE_MENU) {
		_saveRegs(link);
		link->hle = HLE_MENU;
		link->menuSent = false;
		link->menuGot = 0;
		link->menuStartMs = now;
	}

	if (_ready(link) && !link->menuSent) {
		uint8_t selection = _read8(link, game->menuSend);
		unsigned i;
		for (i = 0; i < 3; ++i) {
			_queueUnit(link, selection);
		}
		_flush(link);
		link->menuSent = true;
	}

	if (_ready(link) && link->menuSent) {
		uint8_t byte;
		while (link->menuGot < 3 && !link->discardFirst && _popUnit(link, &byte)) {
			link->menuBytes[link->menuGot++] = byte;
		}
		if (link->menuGot >= 3) {
			_menuFinish(link);
			return;
		}
	}

	if (!_ready(link) && link->menuSent) {
		// The link dropped in the middle of a call: give the ROM what an idle line gives.
		link->menuBytes[0] = link->menuBytes[1] = link->menuBytes[2] = 0xFF;
		_menuFinish(link);
		return;
	}

	_yield(link, game->menuEntry);
}

static void _moduleNoop(struct mDebuggerModule* module) {
	UNUSED(module);
}

static void _modulePaused(struct mDebuggerModule* module, int32_t timeoutMs) {
	UNUSED(module);
	UNUSED(timeoutMs);
}

static void _moduleEntered(struct mDebuggerModule* module, enum mDebuggerEntryReason reason, struct mDebuggerEntryInfo* info) {
	struct GBVCLink* link = (struct GBVCLink*) ((char*) module - offsetof(struct GBVCLink, module));
	module->isPaused = false;
	if (reason != DEBUGGER_ENTER_BREAKPOINT) {
		return;
	}
	if (info->pointId == link->fakeBeginId) {
		_hookFakeBegin(link);
	} else if (info->pointId == link->fakeEndId) {
		_hookFakeEnd(link);
	} else if (info->pointId == link->syncId) {
		_hookSync(link);
	} else if (info->pointId == link->menuId) {
		_hookMenu(link);
	}
}

static ssize_t _setBreakpoint(struct GBVCLink* link, struct VCAddr at) {
	struct mDebuggerPlatform* platform = link->module.p ? link->module.p->platform : NULL;
	if (!platform || !platform->setBreakpoint) {
		return -1;
	}
	struct mBreakpoint bp = {
		.address = at.addr,
		.segment = at.bank == 0 ? -1 : at.bank,
		.type = BREAKPOINT_HARDWARE,
	};
	return platform->setBreakpoint(platform, &link->module, &bp);
}

// Create / destroy ----------------------------------------------------------------------------------------------------

struct GBVCLink* GBVCLinkCreate(struct mCore* core, struct mDebugger* debugger, const uint16_t name[GBVC_NAME_WORDS],
                                uint16_t listenPort, uint16_t sendPort) {
	if (!core || core->platform(core) != mPLATFORM_GB) {
		return NULL;
	}
	char title[VC_TITLE_KEY + 1];
	_headerTitle(core, title);
	const struct VCGame* game = _findGame(core, title);
	if (!game) {
		size_t i;
		for (i = 0; i < sizeof(sGen2Titles) / sizeof(sGen2Titles[0]); ++i) {
			if (strncmp(title, sGen2Titles[i], VC_TITLE_KEY) == 0) {
				mLOG(GB_SIO, WARN, "Virtual Console: Gen 2 (%s) is not supported yet, only Red, Blue and Yellow", title);
				return NULL;
			}
		}
		mLOG(GB_SIO, WARN, "Virtual Console: this game is not one the wrapper knows (header title \"%s\"; Red, Blue and Yellow are)",
		     title);
		return NULL;
	}
	if (!debugger) {
		mLOG(GB_SIO, ERROR, "Virtual Console: this build has no debugger, so the ROM hooks cannot be installed");
		return NULL;
	}
	struct GBVCLink* link = calloc(1, sizeof(*link));
	link->core = core;
	link->gb = core->board;
	link->game = game;
	link->startMs = _nowMs();
	link->lastRoom = -1;
	link->lastSession = -1;
	link->fakeBeginId = link->fakeEndId = link->syncId = link->menuId = -1;

	const char* dir = getenv("MGBA_VCLINK_TRACE");
	if (dir && *dir) {
		char path[512];
		snprintf(path, sizeof(path), "%s/vclink_%u.txt", dir, (unsigned) time(NULL));
		link->trace = fopen(path, "w");
	}

	if (!udsJoinerOpen(&link->joiner, name, listenPort, sendPort)) {
		mLOG(GB_SIO, ERROR, "Virtual Console: cannot listen on 127.0.0.1:%u (is another mGBA using it?)",
		     listenPort ? listenPort : 0);
		GBVCLinkDestroy(link);
		return NULL;
	}

	link->d.init = _driverInit;
	link->d.deinit = _driverDeinit;
	link->d.writeSB = _driverWriteSB;
	link->d.writeSC = _driverWriteSC;

	if (!core->debugger) {
		mDebuggerAttach(debugger, core);
	}
	link->module.type = DEBUGGER_CUSTOM;
	link->module.init = _moduleNoop;
	link->module.deinit = _moduleNoop;
	link->module.paused = _modulePaused;
	link->module.update = _moduleNoop;
	link->module.entered = _moduleEntered;
	link->module.custom = _moduleNoop;
	link->module.interrupt = _moduleNoop;
	link->module.isPaused = false;
	link->module.needsCallback = false;
	mDebuggerAttachModule(core->debugger, &link->module);
	link->attachedModule = true;
	link->fakeBeginId = _setBreakpoint(link, game->fakeBegin);
	link->fakeEndId = _setBreakpoint(link, game->fakeEnd);
	link->syncId = _setBreakpoint(link, game->syncEntry);
	link->menuId = _setBreakpoint(link, (struct VCAddr) {0, game->menuEntry});
	if (link->fakeBeginId < 0 || link->fakeEndId < 0 || link->syncId < 0 || link->menuId < 0) {
		mLOG(GB_SIO, ERROR, "Virtual Console: could not install the ROM hooks");
		GBVCLinkDestroy(link);
		return NULL;
	}

	GBSIOSetDriver(&link->gb->sio, &link->d);
	link->attachedDriver = true;

	link->event.context = link;
	link->event.name = "GB VC link";
	link->event.callback = _poll;
	link->event.priority = 0x40;
	mTimingSchedule(&link->gb->timing, &link->event, GBVC_POLL_CYCLES);

	mLOG(GB_SIO, INFO, "Virtual Console: %s, waiting for Azahar's UDS bridge on 127.0.0.1:%u (sending to %u)",
	     game->title, link->joiner.udp.listenPort, link->joiner.udp.sendPort);
	_trace(link, "created for %s", game->title);
	return link;
}

void GBVCLinkDestroy(struct GBVCLink* link) {
	if (!link) {
		return;
	}
	if (link->attachedDriver) {
		mTimingDeschedule(&link->gb->timing, &link->event);
		GBSIOSetDriver(&link->gb->sio, NULL);
	}
	if (link->attachedModule && link->core->debugger) {
		struct mDebuggerPlatform* platform = link->module.p ? link->module.p->platform : NULL;
		if (platform && platform->clearBreakpoint) {
			ssize_t ids[4] = {link->fakeBeginId, link->fakeEndId, link->syncId, link->menuId};
			size_t i;
			for (i = 0; i < 4; ++i) {
				if (ids[i] >= 0) {
					platform->clearBreakpoint(platform, ids[i]);
				}
			}
		}
		mDebuggerDetachModule(link->core->debugger, &link->module);
	}
	udsJoinerClose(&link->joiner);
	if (link->trace) {
		fclose(link->trace);
	}
	mLOG(GB_SIO, INFO, "Virtual Console: link stopped");
	free(link);
}
