/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/rfu-wrapper-air.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/internal/gba/sio/rfu.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Layout of this file:
 *   1. wire helpers        LLSF headers, 14-byte command slots
 *   2. block receive/send  RFU block fragments (12 bytes) and the child's ack-gated block sender
 *   3. barriers            standby (0x6600) and close (0x5F00) rounds
 *   4. NI handshake        the child's game-data send and the acks of the host's
 *   5. cable queue         what the virtual master says to the game
 *   6. translator          host pulls, blocks, keys, barriers, LinkPlayer patching
 *   7. air                 backend events, one child frame per host frame
 */

enum {
	// RFU opcodes (high byte of a slot's first word; pokefirered link_rfu.h)
	RFUCMD_MASK = 0xFF00,
	RFUCMD_READY_CLOSE_LINK = 0x5F00,
	RFUCMD_READY_EXIT_STANDBY = 0x6600,
	RFUCMD_SEND_PLAYER_IDS = 0x7700,
	RFUCMD_SEND_BLOCK_INIT = 0x8800,
	RFUCMD_SEND_BLOCK = 0x8900,
	RFUCMD_SEND_BLOCK_REQ = 0xA100,
	RFUCMD_SEND_HELD_KEYS = 0xBE00,
	RFUCMD_DISCONNECT = 0xED00,

	// Cable commands (pokeruby link.h)
	LINKCMD_SEND_LINK_TYPE = 0x2222,
	LINKCMD_READY_EXIT_STANDBY = 0x2FFE,
	LINKCMD_READY_CLOSE_LINK = 0x5FFF,
	LINKCMD_CONT_BLOCK = 0x8888,
	LINKCMD_INIT_BLOCK = 0xBBBB,
	LINKCMD_SEND_HELD_KEYS = 0xCAFE,
	LINKCMD_SEND_BLOCK_REQ = 0xCCCC,

	// librfu link-layer sub-frame states
	LCOM_NULL = 0,
	LCOM_NI_START = 1,
	LCOM_NI = 2,
	LCOM_NI_END = 3,
	LCOM_UNI = 4,

	SLOT_BYTES = 14,
	FRAG_BYTES = 12,
	HOST_FRAME_BYTES = 3 + 5 * SLOT_BYTES,
	MAX_FRAGS = 24,
	MAX_BLOCK_BYTES = MAX_FRAGS * FRAG_BYTES,
	CMD_WORDS = RFU_WRAPPER_CMD_LENGTH,

	LINK_PLAYER_BLOCK_SIZE = 60,
	LP_LINK_TYPE_OFFSET = 16 + 0x14,
	LP_VERSION_OFFSET = 16,
	LP_TRAINER_ID_OFFSET = 16 + 4,
	LP_NAME_OFFSET = 16 + 8,
	LP_BUFFER_SIZE = 200,
	CARD_SIZE = 100,
	CARD_VERSION_OFFSET = 0x38,

	OWNER_FLAG = 0x80,
	CABLE_QUEUE = 96,
	CHILD_QUEUE = 8,
	HELD_BLOCKS = 8,
	ANNOUNCE_PACKETS = 30,
	// After the LinkPlayer exchange the game still has to leave its link setup (pokeruby trade.c states 2-5 and the first
	// party-exchange cases). A block that arrives before it does is counted, then wiped by its
	// ResetBlockReceivedFlags, and the game ends up one exchange behind for good. Real cable partners cannot be that early.
	SESSION_SETTLE_PACKETS = 150,
	BARRIER_EMITS = 6, // new standby frames per count before going quiet (the real child re-emits every >60 frames)
	INITIATE_TIMEOUT = 600,
	IDLE_TIMEOUT = 90,
	HOST_SILENT_FRAMES = 600,
};

enum {
	AIR_SEARCH,
	AIR_CONNECTING,
	AIR_NI,
	AIR_UNI,
};

enum {
	EXPECT_NONE,
	EXPECT_LINK_PLAYER,
	EXPECT_PULL,
	EXPECT_CARD, // the game's trainer card, asked for early (see _requestEarlyCard)
};

enum {
	BAR_IDLE,
	BAR_STANDBY,
	BAR_CLOSE,
};

enum {
	PURPOSE_NONE,
	PURPOSE_ROUND0,
	PURPOSE_GAME_STANDBY, // the game said 2FFE: answer it on the cable once the leader has passed the round
	PURPOSE_GAME_CLOSE,
	PURPOSE_EXIT_CLOSE, // the game leaves the room for good: a real READY_CLOSE_LINK, then the wireless link ends
};

// --- 2. block receive/send ---------------------------------------------------------------------------------------------

struct Recv {
	unsigned count;
	uint32_t flags;
	bool receiving;
	bool done;
	int lastIndex;
	uint8_t buf[MAX_BLOCK_BYTES];
};

enum {
	SEND_INIT,
	SEND_STREAM,
	SEND_HOLD,
	SEND_DONE,
};

struct Send {
	bool active;
	int state;
	unsigned count;
	unsigned index;
	unsigned initSends;
	unsigned holdSends;
	unsigned rr;
	uint8_t data[MAX_BLOCK_BYTES];
	bool isLinkPlayer;
	bool isCard;
};

struct ChildBlock {
	unsigned size;
	bool isLinkPlayer;
	bool isCard;
	uint8_t data[MAX_BLOCK_BYTES];
};

struct Barrier {
	int mode;
	bool initiated;
	int hostCount; // -1: none seen
	unsigned localCount;
	unsigned sinceHost;
	unsigned sinceInitiate;
	int burstFor;
	unsigned burstN;
	unsigned rounds;
};

struct NI {
	int state; // 0 start, 1 sending, 2 last, 3 null, 4 done
	unsigned phase;
	uint8_t n[4];
	unsigned now[4];
	int remain;
	uint8_t src[26];
	uint8_t header[7];
};

enum {
	KEY_QUEUE = 48,
	LINK_KEY_CODE_EMPTY = 0x11,
	LINK_KEY_CODE_EXIT_ROOM = 0x17,
};

struct KeyQueue {
	uint8_t codes[KEY_QUEUE];
	unsigned head;
	unsigned count;
};

struct Air {
	struct GBASIORFUWrapper* w;
	struct GBASIORFU rfu; // headless: only its event queue is used
	struct GBASIORFUBackend* backend;
	char backendName[16];

	int link;
	uint16_t hostId;
	unsigned linkFrames;
	unsigned silentFrames;

	// child link layer
	struct NI ni;
	bool niStarted;
	bool hostUni;
	bool hostNiDone;
	uint8_t tag;
	uint8_t keyCount;
	struct Recv rx0; // the leader's own blocks
	struct Recv rx1; // the leader's reflection of ours (our block's ack)
	struct Send send;
	struct ChildBlock childQueue[CHILD_QUEUE];
	unsigned childHead;
	unsigned childCount;
	struct Barrier bar;
	unsigned lastHostOp;
	unsigned hostFrames;
	unsigned childFrames;

	// translator state that outlives a cable link
	bool haveRubyLP;
	bool haveHostLP;
	uint8_t rubyLP[LINK_PLAYER_BLOCK_SIZE];
	uint8_t hostLP[LINK_PLAYER_BLOCK_SIZE];
	bool lpSentToHost;
	bool lpSendDone;
	bool round0Started;
	bool keysActive;
	// Held-key codes are events, one per frame (a READY press is a single frame), so they travel through queues: a
	// "latest value" would lose any code that is replaced before the other side's next frame.
	struct KeyQueue rubyKeys; // game -> leader
	struct KeyQueue hostKeys; // leader -> game
	int lastHostKeyCount;
	int purpose;
	unsigned closeRoundsLeft;
	unsigned closeCount; // closes the game has asked for: the first leaves the room, the later ones end the trade menu
	bool cancelPending;  // the leader announced BOTH_CANCEL_TRADE: the trade menu ends and both games go back to the room
	bool cancelClose;    // the close being answered now is that exit
	bool cancelReturn;   // the room is being re-entered: the game's next standby is answered locally, then keys resume
	// The trainer card is exchanged with the game at once, before the leader pulls it (see _requestEarlyCard).
	bool cardArmed;
	unsigned cardAt;
	bool cardRequested;
	bool haveRubyCard;
	bool cardSynth;       // the game was given a card made from the leader's LinkPlayer; the leader's own is dropped
	bool cardQueued;
	bool cardPullWaiting;
	bool cardSendDone;
	bool pendingGameStandby;
	uint8_t rubyCard[CARD_SIZE];
	bool exitKeySeen;    // either side pressed EXIT_ROOM since the room was entered
	bool hostClosedSeen; // the leader already sent READY_CLOSE_LINK
	bool roomClosed;     // held keys only exist in the room; after the first close they must never be sent again
	bool pendingPurposeStandby;
	unsigned standbyRoundsSeen;

	// What the leader sent while the game's cable link was not ready (closed, or still exchanging LinkPlayers): held
	// until the session is up, so a block never lands in the middle of the link setup.
	struct ChildBlock held[HELD_BLOCKS];
	unsigned heldCount;
	int pendingPull; // -1: none

	// cable session state (reset whenever the game opens its link)
	unsigned cablePackets;
	unsigned readyAt; // cable packet count from which blocks and pulls may be forwarded to the game
	bool announced;
	int expect;
	int pullType;
	bool sessionRubyLP;
	bool p0LPDelivered;
	unsigned rxSize;
	unsigned rxPos;
	uint8_t rxBuf[MAX_BLOCK_BYTES];
	uint16_t cq[CABLE_QUEUE][CMD_WORDS];
	unsigned cqHead;
	unsigned cqCount;
	unsigned sessions;
};

#define AIRLOG(air, ...) GBASIOCableTrace((air)->w->d.p, "AIR " __VA_ARGS__)

static void _keyPush(struct KeyQueue* q, uint8_t code) {
	if (q->count >= KEY_QUEUE) {
		q->head = (q->head + 1) % KEY_QUEUE; // drop the oldest
		--q->count;
	}
	q->codes[(q->head + q->count) % KEY_QUEUE] = code;
	++q->count;
}

// The next code, or "no key" when the queue is empty.
static uint8_t _keyPop(struct KeyQueue* q) {
	if (!q->count) {
		return LINK_KEY_CODE_EMPTY;
	}
	uint8_t code = q->codes[q->head];
	q->head = (q->head + 1) % KEY_QUEUE;
	--q->count;
	return code;
}

static unsigned _fragCount(unsigned bytes) {
	unsigned count = (bytes + FRAG_BYTES - 1) / FRAG_BYTES;
	return count ? count : 1;
}

// Sizes of the blocks the cable club and trade menus move, by fragment count (link.c sBlockRequestLookupTable and the
// 20-byte link data). Counts are unique, so the count alone says what a block is.
static unsigned _sizeFromCount(unsigned count) {
	switch (count) {
	case 17:
		return 200;
	case 9:
		return 100;
	case 19:
		return 220;
	case 4:
		return 40;
	case 2:
		return 20;
	}
	return count * FRAG_BYTES;
}

static unsigned _sizeFromRequest(unsigned type) {
	switch (type) {
	case 2:
		return 100;
	case 3:
		return 220;
	case 4:
		return 40;
	default:
		return 200;
	}
}

static uint16_t _le16(const uint8_t* p) {
	return p[0] | (p[1] << 8);
}

static void _put16(uint8_t* p, uint16_t v) {
	p[0] = v;
	p[1] = v >> 8;
}

static void _slotToWords(const uint8_t* slot, uint16_t words[7]) {
	unsigned i;
	for (i = 0; i < 7; ++i) {
		words[i] = _le16(&slot[i * 2]);
	}
}

static bool _slotIdle(const uint8_t* slot) {
	unsigned i;
	for (i = 0; i < SLOT_BYTES; ++i) {
		if (slot[i]) {
			return false;
		}
	}
	return true;
}

static void _recvReset(struct Recv* r) {
	memset(r, 0, sizeof(*r));
	r->lastIndex = -1;
}

static void _recvInit(struct Recv* r, unsigned count) {
	if (count == 0 || count > MAX_FRAGS) {
		return;
	}
	// A repeat of the INIT of the block being received must not wipe its fragments.
	if (!r->receiving || r->done || count != r->count) {
		_recvReset(r);
		r->count = count;
		r->receiving = true;
	}
}

static bool _recvBlock(struct Recv* r, unsigned index, const uint8_t* frag) {
	if (!r->receiving || index >= r->count) {
		return false;
	}
	bool wasDone = r->done;
	r->lastIndex = index;
	r->flags |= 1u << index;
	memcpy(&r->buf[index * FRAG_BYTES], frag, FRAG_BYTES);
	if (r->flags == ((1u << r->count) - 1)) {
		r->done = true;
	}
	return r->done && !wasDone;
}

static void _feedRecv(struct Recv* r, const uint16_t words[7], const uint8_t* slot, bool* completed) {
	unsigned op = words[0] & RFUCMD_MASK;
	*completed = false;
	if (op == RFUCMD_SEND_BLOCK_INIT) {
		_recvInit(r, words[1]);
	} else if (op == RFUCMD_SEND_BLOCK) {
		*completed = _recvBlock(r, words[0] & 0x1F, &slot[2]);
	}
}

static void _sendStart(struct Send* s, const uint8_t* data, unsigned size, bool isLinkPlayer) {
	memset(s, 0, sizeof(*s));
	s->active = true;
	s->state = SEND_INIT;
	s->count = _fragCount(size);
	memcpy(s->data, data, size);
	s->isLinkPlayer = isLinkPlayer;
}

static void _blockWords(const struct Send* s, unsigned index, uint16_t words[7]) {
	words[0] = RFUCMD_SEND_BLOCK | (index & 0x1F);
	unsigned i;
	for (i = 0; i < 6; ++i) {
		words[1 + i] = _le16(&s->data[index * FRAG_BYTES + i * 2]);
	}
}

// One VBlank of the ack-gated child block sender (Rfu_InitBlockSend / SendNextBlock / SendLastBlock / HandleSendFailure).
// Fills the 7-word command, all zero when there is nothing to say.
static void _sendTick(struct Send* s, const struct Recv* ack, uint16_t words[7]) {
	memset(words, 0, 7 * sizeof(uint16_t));
	if (!s->active) {
		return;
	}
	if (s->state == SEND_INIT) {
		++s->initSends;
		// INIT repeats every frame until the leader's reflection shows it armed its receive side.
		if (ack->receiving && ack->count == s->count) {
			s->state = SEND_STREAM;
			s->index = 0;
		} else {
			words[0] = RFUCMD_SEND_BLOCK_INIT;
			words[1] = s->count;
			words[2] = 1 | OWNER_FLAG;
			return;
		}
	}
	if (s->state == SEND_STREAM) {
		unsigned index = s->index;
		_blockWords(s, index, words);
		if (index >= s->count - 1) {
			s->state = SEND_HOLD;
			s->holdSends = 0;
		} else {
			++s->index;
		}
		return;
	}
	if (s->state == SEND_HOLD) {
		++s->holdSends;
		unsigned last = s->count - 1;
		uint32_t full = (1u << s->count) - 1;
		if (ack->lastIndex == (int) last && ack->count == s->count) {
			if (ack->flags == full) {
				s->state = SEND_DONE;
				s->active = false;
				return;
			}
			unsigned missing[MAX_FRAGS];
			unsigned n = 0, i;
			for (i = 0; i < s->count; ++i) {
				if (!((ack->flags >> i) & 1)) {
					missing[n++] = i;
				}
			}
			if (n) {
				s->rr = (s->rr + 1) % n;
				_blockWords(s, missing[s->rr], words);
				return;
			}
		}
		_blockWords(s, last, words);
	}
}

// --- 3. barriers -----------------------------------------------------------------------------------------------------

static void _barrierInit(struct Barrier* b) {
	memset(b, 0, sizeof(*b));
	b->hostCount = -1;
	b->burstFor = -1;
}

static void _barrierInitiate(struct Barrier* b, int kind) {
	if (b->mode == kind) {
		return;
	}
	b->mode = kind;
	b->initiated = true;
	b->hostCount = -1;
	b->sinceHost = 0;
	b->sinceInitiate = 0;
	b->burstFor = -1;
}

// The host's own barrier word. Returns true when a round we started has passed.
static bool _barrierHostStandby(struct Air* air, unsigned count) {
	struct Barrier* b = &air->bar;
	b->sinceHost = 0;
	int prev = b->hostCount;
	b->hostCount = count;
	if (b->initiated && b->mode == BAR_STANDBY) {
		if (count == b->localCount) {
			++b->localCount;
			++b->rounds;
			b->mode = BAR_IDLE;
			b->initiated = false;
			return true;
		}
		return false;
	}
	if (count < b->localCount) {
		return false; // the leader repeating a round we already passed
	}
	if (b->mode != BAR_STANDBY) {
		b->mode = BAR_STANDBY;
		b->initiated = false;
		AIRLOG(air, "barrier: leader 0x6600 count=%u -> mirroring", count);
	} else if (prev >= 0 && (int) count != prev) {
		++b->rounds;
	}
	b->localCount = count;
	b->sinceInitiate = 0;
	return false;
}

static void _barrierHostClose(struct Air* air, unsigned count) {
	struct Barrier* b = &air->bar;
	b->sinceHost = 0;
	b->hostCount = count;
	b->localCount = count;
	if (b->mode != BAR_CLOSE) {
		b->mode = BAR_CLOSE;
		b->initiated = false;
		AIRLOG(air, "barrier: leader 0x5F00 count=%u -> mirroring", count);
	}
}

// Once per host frame. Returns true when a round ended without the leader's echo: the leader stopped broadcasting (its
// round passed) or an initiated round timed out (released so the link cannot hang).
static bool _barrierObserve(struct Air* air, bool sawBarrier) {
	struct Barrier* b = &air->bar;
	if (b->mode == BAR_CLOSE && b->initiated) {
		// Our own close: say it again every 60 frames; give up waiting for the leader's after 5 seconds.
		if (b->sinceInitiate && b->sinceInitiate % 60 == 0) {
			b->burstN = 0;
		}
		if (++b->sinceInitiate > 300) {
			b->mode = BAR_IDLE;
			b->initiated = false;
			AIRLOG(air, "barrier: close unanswered, ending the link anyway");
			return true;
		}
		return false;
	}
	if (b->mode != BAR_STANDBY) {
		return false;
	}
	if (sawBarrier) {
		b->sinceHost = 0;
		b->sinceInitiate = 0;
		return false;
	}
	++b->sinceHost;
	if (b->initiated) {
		// Like the real child, say it again every 60 frames: the leader only listens once its own game reaches the standby.
		if (b->sinceInitiate && b->sinceInitiate % 60 == 0) {
			b->burstN = 0;
		}
		if (++b->sinceInitiate > INITIATE_TIMEOUT) {
			b->mode = BAR_IDLE;
			b->initiated = false;
			AIRLOG(air, "barrier: standby unanswered for %u frames, released (count held at %u)", INITIATE_TIMEOUT,
			       b->localCount);
			return true;
		}
	} else if (b->sinceHost > IDLE_TIMEOUT) {
		++b->localCount;
		++b->rounds;
		b->mode = BAR_IDLE;
		AIRLOG(air, "barrier: leader stopped 0x6600, round passed (count now %u)", b->localCount);
		return true;
	}
	return false;
}

static bool _barrierWant(struct Barrier* b, uint16_t words[7]) {
	if (b->mode == BAR_IDLE) {
		return false;
	}
	if (b->burstFor != (int) b->localCount) {
		b->burstFor = b->localCount;
		b->burstN = 0;
	}
	if (b->burstN >= BARRIER_EMITS) {
		return false;
	}
	++b->burstN;
	memset(words, 0, 7 * sizeof(uint16_t));
	words[0] = b->mode == BAR_STANDBY ? RFUCMD_READY_EXIT_STANDBY : RFUCMD_READY_CLOSE_LINK;
	words[1] = b->localCount;
	return true;
}

// --- 4. NI handshake -------------------------------------------------------------------------------------------------

static uint16_t _childLLSF(unsigned state, unsigned n, unsigned phase, unsigned ack, unsigned size) {
	return ((state & 0xF) << 10) | ((ack & 1) << 9) | ((n & 3) << 7) | ((phase & 3) << 5) | (size & 0x1F);
}

static void _niInit(struct NI* ni, const uint8_t* src) {
	memset(ni, 0, sizeof(*ni));
	memcpy(ni->src, src, 26);
	// The 7-byte NI_START header: dataType 1 (game data), payloadSize 12, dataSize 26.
	ni->header[0] = 1;
	_put16(&ni->header[1], 12);
	ni->header[3] = 26;
	ni->remain = 7;
}

// One child NI sub-frame (rfu_STC_NI_constructLLSF, single pass: the local link neither loses nor reorders frames).
// Returns the frame length, 0 when the transfer is finished.
static unsigned _niNext(struct NI* ni, uint8_t* out) {
	const unsigned payload = 12;
	unsigned i;
	if (ni->state == 0) {
		unsigned size = ni->remain < payload ? ni->remain : payload;
		_put16(out, _childLLSF(LCOM_NI_START, 1, 0, 0, size));
		memcpy(&out[2], ni->header, size);
		ni->state = 1;
		ni->phase = 0;
		for (i = 0; i < 4; ++i) {
			ni->n[i] = 1;
			ni->now[i] = payload * i;
		}
		ni->remain = 26;
		return 2 + size;
	}
	if (ni->state == 1) {
		while (ni->now[ni->phase] >= 26) {
			ni->phase = (ni->phase + 1) % 4;
		}
		unsigned off = ni->now[ni->phase];
		unsigned size = 26 - off < payload ? 26 - off : payload;
		_put16(out, _childLLSF(LCOM_NI, ni->n[ni->phase], ni->phase, 0, size));
		memcpy(&out[2], &ni->src[off], size);
		ni->remain -= size;
		ni->now[ni->phase] += payload << 2;
		ni->phase = (ni->phase + 1) % 4;
		if (ni->remain <= 0) {
			ni->state = 2;
			ni->phase = 0;
		}
		return 2 + size;
	}
	if (ni->state == 2) {
		_put16(out, _childLLSF(LCOM_NI_END, 0, 0, 0, 0));
		ni->state = 3;
		return 2;
	}
	if (ni->state == 3) {
		_put16(out, _childLLSF(LCOM_NULL, 1, 0, 0, 0));
		ni->state = 4;
		return 2;
	}
	return 0;
}

// --- 5. cable queue --------------------------------------------------------------------------------------------------

static bool _cablePush(struct Air* air, const uint16_t cmd[CMD_WORDS]) {
	if (air->cqCount >= CABLE_QUEUE) {
		AIRLOG(air, "cable queue full, command %04X dropped", cmd[0]);
		return false;
	}
	unsigned at = (air->cqHead + air->cqCount) % CABLE_QUEUE;
	memcpy(air->cq[at], cmd, sizeof(air->cq[at]));
	++air->cqCount;
	return true;
}

static void _cablePushCmd(struct Air* air, uint16_t c0, uint16_t c1, uint16_t c2) {
	uint16_t cmd[CMD_WORDS] = { c0, c1, c2, 0, 0, 0, 0, 0 };
	_cablePush(air, cmd);
}

// A block for the game as player 0: INIT_BLOCK, then CONT_BLOCK chunks of seven words.
static void _cablePushBlock(struct Air* air, const uint8_t* data, unsigned size) {
	_cablePushCmd(air, LINKCMD_INIT_BLOCK, size, 0 + 128);
	unsigned pos;
	for (pos = 0; pos < size; pos += (CMD_WORDS - 1) * 2) {
		uint16_t cmd[CMD_WORDS] = { LINKCMD_CONT_BLOCK, 0, 0, 0, 0, 0, 0, 0 };
		unsigned i;
		for (i = 0; i < CMD_WORDS - 1; ++i) {
			unsigned at = pos + i * 2;
			uint8_t lo = at < size ? data[at] : 0;
			uint8_t hi = at + 1 < size ? data[at + 1] : 0;
			cmd[1 + i] = lo | (hi << 8);
		}
		_cablePush(air, cmd);
	}
}

// --- 6. translator ---------------------------------------------------------------------------------------------------

static void _releaseHeld(struct Air* air);
static void _exitClosePassed(struct Air* air);
static void _airResetLink(struct Air* air);
static void _flushPendingStandby(struct Air* air);

static void _childQueuePush(struct Air* air, const uint8_t* data, unsigned size, bool isLinkPlayer) {
	if (air->childCount >= CHILD_QUEUE || size > MAX_BLOCK_BYTES) {
		AIRLOG(air, "child block queue full, %u bytes dropped", size);
		return;
	}
	struct ChildBlock* block = &air->childQueue[(air->childHead + air->childCount) % CHILD_QUEUE];
	memset(block, 0, sizeof(*block));
	memcpy(block->data, data, size);
	block->size = size;
	block->isLinkPlayer = isLinkPlayer;
	++air->childCount;
}

// The LinkPlayer we give the leader: the game's own record presented as a LeafGreen (the leader only checks the two
// "GameFreak inc." magics and a valid version) with link type 0, which is what an FRLG wireless link uses.
static void _buildLinkPlayerForHost(struct Air* air, uint8_t* out) {
	memset(out, 0, LP_BUFFER_SIZE);
	memcpy(out, air->rubyLP, LINK_PLAYER_BLOCK_SIZE);
	_put16(&out[LP_VERSION_OFFSET], 0x4005);
	memset(&out[LP_LINK_TYPE_OFFSET], 0, 4);
}

// The leader's LinkPlayer as the game expects to find it: the same record with the game's own link type, because the
// cable club accepts the exchange only when every player reports the same link type.
static void _deliverLinkPlayerToGame(struct Air* air) {
	if (!air->haveHostLP || !air->sessionRubyLP || air->p0LPDelivered) {
		return;
	}
	uint8_t block[LINK_PLAYER_BLOCK_SIZE];
	memcpy(block, air->hostLP, sizeof(block));
	memcpy(&block[LP_LINK_TYPE_OFFSET], &air->rubyLP[LP_LINK_TYPE_OFFSET], 4);
	_cablePushBlock(air, block, sizeof(block));
	air->p0LPDelivered = true;
	air->readyAt = air->cablePackets + SESSION_SETTLE_PACKETS;
	if (!air->cardRequested && !air->roomClosed && air->closeCount == 0 && !air->cancelReturn) {
		air->cardArmed = true;
		air->cardAt = air->cablePackets + 15;
	}
	if (air->cancelReturn) {
		// Back in the room after a cancelled trade: the game sends keys straight away, without a standby of its own.
		air->cancelReturn = false;
		air->roomClosed = false;
		air->exitKeySeen = false;
		air->keysActive = true;
		AIRLOG(air, "room re-entered after the cancelled trade: keys resume");
	}
	AIRLOG(air, "leader's LinkPlayer delivered to the game (link type %04X)", _le16(&block[LP_LINK_TYPE_OFFSET]));
}

static void _maybeStartRound0(struct Air* air) {
	if (air->round0Started || !air->lpSendDone || !air->p0LPDelivered) {
		return;
	}
	air->round0Started = true;
	air->purpose = PURPOSE_ROUND0;
	_barrierInitiate(&air->bar, BAR_STANDBY);
	AIRLOG(air, "LinkPlayers exchanged both ways: standby round %u", air->bar.localCount);
}

// The game's cable link is usable for blocks once both LinkPlayers of this session are through.
static bool _sessionReady(const struct Air* air) {
	return air->sessionRubyLP && air->p0LPDelivered && air->cablePackets >= air->readyAt;
}

static void _requestFromGame(struct Air* air, unsigned type) {
	air->expect = EXPECT_PULL;
	air->pullType = type;
	_cablePushCmd(air, LINKCMD_SEND_BLOCK_REQ, type, 0);
}

static void _releaseHeld(struct Air* air) {
	if (!_sessionReady(air)) {
		return;
	}
	if (air->pendingPull >= 0 && air->expect != EXPECT_PULL) {
		AIRLOG(air, "cable link ready: forwarding the leader's pull of type %d", air->pendingPull);
		_requestFromGame(air, air->pendingPull);
		air->pendingPull = -1;
	}
	unsigned i;
	for (i = 0; i < air->heldCount; ++i) {
		AIRLOG(air, "cable link ready: delivering a held block of %u bytes", air->held[i].size);
		_cablePushBlock(air, air->held[i].data, air->held[i].size);
	}
	air->heldCount = 0;
}

// A trainer card for the leader's player, made from its LinkPlayer, so that the game can finish its "awaiting link-up" and
// walk into the room while the leader is still on its own room-entry screen. (The leader's real card arrives only after
// its room-entry animation; the game's cable club waits for it before it goes on.)
static void _buildHostCard(struct Air* air, uint8_t* out) {
	memset(out, 0, CARD_SIZE);
	const uint8_t* lp = air->hostLP;
	out[0x00] = lp[16 + 0x13]; // gender
	out[0x02] = 1;             // hasPokedex
	out[0x0E] = lp[LP_TRAINER_ID_OFFSET];
	out[0x0F] = lp[LP_TRAINER_ID_OFFSET + 1];
	unsigned i;
	for (i = 0; i < 7 && lp[LP_NAME_OFFSET + i] != 0xFF; ++i) {
		out[0x30 + i] = lp[LP_NAME_OFFSET + i];
	}
	out[0x30 + i] = 0xFF;
}

static void _requestEarlyCard(struct Air* air) {
	if (air->expect != EXPECT_NONE) {
		air->cardArmed = true; // the game is busy with something else: try again shortly
		air->cardAt = air->cablePackets + 10;
		return;
	}
	uint8_t card[CARD_SIZE];
	_buildHostCard(air, card);
	air->cardRequested = true;
	air->cardSynth = true;
	air->expect = EXPECT_CARD;
	_cablePushCmd(air, LINKCMD_SEND_BLOCK_REQ, 2, 0);
	_cablePushBlock(air, card, CARD_SIZE);
	AIRLOG(air, "trainer cards exchanged with the game now (the leader's own card is dropped when it arrives)");
}

static void _queueCard(struct Air* air) {
	_childQueuePush(air, air->rubyCard, CARD_SIZE, false);
	air->childQueue[(air->childHead + air->childCount + CHILD_QUEUE - 1) % CHILD_QUEUE].isCard = true;
	air->cardQueued = true;
}

// The leader pulled a block (RFUCMD_SEND_BLOCK_REQ). The first pull is the LinkPlayer, which we already hold; every other
// pull is forwarded to the game as the cable's own request for that block type.
static void _hostPull(struct Air* air, unsigned type) {
	AIRLOG(air, "leader pulls block type %u", type);
	if (type == 2 && air->cardRequested) {
		if (air->cardQueued) {
			return;
		}
		if (air->haveRubyCard) {
			_queueCard(air);
		} else {
			air->cardPullWaiting = true;
		}
		return;
	}
	if (!air->lpSentToHost && type <= 1) {
		if (!air->haveRubyLP) {
			AIRLOG(air, "pull before we know the game's LinkPlayer");
			return;
		}
		uint8_t buffer[LP_BUFFER_SIZE];
		_buildLinkPlayerForHost(air, buffer);
		_childQueuePush(air, buffer, LP_BUFFER_SIZE, true);
		air->lpSentToHost = true;
		return;
	}
	if (air->expect == EXPECT_PULL || air->pendingPull >= 0) {
		return; // a repeat of the pull we are already serving
	}
	if (!_sessionReady(air)) {
		air->pendingPull = type; // the game's link is not up yet: ask it as soon as it is
		AIRLOG(air, "cable link not ready: pull of type %u held", type);
		return;
	}
	_requestFromGame(air, type);
}

// A block the leader finished sending us.
static void _hostBlock(struct Air* air, unsigned count, const uint8_t* data) {
	unsigned size = _sizeFromCount(count);
	if (count == 17 && !air->haveHostLP && !memcmp(data, "GameFreak inc.", 14)) {
		memcpy(air->hostLP, data, LINK_PLAYER_BLOCK_SIZE);
		air->haveHostLP = true;
		AIRLOG(air, "leader's LinkPlayer received (version %04X, link type %04X)", _le16(&data[LP_VERSION_OFFSET]),
		       _le16(&data[LP_LINK_TYPE_OFFSET]));
		_deliverLinkPlayerToGame(air);
		_maybeStartRound0(air);
		return;
	}
	if (count == 9) {
		// Its trainer card: the first 0x38 bytes are the same struct in every Gen 3 game.
		if (air->cardSynth) {
			AIRLOG(air, "leader's trainer card received: not needed, the game already has one");
			air->cardSynth = false;
			return;
		}
		AIRLOG(air, "leader's trainer card received");
	} else if (count == 2) {
		// A trade-menu command: the first word is its LINKCMD (AABB ready, DDDD set mons, EEBB both cancelled, ...).
		unsigned command = _le16(data);
		AIRLOG(air, "leader's trade-menu block %04X received", command);
		if (command == 0xEEBB) {
			air->cancelPending = true;
		}
	} else {
		AIRLOG(air, "leader's block of %u bytes received", size);
	}
	if (!_sessionReady(air)) {
		if (air->heldCount < HELD_BLOCKS && size <= MAX_BLOCK_BYTES) {
			struct ChildBlock* h = &air->held[air->heldCount++];
			h->size = size;
			memcpy(h->data, data, size);
			AIRLOG(air, "cable link not ready: block of %u bytes held", size);
		}
		return;
	}
	_cablePushBlock(air, data, size);
}

// A block the game finished sending on the cable.
static void _gameBlock(struct Air* air, unsigned size, const uint8_t* data) {
	if (air->expect == EXPECT_LINK_PLAYER && size >= LINK_PLAYER_BLOCK_SIZE) {
		memcpy(air->rubyLP, data, LINK_PLAYER_BLOCK_SIZE);
		air->haveRubyLP = true;
		air->sessionRubyLP = true;
		air->expect = EXPECT_NONE;
		AIRLOG(air, "game's LinkPlayer received (version %04X, link type %04X)", _le16(&data[LP_VERSION_OFFSET]),
		       _le16(&data[LP_LINK_TYPE_OFFSET]));
		if (air->link == AIR_NI || air->link == AIR_UNI) {
			// already connected (a later cable session): nothing to tell the leader
		}
		_deliverLinkPlayerToGame(air);
		_maybeStartRound0(air);
		return;
	}
	if (air->expect == EXPECT_CARD && size >= CARD_SIZE) {
		memset(air->rubyCard, 0, sizeof(air->rubyCard));
		memcpy(air->rubyCard, data, CARD_VERSION_OFFSET);
		air->rubyCard[CARD_VERSION_OFFSET] = 5;
		air->haveRubyCard = true;
		air->expect = EXPECT_NONE;
		AIRLOG(air, "game's trainer card received");
		if (air->cardPullWaiting) {
			air->cardPullWaiting = false;
			_queueCard(air);
		}
		return;
	}
	uint8_t block[MAX_BLOCK_BYTES];
	memset(block, 0, sizeof(block));
	unsigned use = size;
	if (air->expect == EXPECT_PULL) {
		use = _sizeFromRequest(air->pullType);
		air->expect = EXPECT_NONE;
		AIRLOG(air, "game answered pull %u with %u bytes", air->pullType, size);
	}
	memcpy(block, data, size < sizeof(block) ? size : sizeof(block));
	if (use == CARD_SIZE) {
		// Ruby's card is the 0x38-byte RSE layout; FRLG's continues with its own fields, which stay empty.
		memset(&block[CARD_VERSION_OFFSET], 0, sizeof(block) - CARD_VERSION_OFFSET);
		block[CARD_VERSION_OFFSET] = 5;
	}
	_childQueuePush(air, block, use, false);
}

static void _gameStandby(struct Air* air) {
	if (air->cancelReturn) {
		// The game's own standby on returning to the room; the leader's matching round was already part of the exit.
		air->cancelReturn = false;
		air->roomClosed = false;
		air->exitKeySeen = false;
		air->keysActive = true;
		AIRLOG(air, "game standby (2FFE) on re-entering the room: answered locally, keys resume");
		_cablePushCmd(air, LINKCMD_READY_EXIT_STANDBY, 0, 0);
		return;
	}
	if (air->cardRequested && !air->cardSendDone && !air->roomClosed) {
		// The game is ahead of the leader: its round has to wait until the leader has pulled and received our card, because a
		// standby and a block never share the wire.
		air->pendingGameStandby = true;
		AIRLOG(air, "game standby (2FFE) is early: waiting for the leader's card pull");
		return;
	}
	AIRLOG(air, "game standby (2FFE) -> wireless standby round %u", air->bar.localCount);
	air->purpose = PURPOSE_GAME_STANDBY;
	_barrierInitiate(&air->bar, BAR_STANDBY);
}

static void _flushPendingStandby(struct Air* air) {
	if (air->pendingGameStandby && air->cardSendDone && air->bar.mode == BAR_IDLE) {
		air->pendingGameStandby = false;
		_gameStandby(air);
	}
}

static void _gameClose(struct Air* air) {
	if (air->exitKeySeen && !air->cancelPending && !air->roomClosed) {
		// Leaving the room: the leader does not standby, it closes. Answer with a real READY_CLOSE_LINK and end the link.
		AIRLOG(air, "game close (5FFF) after EXIT_ROOM: closing the wireless link");
		air->keysActive = false;
		air->purpose = PURPOSE_EXIT_CLOSE;
		if (air->hostClosedSeen) {
			_exitClosePassed(air);
		} else {
			_barrierInitiate(&air->bar, BAR_CLOSE);
		}
		return;
	}
	AIRLOG(air, "game close (5FFF) #%u: wireless standby rounds, then the cable link is answered", air->closeCount + 1);
	air->keysActive = false;
	air->roomClosed = true;
	air->purpose = PURPOSE_GAME_CLOSE;
	air->cancelClose = air->cancelPending;
	air->cancelPending = false;
	// The leader's standby rounds around the game's closes alternate. Leaving the room: two (post-seat, then the trade menu's
	// entry). Ending the trade menu for the trade scene: one. Ending the trade: two (its last save round, then the
	// entry of the trade menu it returns to, which pulls the parties again). And so on for further trades.
	air->closeRoundsLeft = (air->closeCount % 2 == 0) ? 2 : 1;
	if (air->cancelClose) {
		// Cancelling the trade: the leader does an exit standby, returns to the room and standbys again before it sends keys.
		air->closeRoundsLeft = 2;
		air->closeCount = 0; // the next trip through the room starts the sequence over
	} else {
		++air->closeCount;
	}
	_barrierInitiate(&air->bar, BAR_STANDBY);
}

// A round we started has passed on the wireless side.
static void _exitClosePassed(struct Air* air) {
	AIRLOG(air, "room exit complete: answering the game, ending the wireless link");
	_cablePushCmd(air, LINKCMD_READY_CLOSE_LINK, 0, 0);
	air->backend->disconnect(air->backend, 0);
	_airResetLink(air);
	air->link = -1;
	air->haveRubyLP = false; // a new cable club visit starts a new search
	air->closeCount = 0;
	air->roomClosed = false;
	air->cancelPending = false;
	air->cancelClose = false;
	air->cancelReturn = false;
	air->exitKeySeen = false;
	air->hostClosedSeen = false;
	air->purpose = PURPOSE_NONE;
}

static void _roundPassed(struct Air* air) {
	AIRLOG(air, "standby round passed (count now %u)", air->bar.localCount);
	switch (air->purpose) {
	case PURPOSE_EXIT_CLOSE:
		_exitClosePassed(air);
		break;
	case PURPOSE_GAME_STANDBY:
		if (!air->roomClosed) {
			air->keysActive = true;
		}
		air->purpose = PURPOSE_NONE;
		_cablePushCmd(air, LINKCMD_READY_EXIT_STANDBY, 0, 0);
		break;
	case PURPOSE_GAME_CLOSE:
		if (air->closeRoundsLeft > 1) {
			--air->closeRoundsLeft;
			_barrierInitiate(&air->bar, BAR_STANDBY);
		} else {
			air->closeRoundsLeft = 0;
			air->purpose = PURPOSE_NONE;
			_cablePushCmd(air, LINKCMD_READY_CLOSE_LINK, 0, 0);
			if (air->cancelClose) {
				air->cancelClose = false;
				air->cancelReturn = true;
				AIRLOG(air, "trade cancelled: waiting for the game to re-enter the room");
			}
		}
		break;
	default:
		air->purpose = PURPOSE_NONE;
		break;
	}
	_flushPendingStandby(air);
}

// --- 7. air ----------------------------------------------------------------------------------------------------------

static void _buildGameData(struct Air* air, uint8_t* out) {
	const uint8_t* lp = air->rubyLP;
	memset(out, 0, 26);
	_put16(&out[0], 2); // RFU_SERIAL_GAME
	unsigned compat = 2 /* English */ | (5 << 10) /* LeafGreen */;
	_put16(&out[2], compat);
	_put16(&out[4], _le16(&lp[LP_TRAINER_ID_OFFSET]));
	out[12] = 4 | 0x80; // ACTIVITY_TRADE, started
	uint8_t* uname = &out[17];
	unsigned i;
	for (i = 0; i < 7 && lp[LP_NAME_OFFSET + i] != 0xFF; ++i) {
		uname[i] = lp[LP_NAME_OFFSET + i];
	}
	uname[i] = 0xFF;
}

static void _airSend(struct Air* air, const uint8_t* data, unsigned length) {
	air->backend->sendData(air->backend, data, length);
	++air->childFrames;
}

static void _airResetLink(struct Air* air) {
	air->niStarted = false;
	air->hostUni = false;
	air->hostNiDone = false;
	air->tag = 0;
	air->keyCount = 0;
	_recvReset(&air->rx0);
	_recvReset(&air->rx1);
	memset(&air->send, 0, sizeof(air->send));
	air->childCount = 0;
	air->childHead = 0;
	_barrierInit(&air->bar);
	air->lpSentToHost = false;
	air->lpSendDone = false;
	air->cardArmed = false;
	air->cardRequested = false;
	air->haveRubyCard = false;
	air->cardSynth = false;
	air->cardQueued = false;
	air->cardPullWaiting = false;
	air->cardSendDone = false;
	air->pendingGameStandby = false;
	air->round0Started = false;
	air->keysActive = false;
	memset(&air->rubyKeys, 0, sizeof(air->rubyKeys));
	memset(&air->hostKeys, 0, sizeof(air->hostKeys));
	air->lastHostKeyCount = -1;
	air->purpose = PURPOSE_NONE;
	air->hostFrames = 0;
	air->childFrames = 0;
	air->silentFrames = 0;
}

static void _startSearch(struct Air* air) {
	air->link = AIR_SEARCH;
	air->linkFrames = 0;
	air->backend->searchStart(air->backend);
	AIRLOG(air, "searching for a leader (backend \"%s\")", air->backendName);
}

static bool _looksLikeTradeHost(const struct GBASIORFUEvent* event) {
	// RfuGameData: word 3's low byte is the activity (ACTIVITY_TRADE = 4, without the Union Room flag).
	return event->slot != 0xFF && (event->words[3] & 0x7F) == 4;
}

static void _handleHostFrame(struct Air* air, const uint8_t* data, unsigned length);

static void _drainEvents(struct Air* air) {
	struct GBASIORFUEvent event;
	while (GBASIORFUPopEvent(&air->rfu, &event)) {
		switch (event.type) {
		case RFU_EVENT_BROADCAST:
			if (air->link == AIR_SEARCH && _looksLikeTradeHost(&event)) {
				AIRLOG(air, "leader %04X found (%08X %08X %08X %08X %08X %08X): joining", event.deviceId, event.words[0],
				       event.words[1], event.words[2], event.words[3], event.words[4], event.words[5]);
				air->backend->searchStop(air->backend);
				air->hostId = event.deviceId;
				air->link = AIR_CONNECTING;
				air->linkFrames = 0;
				air->backend->connect(air->backend, event.deviceId);
			}
			break;
		case RFU_EVENT_CONNECT_RESULT:
			if (air->link != AIR_CONNECTING) {
				break;
			}
			if (event.accepted) {
				AIRLOG(air, "leader accepted us (slot %d): NI handshake", event.slot);
				uint8_t src[26];
				_buildGameData(air, src);
				_airResetLink(air);
				_niInit(&air->ni, src);
				air->niStarted = true;
				air->link = AIR_NI;
				air->linkFrames = 0;
			} else {
				AIRLOG(air, "leader refused or did not answer: searching again");
				_startSearch(air);
			}
			break;
		case RFU_EVENT_DATA:
			if (event.length && (air->link == AIR_NI || air->link == AIR_UNI)) {
				air->silentFrames = 0;
				_handleHostFrame(air, event.data, event.length);
			}
			break;
		case RFU_EVENT_DISCONNECTED:
			AIRLOG(air, "disconnected from the leader");
			if (air->link == AIR_NI || air->link == AIR_UNI) {
				_airResetLink(air);
				_startSearch(air);
			}
			break;
		default:
			break;
		}
	}
}

static void _logHostOp(struct Air* air, unsigned op, const uint16_t words[7]) {
	if (op == RFUCMD_SEND_BLOCK || op == RFUCMD_SEND_HELD_KEYS || op == 0) {
		return;
	}
	if (op == air->lastHostOp && op != RFUCMD_SEND_BLOCK_REQ) {
		return;
	}
	air->lastHostOp = op;
	AIRLOG(air, "host %04X %04X %04X %04X", words[0], words[1], words[2], words[3]);
}

// Fills words with the child's slot for this frame; all zero is an idle frame.
static void _chooseSlot(struct Air* air, uint16_t words[7]) {
	memset(words, 0, 7 * sizeof(uint16_t));
	if (_barrierWant(&air->bar, words)) {
		return;
	}
	if (air->bar.mode != BAR_IDLE) {
		return; // a barrier is in progress: quiet until the round advances
	}
	if (!air->send.active && air->childCount) {
		struct ChildBlock* block = &air->childQueue[air->childHead];
		_sendStart(&air->send, block->data, block->size, block->isLinkPlayer);
		air->send.isCard = block->isCard;
		_recvReset(&air->rx1);
		AIRLOG(air, "sending a block of %u bytes (%u fragments)", block->size, air->send.count);
		air->childHead = (air->childHead + 1) % CHILD_QUEUE;
		--air->childCount;
	}
	if (air->send.active) {
		bool wasLP = air->send.isLinkPlayer;
		bool wasCard = air->send.isCard;
		_sendTick(&air->send, &air->rx1, words);
		if (!air->send.active && wasCard) {
			air->cardSendDone = true;
			AIRLOG(air, "trainer card acknowledged by the leader");
			_flushPendingStandby(air);
		} else if (!air->send.active && wasLP) {
			air->lpSendDone = true;
			AIRLOG(air, "LinkPlayer block acknowledged by the leader");
			_maybeStartRound0(air);
		} else if (!air->send.active) {
			AIRLOG(air, "block acknowledged by the leader");
		}
		if (words[0]) {
			return;
		}
	}
	if (air->keysActive) {
		words[0] = RFUCMD_SEND_HELD_KEYS;
		words[1] = (air->keyCount++ << 8) | _keyPop(&air->rubyKeys);
	}
}

static void _sendUni(struct Air* air, uint16_t words[7]) {
	uint8_t frame[2 + SLOT_BYTES];
	if (words[0]) {
		words[0] |= (uint16_t) (air->tag << 5);
		air->tag = (air->tag + 1) & 7;
	}
	_put16(frame, (LCOM_UNI << 10) | SLOT_BYTES);
	unsigned i;
	for (i = 0; i < 7; ++i) {
		_put16(&frame[2 + i * 2], words[i]);
	}
	_airSend(air, frame, sizeof(frame));
}

static void _handleHostFrame(struct Air* air, const uint8_t* data, unsigned length) {
	++air->hostFrames;
	if (length < 3) {
		// A bare frame (the leader has nothing to say yet, e.g. a 1-byte idle frame): it is still the leader asking us to
		// speak, and until our game data is out the leader has nothing to show for us, so this is what starts the NI send.
		if (air->link == AIR_NI && air->ni.state < 4) {
			uint8_t frame[16];
			unsigned len = _niNext(&air->ni, frame);
			if (len) {
				_airSend(air, frame, len);
			}
		}
		return;
	}
	uint32_t header = data[0] | (data[1] << 8) | (data[2] << 16);
	unsigned state = (header >> 14) & 0xF;
	unsigned ack = (header >> 13) & 1;
	unsigned n = (header >> 11) & 3;
	unsigned phase = (header >> 9) & 3;

	if (state != LCOM_UNI) {
		// The host's own NI transfer (its join status). Every sub-frame is acknowledged by mirroring it; our own NI
		// data goes first.
		if (air->link != AIR_NI) {
			return;
		}
		if (air->hostFrames == 1 || (state == LCOM_NI_START && !ack)) {
			AIRLOG(air, "host NI frame state=%u ack=%u n=%u phase=%u len=%u", state, ack, n, phase, length);
		}
		uint8_t frame[16];
		unsigned len = 0;
		if (air->ni.state < 4) {
			len = _niNext(&air->ni, frame);
		} else if (!ack && (state == LCOM_NI_START || state == LCOM_NI || state == LCOM_NI_END)) {
			_put16(frame, _childLLSF(state, n, phase, 1, 0));
			len = 2;
			if (state == LCOM_NI_END) {
				air->hostNiDone = true;
			}
		} else if (!ack && state == LCOM_NULL) {
			air->hostNiDone = true;
		}
		if (len) {
			_airSend(air, frame, len);
		}
		return;
	}

	if (length < HOST_FRAME_BYTES) {
		return;
	}
	if (air->link == AIR_NI) {
		if (air->ni.state < 4) {
			return; // our own NI data is still going out
		}
		air->link = AIR_UNI;
		air->hostUni = true;
		AIRLOG(air, "host entered UNI: link layer up (%u host frames)", air->hostFrames);
	}

	// Slot 0 is the leader's own command, slot 1 the reflection of ours.
	const uint8_t* slot0 = &data[3];
	const uint8_t* slot1 = &data[3 + SLOT_BYTES];
	uint16_t words0[7];
	_slotToWords(slot0, words0);
	bool sawBarrier = false;
	bool completed;
	if (!_slotIdle(slot0)) {
		unsigned op = words0[0] & RFUCMD_MASK;
		_logHostOp(air, op, words0);
		switch (op) {
		case RFUCMD_SEND_PLAYER_IDS:
			break;
		case RFUCMD_SEND_BLOCK_REQ:
			_hostPull(air, words0[1]);
			break;
		case RFUCMD_SEND_BLOCK_INIT:
		case RFUCMD_SEND_BLOCK:
			_feedRecv(&air->rx0, words0, slot0, &completed);
			if (completed) {
				_hostBlock(air, air->rx0.count, air->rx0.buf);
				air->rx0.receiving = false;
				air->rx0.done = false;
				air->rx0.flags = 0;
			}
			break;
		case RFUCMD_SEND_HELD_KEYS:
			// The counter in the high byte moves every leader frame; a repeat of the same counter is the same event.
			if (air->lastHostKeyCount != (int) (words0[1] >> 8)) {
				air->lastHostKeyCount = words0[1] >> 8;
				_keyPush(&air->hostKeys, words0[1] & 0xFF);
				if ((words0[1] & 0xFF) == LINK_KEY_CODE_EXIT_ROOM) {
					air->exitKeySeen = true;
				}
			}
			break;
		case RFUCMD_READY_EXIT_STANDBY:
			sawBarrier = true;
			if (_barrierHostStandby(air, words0[1])) {
				_roundPassed(air);
			}
			break;
		case RFUCMD_READY_CLOSE_LINK:
			sawBarrier = true;
			_barrierHostClose(air, words0[1]);
			air->hostClosedSeen = true;
			if (air->purpose == PURPOSE_EXIT_CLOSE) {
				_exitClosePassed(air);
			}
			break;
		case RFUCMD_DISCONNECT:
			AIRLOG(air, "leader sent DISCONNECT");
			break;
		default:
			break;
		}
	}
	if (!_slotIdle(slot1)) {
		uint16_t words1[7];
		_slotToWords(slot1, words1);
		_feedRecv(&air->rx1, words1, slot1, &completed);
	}
	if (_barrierObserve(air, sawBarrier)) {
		_roundPassed(air);
	}

	uint16_t out[7];
	_chooseSlot(air, out);
	_sendUni(air, out);
}

static void _airFrame(void* context) {
	struct Air* air = context;
	if (air->backend->poll) {
		air->backend->poll(air->backend);
	}
	if (air->backend->frame) {
		air->backend->frame(air->backend);
	}
	if (air->link < 0 && air->haveRubyLP) {
		_startSearch(air);
	}
	_drainEvents(air);
	if (air->link == AIR_NI || air->link == AIR_UNI) {
		if (++air->silentFrames > HOST_SILENT_FRAMES) {
			AIRLOG(air, "the leader went quiet");
			_airResetLink(air);
			air->backend->disconnect(air->backend, 0);
			_startSearch(air);
		}
	}
}

// Cable-facing peer callbacks ----------------------------------------------------------------------------------------

static void _peerReset(void* context) {
	struct Air* air = context;
	air->cablePackets = 0;
	air->readyAt = 0;
	air->announced = false;
	air->expect = EXPECT_NONE;
	air->sessionRubyLP = false;
	air->p0LPDelivered = false;
	air->rxSize = 0;
	air->rxPos = 0;
	air->cqHead = 0;
	air->cqCount = 0;
	++air->sessions;
}

static void _peerGameCommand(void* context, const uint16_t command[CMD_WORDS]) {
	struct Air* air = context;
	unsigned i;
	switch (command[0]) {
	case LINKCMD_INIT_BLOCK:
		air->rxSize = command[1] <= MAX_BLOCK_BYTES ? command[1] : 0;
		air->rxPos = 0;
		break;
	case LINKCMD_CONT_BLOCK:
		if (air->rxSize) {
			for (i = 0; i < CMD_WORDS - 1; ++i) {
				unsigned at = air->rxPos + i * 2;
				if (at + 1 < sizeof(air->rxBuf)) {
					_put16(&air->rxBuf[at], command[1 + i]);
				}
			}
			air->rxPos += (CMD_WORDS - 1) * 2;
			if (air->rxPos >= air->rxSize) {
				unsigned size = air->rxSize;
				air->rxSize = 0;
				_gameBlock(air, size, air->rxBuf);
			}
		}
		break;
	case LINKCMD_SEND_HELD_KEYS:
		_keyPush(&air->rubyKeys, command[1] & 0xFF);
		if ((command[1] & 0xFF) == LINK_KEY_CODE_EXIT_ROOM) {
			air->exitKeySeen = true;
		}
		break;
	case LINKCMD_READY_EXIT_STANDBY:
		_gameStandby(air);
		break;
	case LINKCMD_READY_CLOSE_LINK:
		_gameClose(air);
		break;
	default:
		break;
	}
}

static bool _peerNextCommand(void* context, uint16_t command[CMD_WORDS]) {
	struct Air* air = context;
	++air->cablePackets;
	if (air->heldCount || air->pendingPull >= 0) {
		_releaseHeld(air);
	}
	if (air->cardArmed && air->cablePackets >= air->cardAt) {
		air->cardArmed = false;
		_requestEarlyCard(air);
	}
	if (!air->announced && air->cablePackets > ANNOUNCE_PACKETS) {
		// The master asks for the player data exchange (LinkCB_RequestPlayerDataExchange).
		air->announced = true;
		air->expect = EXPECT_LINK_PLAYER;
		memset(command, 0, CMD_WORDS * sizeof(uint16_t));
		command[0] = LINKCMD_SEND_LINK_TYPE;
		command[1] = 0x1133;
		return true;
	}
	if (air->cqCount) {
		memcpy(command, air->cq[air->cqHead], CMD_WORDS * sizeof(uint16_t));
		air->cqHead = (air->cqHead + 1) % CABLE_QUEUE;
		--air->cqCount;
		return true;
	}
	if (air->keysActive) {
		memset(command, 0, CMD_WORDS * sizeof(uint16_t));
		command[0] = LINKCMD_SEND_HELD_KEYS;
		command[1] = _keyPop(&air->hostKeys);
		return true;
	}
	return false;
}

static void _airDestroy(void* context) {
	struct Air* air = context;
	if (air->backend) {
		if (air->backend->deinit) {
			air->backend->deinit(air->backend);
		}
		GBASIORFUBackendDestroy(air->backend);
	}
	GBASIORFUDestroy(&air->rfu);
	free(air);
}

bool GBASIORFUWrapperAttachAir(struct GBASIORFUWrapper* wrapper, const char* backend) {
	struct GBASIORFUBackend* b = GBASIORFUBackendCreate(backend);
	if (!b) {
		return false;
	}
	struct Air* air = calloc(1, sizeof(*air));
	if (!air) {
		GBASIORFUBackendDestroy(b);
		return false;
	}
	air->w = wrapper;
	air->backend = b;
	snprintf(air->backendName, sizeof(air->backendName), "%s", backend);
	GBASIORFUCreate(&air->rfu, b);
	_barrierInit(&air->bar);
	_recvReset(&air->rx0);
	_recvReset(&air->rx1);
	if (b->init && !b->init(b, &air->rfu)) {
		GBASIORFUDestroy(&air->rfu);
		GBASIORFUBackendDestroy(b);
		free(air);
		return false;
	}
	air->link = -1;
	air->pendingPull = -1;

	wrapper->air = air;
	wrapper->airDestroy = _airDestroy;
	wrapper->peer.context = air;
	wrapper->peer.gameCommand = _peerGameCommand;
	wrapper->peer.nextCommand = _peerNextCommand;
	wrapper->peer.reset = _peerReset;
	wrapper->peer.frame = _airFrame;
	return true;
}
