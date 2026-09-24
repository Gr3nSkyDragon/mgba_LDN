/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ldn-pia-connect.h"

#include "ldn-pia-reliable.h" // LDN_PIA_PROTO_*
#include "ldn.h" // LdnRandomBytes

#include <string.h>

static void _queueOut(struct LdnPiaConnect* c, uint8_t proto, uint16_t dst, uint16_t src, bool establishing, bool footer, bool compress,
                      const uint8_t* payload, size_t length) {
	if (c->outboxCount >= LDN_PIA_CONNECT_MAX_OUTBOX || length > LDN_PIA_CONNECT_MAX_MESSAGE) {
		return;
	}
	struct LdnPiaOutMessage* m = &c->outbox[c->outboxCount++];
	m->proto = proto;
	m->dst = dst;
	m->src = src;
	m->establishing = establishing;
	m->footer = footer;
	m->compress = compress;
	memcpy(m->payload, payload, length);
	m->length = length;
}

void LdnPiaConnectInit(struct LdnPiaConnect* c, const uint8_t ourMac[6], const uint8_t hostMac[6], const uint8_t ourIp[4], const char* playerName) {
	memset(c, 0, sizeof(*c));
	memcpy(c->ourMac, ourMac, 6);
	memcpy(c->hostMac, hostMac, 6);
	memcpy(c->ourIp, ourIp, 4);
	c->ourVar = LDN_PIA_DEFAULT_OUR_VAR;
	size_t nameLen = strlen(playerName);
	if (nameLen > 20) {
		nameLen = 20;
	}
	memcpy(c->playerName, playerName, nameLen);
	c->playerId[7] = 0x01; // DEFAULT_PLAYER_ID: all zero except byte 7 = 0x01
	LdnRandomBytes(c->random4, 4);
	c->state = LDN_PIA_CONNECT_ST_NET;
}

bool LdnPiaConnectIsConnected(const struct LdnPiaConnect* c) {
	return c->state == LDN_PIA_CONNECT_ST_CONNECTED;
}

// ---------------------------------------------------------------------------------------------------------------
// Net(1)
// ---------------------------------------------------------------------------------------------------------------

// `parse_net`: type=payload[0] (always 1, the Net protocol's own marker byte, distinct from the Pia message's own
// `proto` tiling field), subtype=payload[1], a 2-byte size field at [2:4] this project does not use, body=[4:].
static bool _parseNetConnRequest(const uint8_t* payload, size_t length, uint16_t* hostVar, uint8_t hostMac[6], uint32_t* seqid) {
	if (length < 4 || payload[1] != LDN_PIA_NET_CONN_REQUEST) {
		return false;
	}
	const uint8_t* body = &payload[4];
	size_t bodyLength = length - 4;
	if (bodyLength < 12) {
		return false;
	}
	*seqid = ((uint32_t) body[0] << 24) | ((uint32_t) body[1] << 16) | ((uint32_t) body[2] << 8) | body[3];
	*hostVar = (uint16_t) ((body[4] << 8) | body[5]);
	memcpy(hostMac, &body[6], 6);
	return true;
}

static size_t _buildNetResponse(uint32_t seqid, uint8_t* out) {
	out[0] = 0x01;
	out[1] = LDN_PIA_NET_CONN_RESPONSE;
	out[2] = 0;
	out[3] = 0;
	out[4] = (uint8_t) (seqid >> 24);
	out[5] = (uint8_t) (seqid >> 16);
	out[6] = (uint8_t) (seqid >> 8);
	out[7] = (uint8_t) seqid;
	return 8;
}

// ---------------------------------------------------------------------------------------------------------------
// Session(13)
// ---------------------------------------------------------------------------------------------------------------

static const uint8_t kProtocols[6][2] = {{1, 0}, {3, 5}, {5, 1}, {10, 3}, {13, 7}, {15, 0}};

static size_t _buildSessionJoin(const struct LdnPiaConnect* c, uint8_t* out) {
	size_t pos = 0;
	out[pos++] = LDN_PIA_SESSION_JOIN_REQUEST;
	out[pos++] = (uint8_t) (sizeof(kProtocols) / sizeof(kProtocols[0]));
	for (size_t i = 0; i < sizeof(kProtocols) / sizeof(kProtocols[0]); ++i) {
		out[pos++] = kProtocols[i][0];
		out[pos++] = kProtocols[i][1];
	}
	out[pos++] = 0x00;
	out[pos++] = 0x58; // app_ver
	memcpy(&out[pos], c->random4, 4);
	pos += 4;
	memcpy(&out[pos], c->ourMac, 6);
	pos += 6;
	out[pos++] = 0;
	out[pos++] = 0; // source constant id padding (MAC + 0000 = 8 bytes)
	out[pos++] = (uint8_t) (c->ourVar >> 8);
	out[pos++] = (uint8_t) c->ourVar;
	out[pos++] = 0;
	out[pos++] = 0; // NAT mapping, is-private-IPv6
	memset(&out[pos], 0, 32); // identification token
	pos += 32;
	memcpy(&out[pos], c->hostMac, 6);
	pos += 6;
	out[pos++] = 0;
	out[pos++] = 0; // dest constant id padding
	out[pos++] = (uint8_t) (c->hostVar >> 8);
	out[pos++] = (uint8_t) c->hostVar;
	out[pos++] = 1;
	out[pos++] = 1; // num players, num participants
	out[pos++] = 0; // StationAddress: an unused leading byte, then IPv4 + port
	memcpy(&out[pos], c->ourIp, 4);
	pos += 4;
	out[pos++] = (uint8_t) (LDN_PIA_PORT >> 8);
	out[pos++] = (uint8_t) LDN_PIA_PORT;
	memcpy(&out[pos], c->playerId, 16); // PlayerInfo
	pos += 16;
	size_t nameLen = strlen(c->playerName);
	if (nameLen > 20) {
		nameLen = 20;
	}
	out[pos++] = 0;
	out[pos++] = 0;
	out[pos++] = 0;
	out[pos++] = (uint8_t) nameLen; // name length, big-endian u32
	out[pos++] = 1; // encoding
	memcpy(&out[pos], c->playerName, nameLen);
	pos += nameLen;
	return pos;
}

static size_t _buildSessionFinalize(const uint8_t ourMac[6], uint8_t* out) {
	// Session type 6: `06 <our_mac:6> 0000 0000000000 01` - references OUR OWN constant id, not the host's.
	out[0] = LDN_PIA_SESSION_UPDATE_ACK;
	memcpy(&out[1], ourMac, 6);
	out[7] = 0;
	out[8] = 0;
	memset(&out[9], 0, 5);
	out[14] = 1;
	return 15;
}

// ---------------------------------------------------------------------------------------------------------------
// RTT(3)
// ---------------------------------------------------------------------------------------------------------------

static bool _parseRttType(const uint8_t* payload, size_t length, uint8_t* type) {
	if (length < 16) {
		return false;
	}
	*type = payload[0];
	return true;
}

static size_t _buildRttResponse(const uint8_t* request, size_t length, uint8_t* out) {
	size_t n = length < 21 ? length : 21;
	memset(out, 0, 21);
	memcpy(out, request, n);
	out[0] = 1; // type 1 = response; the timestamp at [8:16] is echoed unchanged
	return 21;
}

// ---------------------------------------------------------------------------------------------------------------

void LdnPiaConnectOnMessage(struct LdnPiaConnect* c, uint8_t proto, const uint8_t* payload, size_t length) {
	if (proto == LDN_PIA_PROTO_NET) {
		if (length >= 2 && payload[1] == LDN_PIA_NET_CONN_REQUEST && c->state == LDN_PIA_CONNECT_ST_NET) {
			uint16_t hostVar = 0;
			uint8_t hostMac[6];
			uint32_t seqid = 2;
			if (_parseNetConnRequest(payload, length, &hostVar, hostMac, &seqid)) {
				memcpy(c->hostMac, hostMac, 6);
				if (!c->haveHostVar) {
					c->hostVar = hostVar;
					c->haveHostVar = true;
				}
			}
			uint8_t resp[8];
			size_t respLength = _buildNetResponse(seqid, resp);
			_queueOut(c, LDN_PIA_PROTO_NET, 0, 0, true, false, false, resp, respLength);
			if (c->haveHostVar) {
				uint8_t join[LDN_PIA_CONNECT_MAX_MESSAGE];
				size_t joinLength = _buildSessionJoin(c, join);
				_queueOut(c, LDN_PIA_PROTO_SESSION, 0, c->ourVar, true, false, true, join, joinLength);
			}
		} else if (length >= 8 && (payload[1] == LDN_PIA_NET_CONN_REQUEST || payload[1] == LDN_PIA_NET_UPDATE_PROPERTY || payload[1] == LDN_PIA_NET_KEEP_ALIVE)) {
			// Every other host net request - a repeated connection request, a network status update (0x11 again after the
			// join), a network property update (0x50) or a keep-alive (0x80) - repeats every ~500 ms until answered with the
			// request's type + 1 and the same sequence id (GB-Link's firmware, pia_conn.c: a property update left
			// unanswered is retried for ~10 minutes, then the host stops taking this station's traffic). The host only
			// takes the acknowledgement in the form it uses itself - packet id 0, establishing, no footer - never with a
			// running packet id (which is what this branch used to send). Two copies go out: source 0 (the form it takes)
			// and source = our station id (the form it uses itself); a duplicate is harmless.
			uint32_t seqid = ((uint32_t) payload[4] << 24) | ((uint32_t) payload[5] << 16) | ((uint32_t) payload[6] << 8) | payload[7];
			uint8_t ack[8] = {0x01, (uint8_t) (payload[1] + 1), 0, 0, (uint8_t) (seqid >> 24), (uint8_t) (seqid >> 16), (uint8_t) (seqid >> 8),
			                  (uint8_t) seqid};
			_queueOut(c, LDN_PIA_PROTO_NET, 0, 0, true, false, false, ack, sizeof(ack));
			_queueOut(c, LDN_PIA_PROTO_NET, 0, c->ourVar, true, false, false, ack, sizeof(ack));
		}
	} else if (proto == LDN_PIA_PROTO_SESSION) {
		uint8_t type = length > 0 ? payload[0] : 0xFF;
		if (length > 0 && type == LDN_PIA_SESSION_UPDATE && c->haveHostVar && c->state != LDN_PIA_CONNECT_ST_CONNECTED) {
			uint8_t fin[15];
			size_t finLength = _buildSessionFinalize(c->ourMac, fin);
			_queueOut(c, LDN_PIA_PROTO_SESSION, c->hostVar, c->ourVar, false, true, false, fin, finLength);
		}
		if (length > 0 && c->state == LDN_PIA_CONNECT_ST_NET) {
			c->state = LDN_PIA_CONNECT_ST_FINALIZE;
		}
	} else if (proto == LDN_PIA_PROTO_RTT || proto == LDN_PIA_PROTO_RELIABLE) {
		if (c->state == LDN_PIA_CONNECT_ST_FINALIZE) {
			c->state = LDN_PIA_CONNECT_ST_CONNECTED;
		}
		// The native client does not answer RTT before it has left ST_NET (byte-checked against a real capture in the
			// reference: zero RTT responses during the join); answering them earlier is not what the host expects.
			if (proto == LDN_PIA_PROTO_RTT && c->haveHostVar && c->state != LDN_PIA_CONNECT_ST_NET) {
			uint8_t type;
			if (_parseRttType(payload, length, &type) && type == 0) {
				size_t n = length < 21 ? length : 21;
				memcpy(c->lastHostRtt, payload, n);
				c->haveLastHostRtt = true;
				uint8_t resp[21];
				_buildRttResponse(payload, length, resp);
				_queueOut(c, LDN_PIA_PROTO_RTT, LDN_PIA_SESSION_VAR, c->ourVar, false, true, false, resp, 21);
			}
		}
	}
}

void LdnPiaConnectTick(struct LdnPiaConnect* c, unsigned tick) {
	if (!LdnPiaConnectIsConnected(c) || !c->haveLastHostRtt || !c->haveHostVar) {
		return;
	}
	if (tick - c->lastRttOriginTick < LDN_PIA_RTT_ORIGINATE_PERIOD_TICKS) {
		return;
	}
	c->lastRttOriginTick = tick;
	uint8_t req[21];
	memset(req, 0, sizeof(req));
	memcpy(req, c->lastHostRtt, 21);
	req[0] = 0; // type 0 = request
	// bytes [8:16] are a fresh timestamp, little-endian (the one field in this whole layer that is NOT big-endian
	// - measured off the reference client's own build_rtt_request). A free-running tick count is good enough:
	// only used to match this probe's later type-1 reply, never interpreted by anything else.
	uint64_t systime = ((uint64_t) tick << 16) | 0x1000;
	for (int i = 0; i < 8; ++i) {
		req[8 + i] = (uint8_t) (systime >> (8 * i));
	}
	_queueOut(c, LDN_PIA_PROTO_RTT, LDN_PIA_SESSION_VAR, c->ourVar, false, true, false, req, sizeof(req));
}

size_t LdnPiaConnectDrain(struct LdnPiaConnect* c, struct LdnPiaOutMessage* out, size_t maxOut) {
	size_t n = c->outboxCount < maxOut ? c->outboxCount : maxOut;
	memcpy(out, c->outbox, n * sizeof(struct LdnPiaOutMessage));
	if (n < c->outboxCount) {
		memmove(&c->outbox[0], &c->outbox[n], (c->outboxCount - n) * sizeof(struct LdnPiaOutMessage));
	}
	c->outboxCount -= n;
	return n;
}
