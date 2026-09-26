/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LDN_TRADE_SHIM_H
#define GBA_SIO_LDN_TRADE_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Reconciles the link-standby barrier between a retail FireRed/LeafGreen cartridge (the emulated GBA, link child) and
 * the Switch release of the same game (link parent). Ported from GB-Link-Switch-LDN's firmware/common/trade_shim.c,
 * which does the same job for the ESP32 bridge; the decision logic is kept as it is there, so the two stay comparable.
 * The firmware's reset-surviving event log, adapter telemetry and log dump have no counterpart here: every decision
 * goes to the RFU trace instead.
 *
 * After a trade both games run numbered RFU "ready to exit standby" rounds around their saves before reopening the
 * trade menu: the child sends READY_EXIT_STANDBY with its round counter, the parent answers with the same number once
 * it reaches that barrier, and a command whose number differs from the receiver's counter is ignored. The Switch
 * release (pokefirered's REVISION 0xA, trade_scene.c CB2_SaveAndEndTrade cases 43/44) runs one round more than a
 * retail cartridge, so the parent waits for a round the child never sends while the child waits for the party request
 * that only follows it: both stay at "Communication standby".
 *
 * The shim supplies that round on the child's behalf once per trade, after the cartridge has finished its own rounds
 * and the link has gone quiet, and from then on maps round numbers between the two sides. The parent also validates a
 * mod-8 sequence tag on every child command, so the injected frame consumes a tag and the child's later tags are
 * re-stamped to stay consecutive.
 *
 * Safeguards: an injected round the parent never answers is re-sent; a parent answer the child never saw (it keeps
 * retrying a barrier the parent already left) is answered on the parent's behalf; a missing echo of one of the
 * child's block fragments is supplied before the echo the child acts on; and a block request the child refused is
 * repeated to it.
 *
 * Frames are the RFU UNI frames exactly as they cross the adapter: child -> parent is a 2-byte LLSF header and one
 * 14-byte command slot (16 bytes), parent -> child a 3-byte LLSF header and five 14-byte slots (73 bytes). Times are
 * milliseconds on any monotonic clock.
 */

enum {
	LDN_TRADE_SHIM_MAX_FAKES = 16,
	LDN_TRADE_SHIM_CHILD_FRAME = 16,
	LDN_TRADE_SHIM_HOST_FRAME = 73,
};

typedef void (*LdnTradeShimLog)(void* user, const char* message);

struct LdnTradeShim {
	LdnTradeShimLog log;
	void* logUser;

	uint16_t fakes[LDN_TRADE_SHIM_MAX_FAKES]; // injected rounds, parent numbering, ascending
	int fakeCount;
	int base; // fakes older than the list, folded into a plain offset
	uint8_t sendTag; // the sequence tag the next frame sent to the parent gets
	bool haveTag; // the child has sent a command: its stream is established
	bool postTrade; // between the parent's trade confirmation and its next block request
	bool injected; // this trade's extra round has been supplied
	int lastRound; // parent numbering of the parent's newest standby answer, -1 none
	int lastChildRound; // parent numbering of the newest forwarded child standby command
	int answersSinceConfirm;
	int fakeRound; // this trade's extra round, parent numbering, -1 none
	bool fakeAcked;
	int attempts;
	int64_t injectedAt;
	int64_t quietSince;
	int hostBlockCount; // fragment count announced by the parent's last block
	int requestType; // parent's newest block request, -1 none
	bool requestServed; // the child has started answering it
	int64_t requestAt; // when it (or its latest repeat) went to the child
	int requestAttempts;
	uint32_t requestFrags; // fragment indices the child has sent since the request
	bool rawTagValid;
	uint8_t lastRawTag;
	// The child's block in flight: the parent echoes each accepted fragment once, in slot 1, and the child's link layer
	// treats a missing echo as a send failure it cannot repair (its resends carry no sequence tag).
	struct {
		bool active;
		uint8_t count;
		uint32_t sent, echoed;
		uint8_t data[32][12];
	} cb;
	// The parent's block in flight, for gap reporting only.
	struct {
		bool active;
		uint8_t count, expect;
	} pb;
	// The child's newest command frame exactly as the GBA sent it.
	uint8_t lastFrame[LDN_TRADE_SHIM_CHILD_FRAME];
	bool haveLastFrame;
	int64_t lastCommandMs;
	int64_t lastEchoMs;
	int sentSinceEcho;
	bool echoStallReported;

	int trades, injections, reanswers, rerequests, echoesSynthesized, resendsRestamped, tagGaps, duplicates;
};

void LdnTradeShimInit(struct LdnTradeShim*, LdnTradeShimLog log, void* logUser);
// A new session: forgets every round, tag and block. (Not for a link-loss reconnect within a session, where both games
// keep their counters.)
void LdnTradeShimReset(struct LdnTradeShim*);

// Child -> parent frame, rewritten in place. `*forward` is cleared for a frame that must not be sent (an exact repeat of
// the previous one). When the child is retrying a barrier the parent already answered, one or two parent frames for the
// child are written to `reply` and their total length returned.
size_t LdnTradeShimChild(struct LdnTradeShim*, uint8_t* payload, size_t length, int64_t nowMs, uint8_t* reply, size_t replyCapacity, bool* forward);

// A child frame about to go to the parent (called for exactly the frames that are sent, in order): stamps the next
// consecutive sequence tag on a command frame.
void LdnTradeShimStamp(struct LdnTradeShim*, uint8_t* payload, size_t length);

// Parent -> child frame, rewritten in place. When the parent's echo of the child's last block fragment arrives while
// earlier echoes never did, the missing echo frames are written to `pre` and their total length returned: they must
// reach the child before this frame.
size_t LdnTradeShimHost(struct LdnTradeShim*, uint8_t* payload, size_t length, int64_t nowMs, uint8_t* pre, size_t preCapacity);

// The extra child round when it is due (or a re-send of one the parent has not answered): writes a child frame and
// returns its length, or returns 0. The frame still goes through LdnTradeShimStamp on its way out.
size_t LdnTradeShimInject(struct LdnTradeShim*, int64_t nowMs, uint8_t* out, size_t capacity);

// A repeat of the parent's block request when the child has not started answering it: writes a parent frame for the
// child and returns its length, or returns 0. Polled alongside LdnTradeShimInject.
size_t LdnTradeShimHostInject(struct LdnTradeShim*, int64_t nowMs, uint8_t* out, size_t capacity);

// Periodic checks (the parent no longer echoing the child's commands); reporting only.
void LdnTradeShimPoll(struct LdnTradeShim*, int64_t nowMs);

#endif
