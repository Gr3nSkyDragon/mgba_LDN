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
#include <mgba/internal/gb/sio/uds-cable.h>
#include <mgba/internal/gb/sio/uds-esp32.h>
#include <mgba/internal/gb/sio/uds-joiner.h>
#include <mgba/internal/gb/sio/uds-keyfile.h>
#include <mgba/internal/gb/sio/uds-wire.h>
#include <mgba/internal/sm83/sm83.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define GBVC_POLL_CYCLES 4096 // about 1 ms
#define GBVC_USING_INTERNAL_CLOCK 0x02

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
	uint16_t curMap; // wCurMap, for the trace heartbeat only
	uint16_t linkState; // wLinkState, likewise
	bool gen2; // the wire front end recognises the 70 and 80 syncs
	uint32_t commFamily; // the upper part of the WLAN communication id of the title's UDS network (see VC_COMM_MASK); 0 is the Gen 1 titles'
};

// The comm id is the title id plus 0x10 (Red 00171000 advertises 00171010) and so differs between versions: Azahar hosting Gold or
// Silver advertised both 00172610 and 00172710. Titles of one generation share all but the low 12 bits.
#define VC_COMM_MASK 0xFFFFF000u
#define VC_GEN1_COMM_FAMILY 0x00171000u

static const struct VCGame sGames[] = {
	{"POKEMON RED", {1, 0x7202}, {1, 0x7260}, {0, 0x227F}, 0x22C2, 0x20AF, 0xCC42, 0xCC3E, 0xCC3D, 0xCC47, 0xFFAA, 0x2247, 0x226D, 0xCC42, 0xCC3D, 0xD35E, 0xD12B},
	{"POKEMON BLU", {1, 0x7202}, {1, 0x7260}, {0, 0x227F}, 0x22C2, 0x20AF, 0xCC42, 0xCC3E, 0xCC3D, 0xCC47, 0xFFAA, 0x2247, 0x226D, 0xCC42, 0xCC3D, 0xD35E, 0xD12B},
	{"POKEMON YEL", {1, 0x7077}, {1, 0x70D8}, {0, 0x20DB}, 0x211E, 0x1E64, 0xCC42, 0xCC3E, 0xCC3D, 0xCC47, 0xFFAA, 0x20A3, 0x20C9, 0xCC42, 0xCC3D, 0xD35D, 0xD12A},
};

// Gen 2 has different link code (the syncs use the 60, 70 or 80 range by link mode, the room is confirmed by a $D0+room exchange
// (Link_EnsureSync) instead of the Gen 1 menu, and a mail block follows the patch lists), so only the wire mode, which hooks nothing, carries it: its entries carry just the
// addresses the heartbeat reads (wMapNumber, wLinkMode, hSerialConnectionStatus; Gold and Silver share a layout) and the family of the
// comm ids of the titles' networks (00172610 and 00172710 have been seen from Azahar hosting Gold or Silver).
static const struct VCGame sGen2Games[] = {
	{.title = "POKEMON_GLD", .connectionStatus = 0xFFCD, .curMap = 0xDA01, .linkState = 0xD042, .commFamily = 0x00172000, .gen2 = true},
	{.title = "POKEMON_SLV", .connectionStatus = 0xFFCD, .curMap = 0xDA01, .linkState = 0xD042, .commFamily = 0x00172000, .gen2 = true},
	{.title = "PM_CRYSTAL", .connectionStatus = 0xFFCB, .curMap = 0xDCB6, .linkState = 0xC2DC, .commFamily = 0x00172000, .gen2 = true},
};

// What the hooked ROM routine is doing. While one is active the serial device does not pair transfers.
enum HleKind {
	HLE_NONE,
	HLE_SYNC, // Serial_SyncAndExchangeNybble
	HLE_MENU, // Serial_ExchangeLinkMenuSelection
};

struct GBVCLink {
	struct GBSIODriver d;
	struct mDebuggerModule module;
	struct mTimingEvent event;
	struct mCore* core;
	struct GB* gb;
	const struct VCGame* game;
	struct UDSJoiner joiner;
	struct UDSCable cable; // the part of the translation that does not need the emulator (uds-cable.c)
	struct UDSWire wire; // wire mode: the permanent slave (uds-wire.c); then the ROM hooks and `cable` above are not used
	bool wireMode;
	int lastWirePhase;
	unsigned wireExchanges;

	// Board mode (GBVC_AIR_BOARD): the wrapper runs on the ESP32; the joiner, cable and wire above are not used.
	bool boardMode;
	int boardState; // enum BoardState
	struct UDSEsp32 esp;
	uint32_t boardSinceMs;
	uint32_t boardHelloMs;
	char boardTitle[UDS_ESP32_GB_TITLE + 1];
	char keyPath[512];
	uint16_t name[10];
	uint8_t boardPhase; // the wire phase the board last reported
	unsigned boardTimeouts;

	bool attachedDriver;
	bool attachedModule;
	ssize_t fakeBeginId;
	ssize_t fakeEndId;
	ssize_t syncId;
	ssize_t menuId;

	uint32_t startMs;
	bool fake; // between Link_fake_connection_status and Wireless_prompt
	bool started; // the Pia session is up and the first unit has been sent

	unsigned txUnits;
	unsigned rxUnits;

	enum HleKind hle;
	uint16_t savedBC, savedDE, savedHL;

	int lastRoom;
	int lastSession;
	int lastRadio;
	uint32_t lastRadioStatsMs;
	uint32_t lastHeartbeatMs;
	uint32_t lastHookMs; // when a ROM hook last ran, and which
	const char* lastHookName;
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

// The unit port the cable code (uds-cable.c) talks to: the joiner's Pia session, with the trace and counters of this driver.
static bool _portReady(void* context) {
	return _ready(context);
}

static bool _portQueue(void* context, uint8_t byte) {
	struct GBVCLink* link = context;
	if (!udsSessionQueueUnit(&link->joiner.session, byte)) {
		_trace(link, "tx window full, byte %02X dropped", byte);
		return false;
	}
	_trace(link, "tx %02X", byte);
	++link->txUnits;
	return true;
}

static void _portFlush(void* context) {
	struct GBVCLink* link = context;
	udsSessionFlush(&link->joiner.session, link->joiner.nowMs);
}

static bool _portPop(void* context, uint8_t* byte) {
	struct GBVCLink* link = context;
	if (!udsSessionPopUnit(&link->joiner.session, byte)) {
		return false;
	}
	_trace(link, "rx %02X", *byte);
	++link->rxUnits;
	return true;
}

static bool _portPeek(void* context, uint8_t* byte) {
	struct GBVCLink* link = context;
	return udsSessionPeekUnit(&link->joiner.session, byte);
}

static size_t _portWaiting(void* context) {
	struct GBVCLink* link = context;
	return udsSessionUnitsWaiting(&link->joiner.session);
}

static void _portTrace(void* context, const char* line) {
	_trace(context, "%s", line);
}

static void _logStates(struct GBVCLink* link) {
	if (link->joiner.useRadio) {
		struct UDSAirRadio* radio = &link->joiner.radio;
		if ((int) radio->state != link->lastRadio) {
			link->lastRadio = (int) radio->state;
			if (radio->state == UDS_AIR_BOOTING) {
				mLOG(GB_SIO, INFO, "Virtual Console: waiting for the ESP32 board to start");
			} else if (radio->state == UDS_AIR_READY) {
				mLOG(GB_SIO, INFO, "Virtual Console: ESP32 radio up (firmware %u.%u), scanning for a 3DS host", radio->esp.info.major,
				     radio->esp.info.minor);
			} else if (radio->state == UDS_AIR_FAILED) {
				mLOG(GB_SIO, ERROR, "Virtual Console: ESP32 radio failed: %s", radio->error);
			}
			_trace(link, "radio state %d %s", (int) radio->state, radio->state == UDS_AIR_FAILED ? radio->error : "");
		}
		uint32_t now = link->joiner.nowMs;
		if (radio->state == UDS_AIR_READY && now - link->lastRadioStatsMs >= 5000) {
			link->lastRadioStatsMs = now;
			_trace(link, "radio: beacons %u, sent %u (failed %u), delivered %u, dropped: no key %u, decrypt %u, repeats %u, other %u",
			       radio->beaconsSeen, radio->framesSent, radio->txFailed, radio->framesReceived, radio->droppedNoKey,
			       radio->droppedDecrypt, radio->droppedReplay, radio->droppedOther);
		}
	}
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

static const char* const kWirePhases[] = {"down", "role", "idle", "sync", "menu", "pass"};

// A line every 5 s in the trace, so a quiet stretch can be told from a trace that simply ended: where the ROM is, which hook
// ran last and how long ago, whether a transfer is waiting, and the unit counts.
static void _heartbeat(struct GBVCLink* link, uint32_t now) {
	if (!link->trace || now - link->lastHeartbeatMs < 5000) {
		return;
	}
	link->lastHeartbeatMs = now;
	struct SM83Core* cpu = link->core->cpu;
	if (link->wireMode) {
		_trace(link, "heartbeat: pc %02X:%04X map %02X linkstate %02X conn %02X, wire %s, exchanges %u, units sent %u received %u (%u waiting)",
		       link->gb->memory.currentBank, cpu->pc, _read8(link, link->game->curMap), _read8(link, link->game->linkState),
		       _read8(link, link->game->connectionStatus), kWirePhases[link->wire.phase], link->wireExchanges, link->txUnits,
		       link->rxUnits, (unsigned) udsSessionUnitsWaiting(&link->joiner.session));
		return;
	}
	_trace(link, "heartbeat: pc %02X:%04X map %02X linkstate %02X conn %02X, last hook %s %.1f s ago, %s, transfer %s, units sent %u received %u (%u waiting)",
	       link->gb->memory.currentBank, cpu->pc, _read8(link, link->game->curMap), _read8(link, link->game->linkState),
	       _read8(link, link->game->connectionStatus), link->lastHookName ? link->lastHookName : "none",
	       link->lastHookName ? (now - link->lastHookMs) / 1000.0 : 0.0, link->started ? "link up" : "link down",
	       link->cable.txPending ? "waiting" : "idle", link->txUnits, link->rxUnits,
	       (unsigned) udsSessionUnitsWaiting(&link->joiner.session));
}

// Transfer completion -------------------------------------------------------------------------------------------------

static void _finishTransfer(struct GBVCLink* link, uint8_t byte) {
	struct GBSIO* sio = &link->gb->sio;
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
	_heartbeat(link, now);

	if (!link->started && udsJoinerReady(&link->joiner)) {
		link->started = true;
		udsCableBegin(&link->cable);
		mLOG(GB_SIO, INFO, "Virtual Console: link up");
	} else if (link->started && !_ready(link)) {
		// The host went away (bye, silence). Let the game's pending transfer finish on an idle line.
		link->started = false;
		mLOG(GB_SIO, INFO, "Virtual Console: link lost");
		_trace(link, "link lost");
		if (udsCableLost(&link->cable)) {
			_finishTransfer(link, 0xFF);
		}
	}

	if (link->started) {
		uint8_t byte;
		if (udsCablePoll(&link->cable, now, &byte)) {
			_finishTransfer(link, byte);
		}
	}

	mTimingSchedule(timing, &link->event, GBVC_POLL_CYCLES - cyclesLate);
}

// Wire mode: the cartridge is the master and the front end (uds-wire.c) is its slave ----------------------------------------

static bool _wirePortReady(void* context) {
	const struct GBVCLink* link = context;
	return link->joiner.sessionActive && link->joiner.session.state == UDS_STATE_JOINED;
}

static void _wirePoll(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBVCLink* link = context;
	uint32_t now = _nowMs() - link->startMs;
	udsJoinerPoll(&link->joiner, now);
	_logStates(link);
	_heartbeat(link, now);
	udsWirePoll(&link->wire, now);
	if ((int) link->wire.phase != link->lastWirePhase) {
		if (link->lastWirePhase < 0 || link->wire.phase == UDS_WIRE_ROLE || link->wire.phase == UDS_WIRE_DOWN) {
			mLOG(GB_SIO, INFO, "Virtual Console: wire %s", kWirePhases[link->wire.phase]);
		}
		_trace(link, "wire phase: %s", kWirePhases[link->wire.phase]);
		link->lastWirePhase = (int) link->wire.phase;
	}
	link->started = link->wire.phase != UDS_WIRE_DOWN;
	mTimingSchedule(timing, &link->event, GBVC_POLL_CYCLES - cyclesLate);
}

// The cartridge started a transfer as master: its byte goes to the front end, the slave's reply is what the built-in shifter
// will shift in. A transfer the cartridge arms as slave (external clock) is never clocked; the slave never clocks anything.
static uint8_t _wireWriteSC(struct GBSIODriver* driver, uint8_t value) {
	struct GBVCLink* link = (struct GBVCLink*) driver;
	if ((value & 0x81) == 0x81) {
		uint32_t now = _nowMs() - link->startMs;
		uint8_t master = link->gb->memory.io[GB_REG_SB];
		uint8_t reply = udsWirePreload(&link->wire);
		link->gb->sio.pendingSB = reply;
		udsWireExchanged(&link->wire, now, master);
		++link->wireExchanges;
		if (link->trace && link->wireExchanges <= 8000) {
			_trace(link, "wire %02X -> %02X [%s]", master, reply, kWirePhases[link->wire.phase]);
		}
	}
	return value;
}

// Board mode: the wrapper runs on the ESP32 board, this ROM is the cartridge ----------------------------------------------------

enum BoardState {
	BOARD_BOOT, // the board resets when its port opens: Hello until it answers
	BOARD_KEY, // does it hold the 3DS key? (if not, the key file is stored on it)
	BOARD_START, // GB_START for this game
	BOARD_RUNNING, // every transfer goes to the board
	BOARD_FAILED,
};

#define BOARD_BOOT_MS 15000
#define BOARD_TRANSFER_MS 250

static const char* const kRoomStates[] = {"scan", "auth", "eapol", "joined"};
static const char* const kSessionStates[] = {"idle", "setup", "joined", "closed"};

static void _boardFail(struct GBVCLink* link, const char* why) {
	link->boardState = BOARD_FAILED;
	mLOG(GB_SIO, ERROR, "Virtual Console (board): %s", why);
	_trace(link, "board: %s", why);
}

static void _boardLog(void* context, const char* text, size_t length) {
	_trace(context, "board: %.*s", (int) length, text);
}

static void _boardState(void* context, const uint8_t state[5]) {
	struct GBVCLink* link = context;
	link->boardPhase = state[2];
	_trace(link, "board state: room %s, session %s, wire %s, Gen %u, channel %u", state[0] < 4 ? kRoomStates[state[0]] : "?",
	       state[1] < 4 ? kSessionStates[state[1]] : "none", state[2] <= UDS_WIRE_PASS ? kWirePhases[state[2]] : "?", state[3], state[4]);
	if (state[1] == UDS_STATE_JOINED && state[2] == UDS_WIRE_ROLE) {
		mLOG(GB_SIO, INFO, "Virtual Console (board): link up");
	}
}

static void _boardStats(void* context, const uint32_t stats[UDS_ESP32_GB_STATS]) {
	_trace(context, "board stats: beacons %u, frames sent %u received %u, dropped decrypt %u replay %u other %u, tx failed %u, units sent %u received %u, transfers %u",
	       (unsigned) stats[0], (unsigned) stats[1], (unsigned) stats[2], (unsigned) stats[3], (unsigned) stats[4], (unsigned) stats[5],
	       (unsigned) stats[6], (unsigned) stats[7], (unsigned) stats[8], (unsigned) stats[9]);
}

// Brings the board up: called from the poll until it runs. The waits inside are short (the board answers in milliseconds).
static void _boardStep(struct GBVCLink* link, uint32_t now) {
	switch (link->boardState) {
	case BOARD_BOOT:
		if (link->esp.info.valid) {
			_trace(link, "board: firmware %u.%u, protocol %u", link->esp.info.major, link->esp.info.minor, link->esp.info.proto);
			if (!udsEsp32HasGbWrapper(&link->esp)) {
				_boardFail(link, "the board's firmware has no Game Boy wrapper: flash esp32-uds-bridge 1.4 or later (uds-esp32-setup does it)");
				return;
			}
			link->boardState = BOARD_KEY;
		} else if (now - link->boardSinceMs > BOARD_BOOT_MS) {
			_boardFail(link, "the board did not answer Hello: it must run Azahar's esp32-uds-bridge firmware");
		} else if (!link->boardHelloMs || now - link->boardHelloMs >= 500) {
			udsEsp32SendHello(&link->esp);
			link->boardHelloMs = now ? now : 1;
		}
		return;
	case BOARD_KEY: {
		int present = udsEsp32KeyStatus(&link->esp, 1000);
		if (present < 0) {
			_boardFail(link, "the board did not answer the key query");
			return;
		}
		if (!present) {
			// Like GB-Link's web client storing the Switch keys: the key file's UDS key is written to the board once, and stays there.
			uint8_t key[16];
			enum UDSKeyStatus status = udsKeyFileLoad(link->keyPath, key);
			if (!udsKeyStatusOk(status)) {
				char why[200];
				snprintf(why, sizeof(why), "the board holds no 3DS UDS key, and the key file cannot supply one (%s): choose it in Settings > BIOS",
				         udsKeyStatusText(status));
				_boardFail(link, why);
				return;
			}
			bool stored = udsEsp32SetKey(&link->esp, UDS_ESP32_KEY_SLOT_DATA, key, 3000);
			memset(key, 0, sizeof(key));
			if (!stored) {
				_boardFail(link, "the board did not store the UDS key");
				return;
			}
			mLOG(GB_SIO, INFO, "Virtual Console (board): stored the 3DS UDS key on the board");
			_trace(link, "board: UDS key stored on the board");
		}
		link->boardState = BOARD_START;
		return;
	}
	case BOARD_START: {
		int32_t result = udsEsp32GbStart(&link->esp, link->trace != NULL, link->boardTitle, link->name, 2000);
		if (result != 0) {
			char why[200];
			snprintf(why, sizeof(why), "the board's wrapper did not start: %s", udsEsp32GbStartText(result));
			_boardFail(link, why);
			return;
		}
		link->boardState = BOARD_RUNNING;
		mLOG(GB_SIO, INFO, "Virtual Console (board): %s, the wrapper runs on the ESP32; looking for a 3DS", link->game->title);
		_trace(link, "board: wrapper started for %s", link->boardTitle);
		return;
	}
	default:
		return;
	}
}

static void _boardPoll(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBVCLink* link = context;
	uint32_t now = _nowMs() - link->startMs;
	if (link->boardState != BOARD_FAILED) {
		if (!udsEsp32Poll(&link->esp)) {
			_boardFail(link, "the serial port failed (is the board still plugged in?)");
		} else {
			_boardStep(link, now);
		}
	}
	if (link->trace && now - link->lastHeartbeatMs >= 5000) {
		link->lastHeartbeatMs = now;
		struct SM83Core* cpu = link->core->cpu;
		_trace(link, "heartbeat: pc %02X:%04X map %02X linkstate %02X conn %02X, board %d, wire %s, transfers %u, timeouts %u",
		       link->gb->memory.currentBank, cpu->pc, _read8(link, link->game->curMap), _read8(link, link->game->linkState),
		       _read8(link, link->game->connectionStatus), link->boardState,
		       link->boardPhase <= UDS_WIRE_PASS ? kWirePhases[link->boardPhase] : "?", link->wireExchanges, link->boardTimeouts);
	}
	mTimingSchedule(timing, &link->event, GBVC_POLL_CYCLES - cyclesLate);
}

// The cartridge started a transfer as master: the board answers with the slave's byte for this same transfer. The emulator waits for
// it (a few milliseconds over USB), so the game sees a slave that is always ready, as a real one is. Until the board's wrapper runs, the
// line is idle (FF).
static uint8_t _boardWriteSC(struct GBSIODriver* driver, uint8_t value) {
	struct GBVCLink* link = (struct GBVCLink*) driver;
	if ((value & 0x81) == 0x81) {
		uint8_t master = link->gb->memory.io[GB_REG_SB];
		uint8_t reply = UDS_WIRE_IDLE_LINE;
		uint8_t phase = link->boardPhase;
		if (link->boardState == BOARD_RUNNING) {
			if (!udsEsp32GbTransfer(&link->esp, master, &reply, &phase, BOARD_TRANSFER_MS)) {
				reply = UDS_WIRE_IDLE_LINE;
				++link->boardTimeouts;
				_trace(link, "board: no answer to a transfer (%02X)", master);
			} else {
				link->boardPhase = phase;
			}
		}
		link->gb->sio.pendingSB = reply;
		++link->wireExchanges;
		if (link->trace && link->wireExchanges <= 8000) {
			_trace(link, "wire %02X -> %02X [%s]", master, reply, phase <= UDS_WIRE_PASS ? kWirePhases[phase] : "?");
		}
	}
	return value;
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
		udsCableTransfer(&link->cable, link->gb->memory.io[GB_REG_SB]);
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

// Serial_SyncAndExchangeNybble. The exchange itself is uds-cable.c's; what is left here is reading the nybble the ROM wants
// to send and leaving the registers and RAM as the ROM's own routine would.
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
	link->hle = HLE_NONE;
}

static void _hookSync(struct GBVCLink* link) {
	const struct VCGame* game = link->game;
	uint32_t now = _nowMs() - link->startMs;
	int nybble;

	if (link->hle != HLE_SYNC) {
		_saveRegs(link);
		link->hle = HLE_SYNC;
	}
	enum UDSCableStatus status = udsCableSync(&link->cable, _read8(link, game->sendNybble), now, &nybble);
	if (status == UDS_CABLE_TIMED_OUT) {
		// The ROM's own way out is the counter at FFFF ("link closed because of inactivity").
		_write8(link, game->unknownCounter, 0xFF);
		_write8(link, game->unknownCounter + 1, 0xFF);
	}
	if (status != UDS_CABLE_PENDING) {
		_syncFinish(link, nybble);
		return;
	}
	_yield(link, game->syncEntry.addr);
}

// Serial_ExchangeLinkMenuSelection (the exchange is uds-cable.c's).
static void _menuFinish(struct GBVCLink* link, uint8_t first, uint8_t second) {
	const struct VCGame* game = link->game;
	struct SM83Core* cpu = link->core->cpu;
	_write8(link, game->menuReceive, first);
	_write8(link, game->menuReceive + 1, second);
	cpu->bc = link->savedBC;
	cpu->de = link->savedDE;
	cpu->hl = link->savedHL;
	_setPC(link, game->menuReturn);
	link->hle = HLE_NONE;
}

static void _hookMenu(struct GBVCLink* link) {
	const struct VCGame* game = link->game;
	uint32_t now = _nowMs() - link->startMs;
	uint8_t first, second;

	if (link->hle != HLE_MENU) {
		_saveRegs(link);
		link->hle = HLE_MENU;
	}
	if (udsCableMenu(&link->cable, _read8(link, game->menuSend), now, &first, &second) == UDS_CABLE_DONE) {
		_menuFinish(link, first, second);
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
	link->lastHookMs = _nowMs() - link->startMs;
	if (info->pointId == link->fakeBeginId) {
		link->lastHookName = "fake_connection_status";
		_hookFakeBegin(link);
	} else if (info->pointId == link->fakeEndId) {
		link->lastHookName = "wireless_prompt";
		_hookFakeEnd(link);
	} else if (info->pointId == link->syncId) {
		link->lastHookName = "nybble_sync";
		_hookSync(link);
	} else if (info->pointId == link->menuId) {
		link->lastHookName = "link_menu";
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
                                const struct GBVCLinkConfig* config) {
	if (!core || core->platform(core) != mPLATFORM_GB) {
		return NULL;
	}
	char title[VC_TITLE_KEY + 1];
	_headerTitle(core, title);
	// Wire mode (the ROM is a cartridge on a link cable, nothing is hooked) is the default: it carries Red, Blue, Yellow and Gen 2.
	// MGBA_VCLINK_WIRE=0 selects the older hook mode (Gen 1 only), which needs the debugger.
	bool wire = true;
	const char* wireEnv = getenv("MGBA_VCLINK_WIRE");
	if (wireEnv && *wireEnv) {
		wire = *wireEnv != '0';
	}
	if (config->air == GBVC_AIR_BOARD) {
		wire = true; // the board's wrapper is the wire front end; nothing is hooked here
	}
	const struct VCGame* game = _findGame(core, title);
	if (!game) {
		size_t i;
		for (i = 0; i < sizeof(sGen2Games) / sizeof(sGen2Games[0]); ++i) {
			if (strncmp(title, sGen2Games[i].title, VC_TITLE_KEY) == 0) {
				if (!wire) {
					mLOG(GB_SIO, WARN, "Virtual Console: Gen 2 (%s) works only in wire mode, which is the default (is MGBA_VCLINK_WIRE set to 0?)", title);
					return NULL;
				}
				game = &sGen2Games[i];
				break;
			}
		}
		if (!game) {
			mLOG(GB_SIO, WARN, "Virtual Console: this game is not one the wrapper knows (header title \"%s\"; Red, Blue and Yellow are)",
			     title);
			return NULL;
		}
	}
	if (!wire && !debugger) {
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
	link->lastRadio = -1;
	link->fakeBeginId = link->fakeEndId = link->syncId = link->menuId = -1;
	link->wireMode = wire;
	link->lastWirePhase = -1;
	if (wire) {
		udsWireInit(&link->wire, &(struct UDSUnitPort) {
			.context = link,
			.ready = _wirePortReady,
			.queue = _portQueue,
			.flush = _portFlush,
			.pop = _portPop,
			.peek = _portPeek,
			.waiting = _portWaiting,
			.trace = _portTrace,
		});
		udsWireSetGeneration(&link->wire, game->gen2 ? 2 : 1);
	}
	udsCableInit(&link->cable, &(struct UDSUnitPort) {
		.context = link,
		.ready = _portReady,
		.queue = _portQueue,
		.flush = _portFlush,
		.pop = _portPop,
		.peek = _portPeek,
		.waiting = _portWaiting,
		.trace = _portTrace,
	});

	const char* dir = getenv("MGBA_VCLINK_TRACE");
	if (config->tracePath && *config->tracePath) {
		link->trace = fopen(config->tracePath, "w");
	} else if (dir && *dir) {
		char path[512];
		snprintf(path, sizeof(path), "%s/vclink_%u.txt", dir, (unsigned) time(NULL));
		link->trace = fopen(path, "w");
	}

	if (config->air == GBVC_AIR_BOARD) {
		link->boardMode = true;
		link->boardState = BOARD_BOOT;
		link->boardPhase = UDS_WIRE_DOWN;
		memcpy(link->name, name, sizeof(link->name));
		strncpy(link->boardTitle, title, sizeof(link->boardTitle) - 1);
		if (config->keyPath) {
			strncpy(link->keyPath, config->keyPath, sizeof(link->keyPath) - 1);
		}
		struct UDSEsp32Handlers handlers = {
			.context = link, .log = _boardLog, .gbState = _boardState, .gbStats = _boardStats,
		};
		if (!udsEsp32Open(&link->esp, config->portName, &handlers)) {
			mLOG(GB_SIO, ERROR, "Virtual Console (board): no ESP32 board found%s%s", config->portName && *config->portName ? " on " : "",
			     config->portName && *config->portName ? config->portName : "");
			GBVCLinkDestroy(link);
			return NULL;
		}
		link->boardSinceMs = _nowMs() - link->startMs;
		link->d.init = _driverInit;
		link->d.deinit = _driverDeinit;
		link->d.writeSB = _driverWriteSB;
		link->d.writeSC = _boardWriteSC;
		GBSIOSetDriver(&link->gb->sio, &link->d);
		link->attachedDriver = true;
		link->event.context = link;
		link->event.name = "GB VC board";
		link->event.callback = _boardPoll;
		link->event.priority = 0x40;
		mTimingSchedule(&link->gb->timing, &link->event, GBVC_POLL_CYCLES);
		mLOG(GB_SIO, INFO, "Virtual Console (board): %s, the wrapper runs on the ESP32 board%s%s", game->title,
		     config->portName && *config->portName ? " on " : "", config->portName && *config->portName ? config->portName : "");
		_trace(link, "created for %s, board mode", game->title);
		return link;
	}

	if (config->air == GBVC_AIR_RADIO) {
		char error[200];
		if (!udsJoinerOpenRadio(&link->joiner, name, config->portName, config->keyPath, error, sizeof(error))) {
			mLOG(GB_SIO, ERROR, "Virtual Console: cannot start the ESP32 radio: %s", error);
			GBVCLinkDestroy(link);
			return NULL;
		}
	} else if (!udsJoinerOpen(&link->joiner, name, config->listenPort, config->sendPort)) {
		mLOG(GB_SIO, ERROR, "Virtual Console: cannot listen for Azahar's UDS bridge (is another mGBA using the port?)");
		GBVCLinkDestroy(link);
		return NULL;
	}

	link->joiner.leaveWithHost = game->gen2;

	// On the radio only hosts of this generation's titles are joined (the beacon carries the comm id, which differs between versions).
	// The Azahar bridge has one possible host, so it is joined whatever title it runs: any version can be tested against any other.
	if (config->air == GBVC_AIR_BRIDGE) {
		link->joiner.room.wantCommId = 0;
		_trace(link, "bridge: joining whatever Azahar hosts");
	} else {
		link->joiner.room.wantCommId = game->commFamily ? game->commFamily : VC_GEN1_COMM_FAMILY;
		link->joiner.room.wantCommMask = VC_COMM_MASK;
		_trace(link, "looking for a host with comm id %08X/%08X", (unsigned) link->joiner.room.wantCommId, (unsigned) link->joiner.room.wantCommMask);
	}

	link->d.init = _driverInit;
	link->d.deinit = _driverDeinit;
	link->d.writeSB = _driverWriteSB;
	link->d.writeSC = wire ? _wireWriteSC : _driverWriteSC;

	if (wire) {
		GBSIOSetDriver(&link->gb->sio, &link->d);
		link->attachedDriver = true;
		link->event.context = link;
		link->event.name = "GB VC wire";
		link->event.callback = _wirePoll;
		link->event.priority = 0x40;
		mTimingSchedule(&link->gb->timing, &link->event, GBVC_POLL_CYCLES);
		mLOG(GB_SIO, INFO, "Virtual Console: %s in wire mode (the ROM is the master of a link cable; no ROM hooks)", game->title);
		_trace(link, "created for %s, wire mode", game->title);
		return link;
	}

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

	if (config->air == GBVC_AIR_RADIO) {
		mLOG(GB_SIO, INFO, "Virtual Console: %s over the ESP32 radio%s%s", game->title,
		     config->portName && *config->portName ? " on " : "", config->portName && *config->portName ? config->portName : "");
	} else {
		mLOG(GB_SIO, INFO, "Virtual Console: %s, waiting for Azahar's UDS bridge on 127.0.0.1:%u (sending to %u)", game->title,
		     link->joiner.udp.listenPort, link->joiner.udp.sendPort);
	}
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
	if (link->boardMode) {
		if (link->boardState == BOARD_RUNNING) {
			udsEsp32GbStop(&link->esp, 500);
		}
		udsEsp32Close(&link->esp);
	} else {
		udsJoinerClose(&link->joiner);
	}
	if (link->trace) {
		_trace(link, "link stopped (units sent %u, received %u)", link->txUnits, link->rxUnits);
		fclose(link->trace);
	}
	mLOG(GB_SIO, INFO, "Virtual Console: link stopped");
	free(link);
}

void GBVCLinkTraceNote(struct GBVCLink* link, const char* text) {
	if (link) {
		_trace(link, "app: %s", text);
	}
}
