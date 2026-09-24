/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LDN_PIA_CONNECT_H
#define GBA_SIO_LDN_PIA_CONNECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The Pia CONNECTION layer: the Net(1)/Session(13)/RTT(3) handshake a host completes before it will accept this
 * joiner's Reliable(10) data at all. Read from the `pokeldn` reference project's `pokeldn/ldn/pia_connect.py`
 * (its FRLG/Pia-6.32+ `ConnectionManager` - that file also documents an unrelated "Pia 6.16-6.30 (version 11)"
 * layout for other titles, not used here) - used only to learn the wire format and the state machine, not
 * copied source.
 *
 * State machine (this side only ever REACTS to the host - nothing here advances on its own send, matching the
 * reference client's own design: if an outgoing packet is lost, the host's own retransmit of what prompted it
 * re-triggers our reply on the next incoming copy):
 *
 *   ST_NET       waiting for the host's Net 0x11 (broadcast to the LDN subnet's .255 address, then unicast).
 *                On receipt: reply Net 0x12 (echoing its seqid) AND immediately send our Session join (type 0).
 *   ST_FINALIZE  entered on ANY Session-protocol reply. On a Session type-5 UPDATE, send Session type 6
 *                (`build_session_finalize`).
 *   ST_CONNECTED entered on the FIRST RTT or Reliable-protocol message seen after finalizing.
 *
 * zstd compression: the reference client sends its Session join zstd-compressed and nothing else on this layer
 * (see pokeldn/frlgsim's `ConnectionManager.on_message`, the only `_q(..., compress=True, ...)` call site) -
 * matches live capture, which showed the host's own Net/Session messages arriving zstd-compressed too (see
 * ldn-pia.c's LdnPiaDecompress/LdnPiaCompress). `LdnPiaOutMessage.compress` is set true only for the Session
 * join queued below; the caller (ldn-pia-join.c) is responsible for actually compressing the tiled bytes via
 * LdnPiaCompress before appending any footer/padding and encrypting.
 */

enum {
	LDN_PIA_NET_CONN_REQUEST = 0x11,
	LDN_PIA_NET_CONN_RESPONSE = 0x12,
	LDN_PIA_NET_UPDATE_PROPERTY = 0x50,
	LDN_PIA_NET_UPDATE_PROPERTY_ACK = 0x51,

	LDN_PIA_SESSION_JOIN_REQUEST = 0,
	LDN_PIA_SESSION_JOIN_RESPONSE = 2,
	LDN_PIA_SESSION_UPDATE = 5,
	LDN_PIA_SESSION_UPDATE_ACK = 6,
	LDN_PIA_SESSION_LEFT_SYNC = 7,

	LDN_PIA_SESSION_VAR = 0x0001, // RTT/session control messages address this pseudo-station in the Pia header's dst
	LDN_PIA_DEFAULT_OUR_VAR = 0xc493, // STATION_JOINER, matching ldn-pia.c's crypto.py-derived default

	LDN_PIA_CONNECT_ST_NET = 0,
	LDN_PIA_CONNECT_ST_FINALIZE = 1,
	LDN_PIA_CONNECT_ST_CONNECTED = 2,

	LDN_PIA_CONNECT_MAX_OUTBOX = 4,
	LDN_PIA_CONNECT_MAX_MESSAGE = 256, // generous for the largest message this sends, the Session join (~100 bytes)

	LDN_PIA_RTT_ORIGINATE_PERIOD_TICKS = 10,
};

// One message queued for the Pia message-tiling/reliable-window layer above to actually send (see ldn-pia.c /
// ldn-pia-reliable.c). `establishing` means "force pktid 0 and do not route through the Reliable window" -
// Net/Session/RTT control traffic rides its own unreliable/best-effort channel, never the Reliable(10) stream.
struct LdnPiaOutMessage {
	uint8_t proto;
	uint16_t dst;
	uint16_t src;
	bool establishing;
	// Whether the datagram carrying this message should append the 2-byte recipient-station-id footer (the
	// value is always the host's own var id, for every message this project ever sends one on).
	bool footer;
	// Whether the tiled message blob should be zstd-compressed before the footer/padding is appended and the
	// datagram encrypted. The reference client (pokeldn/frlgsim ConnectionManager.on_message) compresses exactly
	// one outgoing message on this whole layer - the Session join - and nothing else; matches the live-observed
	// fact that the HOST's own Net/Session messages arrive zstd-compressed too (see ldn-pia.c's LdnPiaDecompress).
	bool compress;
	uint8_t payload[LDN_PIA_CONNECT_MAX_MESSAGE];
	size_t length;
};

struct LdnPiaConnect {
	uint8_t ourMac[6];
	uint8_t hostMac[6]; // learned from the host's own Net 0x11 body - see the .c file; the constructor value is a
	                    // placeholder (e.g. the LDN participant MAC) used only until the real one arrives
	uint8_t ourIp[4];
	uint16_t ourVar;
	uint16_t hostVar;
	bool haveHostVar;
	char playerName[21];
	uint8_t random4[4];
	uint8_t playerId[16];

	int state;

	struct LdnPiaOutMessage outbox[LDN_PIA_CONNECT_MAX_OUTBOX];
	size_t outboxCount;

	bool haveLastHostRtt;
	uint8_t lastHostRtt[21];
	unsigned rttOriginateTick;
	unsigned lastRttOriginTick;
};

// `playerName` is truncated to 20 bytes. Generates a fresh random4 nonce via LdnRandomBytes (ldn.c).
void LdnPiaConnectInit(struct LdnPiaConnect*, const uint8_t ourMac[6], const uint8_t hostMac[6], const uint8_t ourIp[4], const char* playerName);

// Feeds one already-decrypted-and-tiled incoming Pia message (its `proto` and inner `payload`). May queue
// replies (drain with LdnPiaConnectDrain).
void LdnPiaConnectOnMessage(struct LdnPiaConnect*, uint8_t proto, const uint8_t* payload, size_t length);

// Called once per tick (any regular cadence, e.g. once per emulated frame) so periodic behaviour (originating an
// RTT probe once connected) has a clock. `tick` is a simple incrementing counter, not a millisecond time.
void LdnPiaConnectTick(struct LdnPiaConnect*, unsigned tick);

// Pulls up to `maxOut` queued outgoing messages; returns how many.
size_t LdnPiaConnectDrain(struct LdnPiaConnect*, struct LdnPiaOutMessage* out, size_t maxOut);

bool LdnPiaConnectIsConnected(const struct LdnPiaConnect*);

#endif
