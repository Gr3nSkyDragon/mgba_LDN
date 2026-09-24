/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ldn-pia-reliable.h"

#include <string.h>

const uint8_t kLdnPiaMetadataFrame[46] = {
    0x4a, 0x00, 0x2a, 0x00, 0x58, 0x01, 0x00, 0x4c, 0x65, 0x61, 0x66, 0x47, 0x72, 0x65, 0x65, 0x6e,
    0x5f, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// ---------------------------------------------------------------------------------------------------------------
// Message tiling
// ---------------------------------------------------------------------------------------------------------------

size_t LdnPiaParseMessages(const uint8_t* data, size_t length, struct LdnPiaMessage* outMessages, size_t maxMessages, size_t* consumed) {
	size_t i = 0;
	bool haveMf = false;
	uint8_t mf = 0;
	bool haveSize = false;
	uint16_t size = 0;
	uint8_t proto = 0;
	size_t count = 0;
	while (i < length) {
		uint8_t fl = data[i];
		if (fl == 0xFF || (fl & 0xF0)) {
			break;
		}
		if (fl == 0 && !haveSize) {
			break;
		}
		size_t j = i + 1;
		if (fl & 1) {
			if (j >= length) {
				break;
			}
			mf = data[j];
			haveMf = true;
			++j;
		}
		if (fl & 2) {
			if (j + 2 > length) {
				break;
			}
			size = (uint16_t) ((data[j] << 8) | data[j + 1]);
			haveSize = true;
			j += 2;
		}
		if (fl & 4) {
			if (j >= length) {
				break;
			}
			proto = data[j];
			++j;
		}
		if (fl & 8) {
			if (j + 1 > length) {
				break;
			}
			++j; // a 1-byte port field this project has no use for
		}
		if (!haveSize || j + size > length) {
			break;
		}
		if (count < maxMessages) {
			outMessages[count].haveMsgFlags = haveMf;
			outMessages[count].msgFlags = mf;
			outMessages[count].proto = proto;
			outMessages[count].payload = &data[j];
			outMessages[count].payloadLength = size;
		}
		++count;
		i = j + size;
	}
	if (consumed) {
		*consumed = i;
	}
	return count;
}

size_t LdnPiaBuildMessage(uint8_t proto, const uint8_t* payload, size_t payloadLength, bool haveMsgFlags, uint8_t msgFlags, uint8_t* out) {
	uint8_t flags = 0x02 | 0x04;
	size_t pos = 1;
	if (haveMsgFlags) {
		flags |= 0x01;
	}
	out[0] = flags;
	if (haveMsgFlags) {
		out[pos++] = msgFlags;
	}
	out[pos++] = (uint8_t) (payloadLength >> 8);
	out[pos++] = (uint8_t) payloadLength;
	out[pos++] = proto;
	memcpy(&out[pos], payload, payloadLength);
	return pos + payloadLength;
}

bool LdnPiaStripFooter(const uint8_t* data, size_t length, size_t consumed, uint8_t footerSize, uint16_t* outStation) {
	(void) footerSize; // informational only - matches the reference client's own logic, which does not gate on it either
	size_t end = length;
	while (end > consumed && data[end - 1] == 0xFF) {
		--end;
	}
	if (end - consumed != 2) {
		return false;
	}
	*outStation = (uint16_t) ((data[consumed] << 8) | data[consumed + 1]);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The Reliable(10) sub-header and its bulk-ack payload
// ---------------------------------------------------------------------------------------------------------------

bool LdnPiaParseReliableFrame(const uint8_t* data, size_t length, struct LdnPiaReliableFrame* out) {
	if (length < 8) {
		return false;
	}
	uint16_t size = (uint16_t) ((data[1] << 8) | data[2]);
	if ((size_t) 8 + size > length) {
		return false;
	}
	out->flagsA = data[0];
	out->seq = (uint16_t) ((data[3] << 8) | data[4]);
	out->ack = (uint16_t) ((data[5] << 8) | data[6]);
	// data[7] = N, the multicast recipient count; always 0 for a unicast link, not otherwise used here.
	out->payload = &data[8];
	out->payloadLength = size;
	return true;
}

size_t LdnPiaBuildReliableFrame(uint16_t seq, uint16_t ack, uint8_t flagsA, const uint8_t* inner, size_t innerLength, uint8_t* out) {
	out[0] = flagsA;
	out[1] = (uint8_t) (innerLength >> 8);
	out[2] = (uint8_t) innerLength;
	out[3] = (uint8_t) (seq >> 8);
	out[4] = (uint8_t) seq;
	out[5] = (uint8_t) (ack >> 8);
	out[6] = (uint8_t) ack;
	out[7] = 0;
	memcpy(&out[8], inner, innerLength);
	return 8 + innerLength;
}

size_t LdnPiaBuildBulkAck(uint16_t nextExpected, const uint8_t mask[16], uint8_t* out) {
	out[0] = 0; // stream id
	out[1] = 1; // entry count
	out[2] = (uint8_t) (nextExpected >> 8);
	out[3] = (uint8_t) nextExpected;
	memcpy(&out[4], mask, 16);
	return 20;
}

bool LdnPiaParseBulkAck(const uint8_t* data, size_t length, uint16_t* outNextExpected, uint8_t outMask[16]) {
	if (length < 4) {
		return false;
	}
	*outNextExpected = (uint16_t) ((data[2] << 8) | data[3]);
	memset(outMask, 0, 16);
	size_t maskLength = length - 4;
	if (maskLength > 16) {
		maskLength = 16;
	}
	memcpy(outMask, &data[4], maskLength);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The sliding window
// ---------------------------------------------------------------------------------------------------------------

// True if `a` is strictly before `b` in sequence order, accounting for 16-bit wraparound (a gap of half the
// space or more is treated as "not less than", matching the reference implementation's own rule).
static bool _seqLt(uint16_t a, uint16_t b) {
	uint16_t d = (uint16_t) (b - a);
	return d != 0 && d < 0x8000;
}

static bool _maskBit(const uint8_t mask[16], size_t i) {
	return i < 128 && ((mask[i / 8] >> (i % 8)) & 1) != 0;
}

void LdnPiaReliableInit(struct LdnPiaReliable* link, uint32_t ackPeriodMs, uint32_t rtoBootstrapMs) {
	memset(link, 0, sizeof(*link));
	link->outSeq = LDN_PIA_RELIABLE_START_SEQ;
	link->windowLo = LDN_PIA_RELIABLE_START_SEQ;
	link->recvNext = LDN_PIA_RELIABLE_START_SEQ;
	link->ackPeriodMs = ackPeriodMs;
	link->rtoBootstrapMs = rtoBootstrapMs;
}

uint16_t LdnPiaReliableSendLow(const struct LdnPiaReliable* link) {
	for (size_t i = 0; i < LDN_PIA_RELIABLE_MAX_INFLIGHT; ++i) {
		if (link->unacked[i].used) {
			return link->windowLo;
		}
	}
	return link->outSeq;
}

static bool _queue(struct LdnPiaReliable* link, const uint8_t* payload, size_t length, uint8_t flagsA, uint32_t nowMs, uint16_t* outSeq) {
	if (length > LDN_PIA_RELIABLE_MAX_PAYLOAD) {
		return false;
	}
	uint16_t seq = link->outSeq;
	size_t slot = seq % LDN_PIA_RELIABLE_MAX_INFLIGHT;
	if (link->unacked[slot].used) {
		return false; // the window is full
	}
	link->unacked[slot].used = true;
	link->unacked[slot].seq = seq;
	link->unacked[slot].flagsA = flagsA;
	memcpy(link->unacked[slot].payload, payload, length);
	link->unacked[slot].length = length;
	link->unacked[slot].lastTxMs = nowMs;
	link->unacked[slot].resends = 0;
	link->unacked[slot].acked = false;
	link->outSeq = (uint16_t) (seq + 1);
	*outSeq = seq;
	return true;
}

bool LdnPiaReliableOpen(struct LdnPiaReliable* link, const uint8_t* payload, size_t length, uint32_t nowMs, uint16_t* outSeq) {
	if (link->localOpened) {
		return false;
	}
	if (!_queue(link, payload, length, LDN_PIA_FLAGSA_INIT, nowMs, outSeq)) {
		return false;
	}
	link->localOpened = true;
	return true;
}

bool LdnPiaReliableSend(struct LdnPiaReliable* link, const uint8_t* payload, size_t length, uint32_t nowMs, uint16_t* outSeq) {
	if (!link->localOpened) {
		return false;
	}
	return _queue(link, payload, length, LDN_PIA_FLAGSA_GBA, nowMs, outSeq);
}

static void _addRttSample(struct LdnPiaReliable* link, uint32_t rttMs) {
	link->rttSamples[link->rttCount % LDN_PIA_RELIABLE_RTT_SAMPLES] = rttMs;
	++link->rttCount;
}

static void _onAck(struct LdnPiaReliable* link, uint16_t ackId, const uint8_t mask[16], uint32_t nowMs) {
	for (size_t slot = 0; slot < LDN_PIA_RELIABLE_MAX_INFLIGHT; ++slot) {
		if (!link->unacked[slot].used || link->unacked[slot].acked) {
			continue;
		}
		uint16_t seq = link->unacked[slot].seq;
		bool arrived = _seqLt(seq, ackId);
		if (!arrived) {
			uint16_t i = (uint16_t) (seq - ackId - 1); // MASK_ORIGIN_OFFSET = 1
			arrived = _maskBit(mask, i);
		}
		if (arrived) {
			if (link->unacked[slot].resends == 0) {
				_addRttSample(link, nowMs - link->unacked[slot].lastTxMs);
			}
			link->unacked[slot].acked = true;
		}
	}
	while (true) {
		size_t slot = link->windowLo % LDN_PIA_RELIABLE_MAX_INFLIGHT;
		if (link->unacked[slot].used && link->unacked[slot].seq == link->windowLo && link->unacked[slot].acked) {
			link->unacked[slot].used = false;
			link->windowLo = (uint16_t) (link->windowLo + 1);
		} else {
			break;
		}
	}
}

size_t LdnPiaReliableReceive(struct LdnPiaReliable* link, const struct LdnPiaReliableFrame* frame, uint32_t nowMs, struct LdnPiaReliableEntry* outEntries,
                             size_t maxEntries) {
	if (!(frame->flagsA & LDN_PIA_FLAGSA_APP_DATA)) {
		uint16_t ackId;
		uint8_t mask[16];
		if (LdnPiaParseBulkAck(frame->payload, frame->payloadLength, &ackId, mask)) {
			_onAck(link, ackId, mask, nowMs);
		}
		return 0;
	}
	if (!link->peerOpened && frame->seq == LDN_PIA_RELIABLE_START_SEQ && !(frame->flagsA & LDN_PIA_FLAGSA_INITIALIZED)) {
		// The opening slot must carry Initialized; a plain frame there is not a valid stream start.
		return 0;
	}
	if (frame->flagsA & LDN_PIA_FLAGSA_INITIALIZED) {
		link->peerOpened = true;
	}
	link->ackOwed = true;
	if (!link->haveNextAckMs) {
		link->nextAckMs = nowMs + link->ackPeriodMs;
		link->haveNextAckMs = true;
	}

	size_t count = 0;
	if (frame->seq == link->recvNext) {
		if (count < maxEntries && frame->payloadLength <= LDN_PIA_RELIABLE_MAX_PAYLOAD) {
			outEntries[count].seq = frame->seq;
			outEntries[count].flagsA = frame->flagsA;
			memcpy(outEntries[count].payload, frame->payload, frame->payloadLength);
			outEntries[count].length = frame->payloadLength;
			++count;
		}
		link->recvNext = (uint16_t) (link->recvNext + 1);
		while (count < maxEntries) {
			size_t slot = link->recvNext % LDN_PIA_RELIABLE_MAX_INFLIGHT;
			if (!link->recvBuf[slot].used || link->recvBuf[slot].seq != link->recvNext) {
				break;
			}
			outEntries[count].seq = link->recvNext;
			outEntries[count].flagsA = LDN_PIA_FLAGSA_GBA;
			memcpy(outEntries[count].payload, link->recvBuf[slot].payload, link->recvBuf[slot].length);
			outEntries[count].length = link->recvBuf[slot].length;
			link->recvBuf[slot].used = false;
			++count;
			link->recvNext = (uint16_t) (link->recvNext + 1);
		}
	} else if (_seqLt(link->recvNext, frame->seq)) {
		size_t slot = frame->seq % LDN_PIA_RELIABLE_MAX_INFLIGHT;
		if (frame->payloadLength <= LDN_PIA_RELIABLE_MAX_PAYLOAD) {
			link->recvBuf[slot].used = true;
			link->recvBuf[slot].seq = frame->seq;
			memcpy(link->recvBuf[slot].payload, frame->payload, frame->payloadLength);
			link->recvBuf[slot].length = frame->payloadLength;
		}
	}
	// else: at or below recvNext already delivered - a duplicate, ignored for delivery (still acked above).
	return count;
}

static bool _rto(const struct LdnPiaReliable* link, uint32_t* outRto) {
	size_t n = link->rttCount < LDN_PIA_RELIABLE_RTT_SAMPLES ? link->rttCount : LDN_PIA_RELIABLE_RTT_SAMPLES;
	if (n > 0) {
		uint32_t sorted[LDN_PIA_RELIABLE_RTT_SAMPLES];
		memcpy(sorted, link->rttSamples, n * sizeof(uint32_t));
		for (size_t i = 1; i < n; ++i) {
			uint32_t key = sorted[i];
			size_t j = i;
			while (j > 0 && sorted[j - 1] > key) {
				sorted[j] = sorted[j - 1];
				--j;
			}
			sorted[j] = key;
		}
		uint32_t median = sorted[n / 2];
		*outRto = (uint32_t) (LDN_PIA_RELIABLE_RTO_BASE_MS + 1.4 * median);
		return true;
	}
	if (link->rtoBootstrapMs) {
		*outRto = link->rtoBootstrapMs;
		return true;
	}
	return false;
}

size_t LdnPiaReliablePoll(struct LdnPiaReliable* link, uint32_t nowMs, struct LdnPiaReliableEntry* outEntries, size_t maxEntries) {
	size_t count = 0;
	uint32_t rto;
	bool haveRto = _rto(link, &rto);

	if (haveRto) {
		for (uint32_t offset = 0; offset < LDN_PIA_RELIABLE_MAX_INFLIGHT && count < maxEntries; ++offset) {
			uint16_t seq = (uint16_t) (link->windowLo + offset);
			size_t slot = seq % LDN_PIA_RELIABLE_MAX_INFLIGHT;
			if (!link->unacked[slot].used || link->unacked[slot].seq != seq || link->unacked[slot].acked) {
				continue;
			}
			if (nowMs - link->unacked[slot].lastTxMs < rto) {
				continue;
			}
			link->unacked[slot].lastTxMs = nowMs;
			++link->unacked[slot].resends;
			outEntries[count].seq = seq;
			outEntries[count].flagsA = link->unacked[slot].flagsA;
			memcpy(outEntries[count].payload, link->unacked[slot].payload, link->unacked[slot].length);
			outEntries[count].length = link->unacked[slot].length;
			++count;
		}
	}

	if (count < maxEntries) {
		bool haveOoo = false;
		for (size_t i = 0; i < LDN_PIA_RELIABLE_MAX_INFLIGHT; ++i) {
			if (link->recvBuf[i].used) {
				haveOoo = true;
				break;
			}
		}
		bool ackDue = link->haveNextAckMs && nowMs >= link->nextAckMs;
		if (ackDue && (link->ackOwed || haveOoo)) {
			uint8_t mask[16] = {0};
			for (size_t slot = 0; slot < LDN_PIA_RELIABLE_MAX_INFLIGHT; ++slot) {
				if (!link->recvBuf[slot].used) {
					continue;
				}
				uint16_t i = (uint16_t) (link->recvBuf[slot].seq - link->recvNext - 1);
				if (i < 128) {
					mask[i / 8] |= (uint8_t) (1 << (i % 8));
				}
			}
			outEntries[count].seq = LDN_PIA_RELIABLE_START_SEQ;
			outEntries[count].flagsA = LDN_PIA_FLAGSA_CTRL;
			outEntries[count].length = LdnPiaBuildBulkAck(link->recvNext, mask, outEntries[count].payload);
			++count;
			link->ackOwed = false;
			link->haveNextAckMs = haveOoo;
			if (haveOoo) {
				link->nextAckMs = nowMs + link->ackPeriodMs;
			}
		}
	}
	return count;
}
