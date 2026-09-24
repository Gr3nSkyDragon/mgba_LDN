/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LDN_PIA_RELIABLE_H
#define GBA_SIO_LDN_PIA_RELIABLE_H

#include "ldn-pia.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Pia message tiling (a decrypted Pia payload is zero or more self-describing messages, an optional 2-byte
 * recipient-station footer, then 0xFF padding) and the Reliable(10) protocol riding inside it: a selective-repeat
 * sliding window carrying this project's actual RFU-adapter bytes (see rfu-broadcast.c) reliably and in order.
 *
 * Read from the `pokeldn` reference project's `pokeldn/ldn/reliable.py` (used only to learn the wire format and
 * the window algorithm - not copied source; see the project notes for the full field-by-field spec this was
 * built from). FRLG-specific: PROTO_RELIABLE=10, sequence numbers start at 0xFFF0 (not 0), flagsA bit 0 = this is
 * application data (vs. a pure ack), bit 3 = this is the stream-opening frame (must be sent exactly once, first).
 */

enum {
	LDN_PIA_PROTO_NET = 1,
	LDN_PIA_PROTO_RTT = 3,
	LDN_PIA_PROTO_RELIABLE = 10,
	LDN_PIA_PROTO_SESSION = 13,

	LDN_PIA_FLAGSA_APP_DATA = 0x01,
	LDN_PIA_FLAGSA_MSG_START = 0x02,
	LDN_PIA_FLAGSA_MSG_END = 0x04,
	LDN_PIA_FLAGSA_INITIALIZED = 0x08,
	LDN_PIA_FLAGSA_GBA = 0x07, // AppData | MsgStart | MsgEnd: an ordinary one-shot data frame
	LDN_PIA_FLAGSA_INIT = 0x0F, // FLAGSA_GBA | Initialized: the stream-opening frame, sent exactly once, first
	LDN_PIA_FLAGSA_CTRL = 0x00, // a pure (non-data) frame: always a bulk ack

	LDN_PIA_RELIABLE_START_SEQ = 0xFFF0,
	LDN_PIA_RELIABLE_MAX_INFLIGHT = 128, // the selective-ack mask spans 128 sequence ids; also this window's depth
	LDN_PIA_RELIABLE_RTO_BASE_MS = 33,
	LDN_PIA_RELIABLE_RTT_SAMPLES = 7,

	// The largest inner payload one Reliable(10) frame ever carries in this project: the RFU adapter's own
	// SendData payload (RFU_PACKET_MAX = 96 bytes, see rfu.h) or the fixed 46-byte stream-opening metadata frame,
	// with generous headroom. NOT the same thing as LDN_PIA_MAX_DATAGRAM (a whole encrypted Pia datagram, which
	// may batch several tiled messages together) - using that far larger size here would make
	// struct LdnPiaReliable's 2*128-slot windows well over a megabyte, too large to keep on a stack.
	LDN_PIA_RELIABLE_MAX_PAYLOAD = 512,
};

// The emulator's very first Reliable payload (sent as the stream-opening frame, FLAGSA_INIT, before any real RFU
// data): a fixed title/version metadata blob. Verbatim from the reference project; not otherwise interpreted by
// this project, just relayed unchanged since the host expects to see it before anything else.
extern const uint8_t kLdnPiaMetadataFrame[46];

// One tiled Pia message: `proto`/`payload`/`payloadLength` describe an inner message (e.g. a Reliable(10)
// sub-frame); `payload` points INTO the caller's decrypted buffer, not a copy.
struct LdnPiaMessage {
	bool haveMsgFlags;
	uint8_t msgFlags;
	uint8_t proto;
	const uint8_t* payload;
	size_t payloadLength;
};

// Parses `data` (a decrypted Pia payload, WITHOUT the trailing footer/0xFF padding stripped yet) into up to
// `maxMessages` tiled messages. Returns the number found; `*consumed` receives how many bytes of `data` the
// messages occupied (the remainder is the footer + padding, if any - see LdnPiaStripFooter).
size_t LdnPiaParseMessages(const uint8_t* data, size_t length, struct LdnPiaMessage* outMessages, size_t maxMessages, size_t* consumed);

// Encodes one self-describing tiled message (always with size+proto present, matching what this project ever
// sends - inheritance from a previous message's fields, which the wire format also allows, is never used here).
// `out` must be at least `payloadLength + 5` bytes (msgFlags is always included when `haveMsgFlags`, making the
// header a constant 5 bytes; 4 bytes when omitted). Returns the encoded length.
size_t LdnPiaBuildMessage(uint8_t proto, const uint8_t* payload, size_t payloadLength, bool haveMsgFlags, uint8_t msgFlags, uint8_t* out);

// Strips a trailing 2-byte big-endian station id (if `footerSize` == 2; any other value means "no footer") off
// `data[consumed:]`, ignoring 0xFF padding. Returns true and sets *outStation if a footer was present.
bool LdnPiaStripFooter(const uint8_t* data, size_t length, size_t consumed, uint8_t footerSize, uint16_t* outStation);

// The Reliable(10) sub-header: flagsA(1) size(2 BE) seq(2 BE) ack(2 BE, = the sender's own lowest still-unacked
// sequence, NOT an echo of anything we sent) N(1, multicast recipient count, always 0 here) then `size` payload
// bytes. 8 bytes fixed + payload.
struct LdnPiaReliableFrame {
	uint8_t flagsA;
	uint16_t seq;
	uint16_t ack;
	const uint8_t* payload;
	size_t payloadLength;
};

bool LdnPiaParseReliableFrame(const uint8_t* data, size_t length, struct LdnPiaReliableFrame* out);
// `out` must be at least `innerLength + 8` bytes. Returns the encoded length.
size_t LdnPiaBuildReliableFrame(uint16_t seq, uint16_t ack, uint8_t flagsA, const uint8_t* inner, size_t innerLength, uint8_t* out);

// A bulk ack's own payload (what rides as a Reliable(10) frame's `payload` when flagsA lacks APP_DATA):
// stream_id(1)=0 count(1)=1 next_expected(2 BE) mask(16). `out` must be at least 20 bytes.
size_t LdnPiaBuildBulkAck(uint16_t nextExpected, const uint8_t mask[16], uint8_t* out);
bool LdnPiaParseBulkAck(const uint8_t* data, size_t length, uint16_t* outNextExpected, uint8_t outMask[16]);

// One send-window slot (also used to report a delivered receive, via LdnPiaReliablePoll).
struct LdnPiaReliableEntry {
	uint16_t seq;
	uint8_t flagsA;
	uint8_t payload[LDN_PIA_RELIABLE_MAX_PAYLOAD];
	size_t length;
};

struct LdnPiaReliable {
	// Send side: a selective-repeat window, one slot per in-flight sequence id (indexed by seq % MAX_INFLIGHT).
	uint16_t outSeq;
	uint16_t windowLo; // the lowest sequence id we have sent but not yet seen acked
	bool localOpened;
	struct {
		bool used;
		uint16_t seq; // the slot index is seq % MAX_INFLIGHT, but the full 16-bit value is needed for comparisons
		uint8_t flagsA;
		uint8_t payload[LDN_PIA_RELIABLE_MAX_PAYLOAD];
		size_t length;
		uint32_t lastTxMs;
		unsigned resends;
		bool acked;
	} unacked[LDN_PIA_RELIABLE_MAX_INFLIGHT];

	// Receive side: both ack accounting and in-order delivery share one cursor (this project always delivers
	// synchronously, so - unlike the reference implementation - there is no reason to track them separately).
	uint16_t recvNext;
	bool peerOpened;
	struct {
		bool used;
		uint16_t seq;
		uint8_t payload[LDN_PIA_RELIABLE_MAX_PAYLOAD];
		size_t length;
	} recvBuf[LDN_PIA_RELIABLE_MAX_INFLIGHT]; // indexed by seq % MAX_INFLIGHT; holds arrivals ahead of recvNext
	bool ackOwed;
	bool haveNextAckMs;
	uint32_t nextAckMs;
	uint32_t ackPeriodMs;

	// RTT, for the retransmit timeout: RTO = RTO_BASE_MS + 1.4 * median(RTT), bootstrapped at rtoBootstrapMs
	// before the first sample.
	uint32_t rttSamples[LDN_PIA_RELIABLE_RTT_SAMPLES];
	unsigned rttCount;
	uint32_t rtoBootstrapMs;
};

void LdnPiaReliableInit(struct LdnPiaReliable*, uint32_t ackPeriodMs, uint32_t rtoBootstrapMs);

// Queues the stream-opening frame (FLAGSA_INIT). Must be called exactly once, before any LdnPiaReliableSend.
// Returns the assigned sequence id via *outSeq, or false if already opened.
bool LdnPiaReliableOpen(struct LdnPiaReliable*, const uint8_t* payload, size_t length, uint32_t nowMs, uint16_t* outSeq);
// Queues an ordinary data frame (FLAGSA_GBA). Returns false if the window is full (LDN_PIA_RELIABLE_MAX_INFLIGHT
// frames already unacked) or the stream has not been opened yet.
bool LdnPiaReliableSend(struct LdnPiaReliable*, const uint8_t* payload, size_t length, uint32_t nowMs, uint16_t* outSeq);

// Feeds one incoming Reliable(10) frame (already parsed by LdnPiaParseReliableFrame). A control frame (no
// APP_DATA flag) updates the send window from its bulk-ack payload and returns 0. A data frame updates the
// receive side and delivers newly-in-order payloads (including any that were buffered waiting for this one) into
// `outEntries` (up to `maxEntries`), returning how many. Pass a generous `maxEntries` (32 is ample for this
// project's traffic - realistic reordering depth over one local Wi-Fi hop is nowhere near that): a delivery run
// longer than `maxEntries` stops early and is NOT resumed by a later call with nothing new to feed it.
size_t LdnPiaReliableReceive(struct LdnPiaReliable*, const struct LdnPiaReliableFrame* frame, uint32_t nowMs, struct LdnPiaReliableEntry* outEntries,
                             size_t maxEntries);

// Frames due to go out right now: retransmits of unacked data (oldest first) followed by at most one bulk ack.
// Returns how many were written to `outEntries` (up to `maxEntries`); each has `seq`/`flagsA`/`payload`/`length`
// already filled in, ready for LdnPiaBuildReliableFrame (ack = LdnPiaReliableSendLow(link) at the moment of
// sending - see the .c file for why this needs to be resampled per emission, not stored in the entry).
size_t LdnPiaReliablePoll(struct LdnPiaReliable*, uint32_t nowMs, struct LdnPiaReliableEntry* outEntries, size_t maxEntries);

// The `ack` field value for a frame emitted right now (the window's own lowest still-unacked sequence, or the
// next sequence to be assigned if nothing is outstanding).
uint16_t LdnPiaReliableSendLow(const struct LdnPiaReliable*);

#endif
