/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "trade-shim.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum {
	RFUCMD_READY_EXIT_STANDBY = 0x6600,
	RFUCMD_SEND_BLOCK_INIT = 0x8800,
	RFUCMD_SEND_BLOCK = 0x8900,
	RFUCMD_SEND_BLOCK_REQ = 0xA100,
	LINKCMD_CONFIRM_FINISH_TRADE = 0xDCBA,

	// Post-trade standby rounds a retail FireRed/LeafGreen cartridge runs: trade_scene.c CB2_SaveAndEndTrade cases 1,
	// 41, 5 and 8, then trade.c CB2_CreateTradeMenu case 4. The parent answers each; after the fifth answer it is
	// parked in its sixth barrier.
	kChildRounds = 5,
	// Quiet time after that fifth answer before the extra round goes in: long enough for the parent to have processed
	// its own answer (a frame or two) and for a parent that did not need a sixth round to have sent its party request
	// instead (~12 frames).
	kSettleMs = 750,
	// For a cartridge with a different round count: any post-trade answer followed by this much silence on both sides
	// also counts as the deadlock. Longer than the cartridge's save, the longest legitimate gap between its rounds.
	kFallbackMs = 8000,
	// The parent parked in its barrier answers the extra round within a frame or two.
	kAckMs = 2000,
	kMaxAttempts = 4,
	// A child that accepted a block request starts sending within a couple of frames; one silent this long since the
	// request refused it (Rfu_InitBlockSend returns FALSE while a previous block send or another command is pending),
	// and the parent never repeats it. Silence, not elapsed time, so a slow but accepted request is not answered twice.
	kRerequestMs = 700,
	kMaxRerequests = 3,
	// Commands keep going to the parent and none come back for this long: its game has stopped taking them.
	kEchoStallFrames = 6,
	kEchoStallMs = 2000,
};

// Fragments per block request type, sBlockRequests[] in link_rfu_2.c: 200, 200, 100, 220 and 40 bytes in 12-byte
// fragments.
static const uint8_t kRequestFragments[] = {17, 17, 9, 19, 4};

static uint16_t _rd16(const uint8_t* p) {
	return (uint16_t) (p[0] | (p[1] << 8));
}

static void _wr16(uint8_t* p, uint16_t v) {
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
}

static void _log(struct LdnTradeShim* shim, const char* format, ...) {
	if (!shim->log) {
		return;
	}
	char message[160];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	shim->log(shim->logUser, message);
}

void LdnTradeShimInit(struct LdnTradeShim* shim, LdnTradeShimLog log, void* logUser) {
	shim->log = log;
	shim->logUser = logUser;
	LdnTradeShimReset(shim);
}

void LdnTradeShimReset(struct LdnTradeShim* shim) {
	LdnTradeShimLog log = shim->log;
	void* logUser = shim->logUser;
	memset(shim, 0, sizeof(*shim));
	shim->log = log;
	shim->logUser = logUser;
	shim->lastRound = -1;
	shim->fakeRound = -1;
	shim->requestType = -1;
}

// Round-number mapping. A child round c is the parent's round c plus every injected round at or below it; an injected
// round has no child counterpart.
static int _toParent(const struct LdnTradeShim* shim, int c) {
	int p = c + shim->base;
	for (int i = 0; i < shim->fakeCount; ++i) {
		if (shim->fakes[i] <= p) {
			++p;
		}
	}
	return p;
}

static int _toChild(const struct LdnTradeShim* shim, int p) {
	int c = p - shim->base;
	for (int i = 0; i < shim->fakeCount; ++i) {
		if (shim->fakes[i] < p) {
			--c;
		}
	}
	return c;
}

static bool _isFake(const struct LdnTradeShim* shim, int p) {
	for (int i = 0; i < shim->fakeCount; ++i) {
		if (shim->fakes[i] == p) {
			return true;
		}
	}
	return false;
}

static void _addFake(struct LdnTradeShim* shim, int p) {
	if (shim->fakeCount == LDN_TRADE_SHIM_MAX_FAKES) {
		// Rounds only move forward, so the oldest one is far behind every number still in play and can become part of
		// the plain offset.
		memmove(shim->fakes, shim->fakes + 1, sizeof(shim->fakes) - sizeof(shim->fakes[0]));
		--shim->fakeCount;
		++shim->base;
	}
	shim->fakes[shim->fakeCount++] = (uint16_t) p;
}

// A parent UNI frame for the child: 3-byte LLSF header (one child slot, UNI state, 70 data bytes) and five 14-byte
// slots.
static size_t _buildHostFrame(uint8_t* out, uint16_t cmd0, uint16_t v0, uint16_t cmd1, uint16_t v1) {
	memset(out, 0, LDN_TRADE_SHIM_HOST_FRAME);
	out[0] = 0x46;
	out[1] = 0x00;
	out[2] = 0x05;
	_wr16(out + 3, cmd0);
	_wr16(out + 5, v0);
	_wr16(out + 17, cmd1);
	_wr16(out + 19, v1);
	return LDN_TRADE_SHIM_HOST_FRAME;
}

static size_t _buildChildFrame(uint8_t* out, uint8_t tag, uint16_t round) {
	memset(out, 0, LDN_TRADE_SHIM_CHILD_FRAME);
	out[0] = 0x0E; // child UNI frame, 14-byte slot
	out[1] = 0x10;
	out[2] = (uint8_t) (tag << 5);
	out[3] = RFUCMD_READY_EXIT_STANDBY >> 8;
	_wr16(out + 4, round);
	return LDN_TRADE_SHIM_CHILD_FRAME;
}

static bool _isChildUni(const uint8_t* payload) {
	return ((_rd16(payload) >> 10) & 15) == 4;
}

size_t LdnTradeShimChild(struct LdnTradeShim* shim, uint8_t* payload, size_t length, int64_t nowMs, uint8_t* reply, size_t replyCapacity, bool* forward) {
	if (forward) {
		*forward = true;
	}
	if (length < LDN_TRADE_SHIM_CHILD_FRAME || !_isChildUni(payload)) {
		return 0;
	}
	uint8_t* slot = payload + 2;
	if (slot[1] == 0) {
		return 0; // idle: no command, no tag
	}
	shim->quietSince = nowMs;
	uint16_t command = (uint16_t) ((slot[1] << 8) | (slot[0] & 0x1F));
	uint8_t rawTag = (uint8_t) (slot[0] >> 5);
	uint8_t index = (uint8_t) (slot[0] & 0x1F);
	bool fragment = (command & 0xFF00) == RFUCMD_SEND_BLOCK;
	shim->lastCommandMs = nowMs;
	if (shim->haveLastFrame && memcmp(payload, shim->lastFrame, LDN_TRADE_SHIM_CHILD_FRAME) == 0) {
		// The same frame twice in a row: one GBA transfer handed over again (librfu re-runs an exchange whose
		// acknowledgement it misread, or the adapter retransmits). The game's own commands always advance the tag, so
		// this is not a new command; forwarding it would make the parent act on it twice.
		if (forward) {
			*forward = false;
		}
		++shim->duplicates;
		return 0;
	}
	memcpy(shim->lastFrame, payload, LDN_TRADE_SHIM_CHILD_FRAME);
	shim->haveLastFrame = true;
	// Tags are stamped at send time (LdnTradeShimStamp), not forwarded: the parent accepts only the previous tag plus
	// one and gives up on the child after five misses in a row, which a lost frame or the block-resend path would cause
	// (HandleSendFailure queues fragments unstamped, arriving as tag 0). Raw tags are still checked, for the log.
	bool resend = false;
	if (shim->rawTagValid && rawTag != ((shim->lastRawTag + 1) & 7)) {
		resend = rawTag == 0 && fragment && shim->cb.active && index + 1 < shim->cb.count;
		if (resend) {
			++shim->resendsRestamped;
			_log(shim, "GBA resent block fragment %u; re-stamping it", index);
		} else {
			++shim->tagGaps;
			_log(shim, "GBA command tag went %u -> %u", shim->lastRawTag, rawTag);
		}
	}
	if (!resend) { // a resend sits between two stamped frames
		shim->lastRawTag = rawTag;
		shim->rawTagValid = true;
	}
	shim->haveTag = true;
	if (command == RFUCMD_SEND_BLOCK_INIT) {
		uint16_t count = _rd16(slot + 2);
		if (!shim->cb.active || count != shim->cb.count || shim->cb.sent == 0) {
			shim->cb.active = count >= 1 && count <= 32;
			shim->cb.count = (uint8_t) count;
			shim->cb.sent = shim->cb.echoed = 0;
		}
	} else if (fragment && shim->cb.active && index < shim->cb.count) {
		memcpy(shim->cb.data[index], slot + 2, 12);
		shim->cb.sent |= 1u << index;
	}
	if (shim->requestType >= 0 && !shim->requestServed) {
		// The answer is a block of the requested size: its INIT or, should every INIT copy have been lost, all of its
		// fragments. A block the child sends on its own (its 20-byte trade menu messages) or the resend loop of an
		// earlier block is not.
		size_t type = (size_t) shim->requestType;
		if (type >= sizeof(kRequestFragments)) {
			shim->requestServed = true;
		} else if (command == RFUCMD_SEND_BLOCK_INIT) {
			shim->requestServed = _rd16(slot + 2) == kRequestFragments[type];
		} else if (fragment && index < 32) {
			uint32_t all = (1u << kRequestFragments[type]) - 1;
			shim->requestFrags |= 1u << index;
			shim->requestServed = (shim->requestFrags & all) == all;
		}
	}
	if (command != RFUCMD_READY_EXIT_STANDBY) {
		return 0;
	}

	int c = _rd16(slot + 2);
	int p = _toParent(shim, c);
	_wr16(slot + 2, (uint16_t) p);
	if (p > shim->lastChildRound) {
		shim->lastChildRound = p;
	}
	if (p != c) {
		_log(shim, "GBA standby round %d goes to the Switch as %d", c, p);
	}

	// The parent answered this barrier already and has left it; it will never answer again, so the child's retry means
	// the answer was lost on its way down. Complete the child's barrier on the parent's behalf. A late genuine copy is
	// ignored by the child, whose counter has moved on.
	if (p > shim->lastRound || replyCapacity < 2 * LDN_TRADE_SHIM_HOST_FRAME) {
		return 0;
	}
	size_t n = _buildHostFrame(reply, RFUCMD_READY_EXIT_STANDBY, (uint16_t) c, RFUCMD_READY_EXIT_STANDBY, (uint16_t) c);
	++shim->reanswers;
	_log(shim, "GBA is retrying standby round %d that the Switch already answered (as %d), answering for it", c, p);
	// A party request that arrived while the child was still inside that barrier was refused by its link layer and the
	// parent does not repeat it.
	if (shim->requestType >= 0 && !shim->requestServed && shim->requestAttempts < kMaxRerequests) {
		n += _buildHostFrame(reply + n, RFUCMD_SEND_BLOCK_REQ, (uint16_t) shim->requestType, 0, 0);
		++shim->requestAttempts;
		++shim->rerequests;
		shim->requestAt = nowMs;
		_log(shim, "repeating the Switch's party request %d to the GBA", shim->requestType);
	}
	return n;
}

size_t LdnTradeShimHost(struct LdnTradeShim* shim, uint8_t* payload, size_t length, int64_t nowMs, uint8_t* pre, size_t preCapacity) {
	size_t preLength = 0;
	if (length < 3) {
		return 0;
	}
	uint32_t header = payload[0] | (payload[1] << 8) | ((uint32_t) payload[2] << 16);
	if (((header >> 14) & 15) != 4) {
		return 0; // not a UNI frame
	}
	size_t size = header & 0x7F;
	if (size < 28 || 3 + size > length) {
		return 0;
	}
	for (int i = 0; i < 2; ++i) { // slot 0: the parent, slot 1: this child's echo
		uint8_t* slot = payload + 3 + 14 * i;
		uint16_t command = _rd16(slot);
		uint16_t value = _rd16(slot + 2);
		if (command == 0) {
			continue;
		}
		shim->quietSince = nowMs;
		if (i == 1) {
			shim->lastEchoMs = nowMs;
			shim->sentSinceEcho = 0;
			if (shim->echoStallReported) {
				_log(shim, "Switch is echoing the GBA's commands again");
			}
			shim->echoStallReported = false;
		}
		if (i == 1 && (command & 0xFF00) == RFUCMD_SEND_BLOCK && shim->cb.active) {
			uint8_t index = (uint8_t) (command & 0x1F);
			if (index < shim->cb.count) {
				shim->cb.echoed |= 1u << index;
			}
			if (index + 1 == shim->cb.count) {
				// The child acts on this echo: any earlier fragment it sent that was never echoed becomes a "send
				// failure". Supply those echoes first.
				uint32_t below = (1u << index) - 1;
				uint32_t missing = shim->cb.sent & ~shim->cb.echoed & below;
				for (uint8_t k = 0; k < index && preLength + LDN_TRADE_SHIM_HOST_FRAME <= preCapacity; ++k) {
					if (!(missing & (1u << k))) {
						continue;
					}
					uint8_t* f = pre + preLength;
					memset(f, 0, LDN_TRADE_SHIM_HOST_FRAME);
					memcpy(f, payload, 3); // same LLSF header as the real frame
					f[17] = k; // slot 1: the echo, tag already stripped
					f[18] = RFUCMD_SEND_BLOCK >> 8;
					memcpy(f + 19, shim->cb.data[k], 12);
					preLength += LDN_TRADE_SHIM_HOST_FRAME;
					shim->cb.echoed |= 1u << k;
					++shim->echoesSynthesized;
					_log(shim, "echo of the GBA's fragment %u never came back from the Switch, supplying it", k);
				}
			}
		}
		if (i == 0 && (command & 0xFF00) == RFUCMD_SEND_BLOCK && shim->pb.active) {
			// The parent sends each fragment once, so a gap here is a fragment the child can never receive.
			uint8_t index = (uint8_t) (command & 0x1F);
			if (index != shim->pb.expect && index + 1 != shim->pb.expect) {
				_log(shim, "Switch block fragment gap: expected %u, got %u", shim->pb.expect, index);
			}
			if (index >= shim->pb.expect) {
				shim->pb.expect = (uint8_t) (index + 1);
			}
		}
		if (i == 0) {
			if (command == RFUCMD_SEND_BLOCK_INIT) {
				shim->hostBlockCount = value;
				if (!shim->pb.active || (uint8_t) value != shim->pb.count || shim->pb.expect == shim->pb.count) {
					shim->pb.active = value >= 1 && value <= 32;
					shim->pb.count = (uint8_t) value;
					shim->pb.expect = 0;
				}
			} else if (command == RFUCMD_SEND_BLOCK && shim->hostBlockCount == 2 && value == LINKCMD_CONFIRM_FINISH_TRADE) {
				shim->postTrade = true;
				shim->injected = false;
				shim->answersSinceConfirm = 0;
				shim->fakeRound = -1;
				shim->fakeAcked = false;
				shim->attempts = 0;
				++shim->trades;
				_log(shim, "trade %d confirmed, watching the post-trade standby rounds", shim->trades);
			} else if (command == 0xED00 || command == 0xEE00) {
				_log(shim, "Switch sent a disconnect command %04X", command);
			} else if (command == RFUCMD_SEND_BLOCK_REQ) {
				shim->requestType = value;
				shim->requestServed = false;
				shim->requestAt = nowMs;
				shim->requestAttempts = 0;
				shim->requestFrags = 0;
				if (shim->postTrade) {
					shim->postTrade = false;
					shim->fakeAcked = true; // the parent has moved on, whatever it saw
					_log(shim, "Switch is requesting party data again");
				}
			}
		}
		if (command != RFUCMD_READY_EXIT_STANDBY) {
			continue;
		}
		if (_isFake(shim, value)) {
			// The parent's half of an injected round. The child is not in that round, so this must not reach it: a stray
			// matching command would pre-arm its next barrier.
			if (i == 0 && value == shim->fakeRound && !shim->fakeAcked) {
				shim->fakeAcked = true;
				_log(shim, "Switch answered the supplied round %d", value);
			}
			memset(slot, 0, 4);
			continue;
		}
		if (i == 0 && (int) value > shim->lastRound) {
			shim->lastRound = value;
			if (shim->postTrade) {
				++shim->answersSinceConfirm;
			}
		}
		_wr16(slot + 2, (uint16_t) _toChild(shim, value));
	}
	return preLength;
}

size_t LdnTradeShimInject(struct LdnTradeShim* shim, int64_t nowMs, uint8_t* out, size_t capacity) {
	if (capacity < LDN_TRADE_SHIM_CHILD_FRAME || !shim->postTrade || !shim->haveTag) {
		return 0;
	}
	int64_t quiet = nowMs - shim->quietSince;
	if (!shim->injected) {
		bool due = (shim->answersSinceConfirm >= kChildRounds && quiet >= kSettleMs) || (shim->answersSinceConfirm >= 1 && quiet >= kFallbackMs);
		if (!due) {
			return 0;
		}
		int next = shim->lastRound > shim->lastChildRound ? shim->lastRound : shim->lastChildRound;
		shim->fakeRound = next + 1;
		_addFake(shim, shim->fakeRound);
		shim->injected = true;
		shim->fakeAcked = false;
		shim->attempts = 1;
		shim->injectedAt = nowMs;
		shim->quietSince = nowMs;
		++shim->injections;
		_log(shim, "supplied standby round %d on the GBA's behalf after %d answered rounds", shim->fakeRound, shim->answersSinceConfirm);
		return _buildChildFrame(out, 0, (uint16_t) shim->fakeRound);
	}
	if (shim->fakeAcked || nowMs - shim->injectedAt < kAckMs) {
		return 0;
	}
	if (shim->attempts >= kMaxAttempts) {
		if (shim->attempts == kMaxAttempts) {
			++shim->attempts;
			_log(shim, "the Switch never answered the supplied round %d; giving up on it", shim->fakeRound);
		}
		return 0;
	}
	// Same round again. If the parent's game did see the first copy and its answer is merely late, this one arrives
	// after the round completed and is ignored as a number that no longer matches.
	++shim->attempts;
	shim->injectedAt = nowMs;
	_log(shim, "no answer to the supplied round %d, sending it again (attempt %d)", shim->fakeRound, shim->attempts);
	return _buildChildFrame(out, 0, (uint16_t) shim->fakeRound);
}

size_t LdnTradeShimHostInject(struct LdnTradeShim* shim, int64_t nowMs, uint8_t* out, size_t capacity) {
	if (capacity < LDN_TRADE_SHIM_HOST_FRAME || shim->requestType < 0 || shim->requestServed) {
		return 0;
	}
	if (shim->requestAttempts >= kMaxRerequests || nowMs - shim->requestAt < kRerequestMs || nowMs - shim->lastCommandMs < kRerequestMs) {
		return 0;
	}
	// The child's link layer refused the parent's block request (it was still inside its previous block send, where a
	// lost echo keeps it, or another command was pending) and the parent asks only once. The child accepts a repeat once
	// it is free; while it is still busy the repeat is refused the same way.
	++shim->requestAttempts;
	++shim->rerequests;
	shim->requestAt = nowMs;
	_log(shim, "GBA did not answer the Switch's block request %d, repeating it (attempt %d)", shim->requestType, shim->requestAttempts);
	return _buildHostFrame(out, RFUCMD_SEND_BLOCK_REQ, (uint16_t) shim->requestType, 0, 0);
}

void LdnTradeShimStamp(struct LdnTradeShim* shim, uint8_t* payload, size_t length) {
	if (length < 4 || !_isChildUni(payload) || payload[3] == 0) {
		return; // idle frames carry no tag
	}
	payload[2] = (uint8_t) ((shim->sendTag << 5) | (payload[2] & 0x1F));
	shim->sendTag = (uint8_t) ((shim->sendTag + 1) & 7);
	++shim->sentSinceEcho;
}

void LdnTradeShimPoll(struct LdnTradeShim* shim, int64_t nowMs) {
	// Commands keep going to the parent and none come back: its game has stopped taking the child's frames (a sequence
	// it rejected, or its own link error).
	if (!shim->echoStallReported && shim->sentSinceEcho >= kEchoStallFrames && shim->lastEchoMs && nowMs - shim->lastEchoMs >= kEchoStallMs) {
		shim->echoStallReported = true;
		_log(shim, "Switch stopped echoing the GBA's commands (%d unechoed)", shim->sentSinceEcho);
	}
}
