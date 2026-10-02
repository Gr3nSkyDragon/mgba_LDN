/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LDN_LDND_H
#define GBA_SIO_LDN_LDND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Client for ldnd, the LDN daemon, over its named pipe (Windows). Speaks protocol version 7 (see ldnrs's
 * docs/daemon-protocol.md).
 *
 * ldnd owns the Wi-Fi adapter and prod.keys. It scans for LDN networks and joins one (WPA2 association and LDN's own
 * authentication), and then hands out channels on the joined network. A datagram channel is a UDP socket in ldnd's
 * kernel, bound to the wildcard address so that the host's broadcasts reach it as well as datagrams addressed to us.
 * A connection selects LDN in its Hello (LdndOpen does that) and may have one network open at a time. ldnd serves one
 * connection at a time and refuses a second one's Hello with BUSY - including, briefly, a reconnect racing ldnd's
 * cleanup of our own previous connection.
 *
 * Frames are `u8 op, u8 sub_op, u32 request_id, u32 body_length` (little endian, 10 bytes), then the body. Each
 * request is answered by a Reply carrying its request_id. Replies can come out of order (a scan answers from its own
 * task), so any number of threads may each be waiting for one. Events and incoming data are unsolicited: they are
 * handed to the LdndCallbacks on the connection's reader thread, and a callback must not make requests itself (their
 * replies are read by the very thread it would be blocking).
 */

enum {
	LDND_PROTOCOL_VERSION = 7,

	LDND_MAX_PARTICIPANTS = 8,
	LDND_MAX_APPLICATION_DATA = 0x180,
	LDND_MAX_PARTICIPANT_NAME = 32,
	LDND_MAX_MESSAGE = 200,
};

// What a request returns: LDND_OK, one of ldnd's own statuses (positive, from its reply), or a client-side failure
// (negative).
enum {
	LDND_OK = 0,

	LDND_STATUS_BAD_REQUEST = 1,
	LDND_STATUS_UNSUPPORTED_VERSION = 2,
	LDND_STATUS_INVALID_HANDLE = 3,
	LDND_STATUS_INVALID_PARAM = 4,
	LDND_STATUS_BUSY = 5,
	LDND_STATUS_NO_RADIO = 6,
	LDND_STATUS_NO_KEYS = 7,
	LDND_STATUS_NOT_FOUND = 8,
	LDND_STATUS_TIMEOUT = 9, // the join failed: the network was not found or the association dropped
	LDND_STATUS_AUTH_FAILED = 10, // the host refused the join; see LdndNetworkReply.authStatus
	LDND_STATUS_IO = 11,
	LDND_STATUS_UNSUPPORTED = 12,
	LDND_STATUS_INTERNAL = 13,

	LDND_ERR_PIPE = -1, // ldnd is not running, or the pipe is gone (see LdndAbort)
	LDND_ERR_TIMEOUT = -2, // no reply in time
	LDND_ERR_ARGS = -3, // the request could not be built
	LDND_ERR_PROTOCOL = -4, // ldnd sent something that does not decode
};

// Bits of LdndHello's capabilities.
enum {
	LDND_CAP_SCAN = 1 << 0,
	LDND_CAP_JOIN = 1 << 1,
	LDND_CAP_HOST = 1 << 2,
};

enum LdndRadioState {
	LDND_RADIO_IDLE = 0,
	LDND_RADIO_ATTACHING = 1,
	LDND_RADIO_READY = 2,
	LDND_RADIO_LOST = 3,
	LDND_RADIO_FAILED = 4,
};

enum LdndEventKind {
	LDND_EVENT_NETWORK_FOUND = 1,
	LDND_EVENT_SCAN_DONE = 2,
	LDND_EVENT_JOIN = 3,
	LDND_EVENT_LEAVE = 4,
	LDND_EVENT_DISCONNECT = 5,
	LDND_EVENT_APP_DATA_CHANGED = 6,
	LDND_EVENT_POLICY_CHANGED = 7,
	LDND_EVENT_CHANNEL_ERROR = 8,
	LDND_EVENT_LOG = 9,
	LDND_EVENT_RADIO_STATE = 10,
	LDND_EVENT_LOG_DROPPED = 11,
};

// The failure a request reports: its code (as returned) and, when there is one, ldnd's own explanation or a
// description of what went wrong on this side.
struct LdndResult {
	int code;
	char message[LDND_MAX_MESSAGE];
};

struct LdndHello {
	uint32_t protocolVersion;
	char daemonVersion[64];
	uint8_t ldnCapabilities; // LDND_CAP_*
	uint8_t nwmCapabilities;
	bool radioReady;
};

struct LdndParticipant {
	uint8_t ip[4];
	uint8_t mac[6];
	bool connected;
	uint8_t nameLength;
	uint8_t name[LDND_MAX_PARTICIPANT_NAME]; // not NUL-terminated
	uint16_t appVersion;
	uint8_t platform;
};

// A network as ldnd describes it: decoded from the host's advertisement by a scan, or the joined network's latest
// state in a Connect reply. Connect takes one of the networks a scan returned, unchanged.
struct LdndNetworkInfo {
	uint8_t protocol;
	uint8_t address[6]; // the host's MAC (BSSID)
	uint8_t band;
	uint8_t channel;
	uint64_t localCommunicationId;
	uint16_t sceneId;
	uint8_t ssid[16];
	uint8_t version;
	uint8_t serverRandom[16];
	uint16_t securityMode;
	uint16_t appVersion;
	uint8_t acceptPolicy;
	uint8_t maxParticipants;
	uint8_t numParticipants;
	// Every slot, connected or not (ldnd sends all eight): a participant's index is its position. The host is 0.
	uint8_t participantCount;
	struct LdndParticipant participants[LDND_MAX_PARTICIPANTS];
	uint16_t applicationDataLength;
	uint8_t applicationData[LDND_MAX_APPLICATION_DATA];
	uint64_t challenge;
	uint8_t nonce[4];
};

// One unsolicited event. `kind` says which of the other fields are set; pointers are only valid during the callback.
struct LdndEvent {
	enum LdndEventKind kind;
	uint32_t handle; // the network the event concerns (the channel, for CHANNEL_ERROR); 0 if none

	const struct LdndNetworkInfo* network; // NETWORK_FOUND
	uint32_t count; // SCAN_DONE: networks found; LOG_DROPPED: lines dropped
	uint8_t index; // JOIN, LEAVE: the participant's slot
	const struct LdndParticipant* participant; // JOIN, LEAVE
	uint8_t reason; // DISCONNECT: 3 destroyed, 4 destroyed forcefully, 5 rejected by host, 6 connection lost
	const uint8_t* oldData; // APP_DATA_CHANGED
	size_t oldDataLength;
	const uint8_t* newData;
	size_t newDataLength;
	uint8_t oldPolicy; // POLICY_CHANGED
	uint8_t newPolicy;
	int status; // CHANNEL_ERROR: an LDND_STATUS_*
	uint8_t radioState; // RADIO_STATE: an LdndRadioState
	const char* message; // CHANNEL_ERROR, RADIO_STATE (may be NULL), LOG: NUL-terminated
};

struct LdndCallbacks {
	void* context;
	void (*event)(void* context, const struct LdndEvent* event);
	// Everything a channel received, one DATA frame at a time: for a datagram channel a `peer, port, datagram` body
	// (see LdndParseDatagram), for a raw one an Ethernet frame. Valid only during the call.
	void (*data)(void* context, uint32_t channel, const uint8_t* payload, size_t length);
};

// Opens the pipe (NULL means \\.\pipe\ldnd) and says Hello as `clientName`/`clientVersion`, which is how ldnd
// names this client to anyone it refuses with BUSY. `callbacks` may be NULL; it is copied. Returns NULL when ldnd is
// not running or refused the Hello: `result` says why (code LDND_ERR_PIPE, or ldnd's status such as BUSY with
// ldnd's explanation), and `hello` is filled in either way when ldnd answered.
struct LdndConnection* LdndOpen(const char* pipePath, const char* clientName, const char* clientVersion, const struct LdndCallbacks* callbacks,
                                struct LdndHello* hello, struct LdndResult* result);
// Closes the pipe and frees the connection; ldnd then closes whatever network and channels it held. No request may be
// in progress on another thread (see LdndAbort).
void LdndClose(struct LdndConnection*);
// Safe from any thread at any time: every request in progress, and every later one, fails with LDND_ERR_PIPE, and
// ldnd sees the pipe close. The connection still has to be closed.
void LdndAbort(struct LdndConnection*);
// False once the pipe has failed or been aborted.
bool LdndIsOpen(struct LdndConnection*);

// `result` may be NULL on every call below; the return value is its code.

struct LdndScanRequest {
	// The channels to visit, in order, and how long to listen on each. No channels / 0 ms leave it to ldnd (1, 6 and
	// 11, at its default dwell). ldnd also waits ~100 ms after each retune before it starts listening.
	const uint8_t* channels;
	size_t channelCount;
	uint32_t dwellMs;
};

// Scans, and returns once the whole scan is done (roughly `channels x (dwell + 100 ms)`); NETWORK_FOUND events arrive
// while it runs. Fills `networks` with at most `maxNetworks` of what was found and `*count` with how many that is.
// Never scan while a network is open: ldnd retunes the radio for it, taking the joined network down.
int LdndScan(struct LdndConnection*, const struct LdndScanRequest*, struct LdndNetworkInfo* networks, size_t maxNetworks, size_t* count,
             struct LdndResult* result);
// Asks a scan in progress to stop early, and returns without waiting for ldnd's answer (which is always success,
// scan or not) - so it is cheap enough for a thread that must not block, and the scan's own LdndScan call returns
// what it had found so far. Only fails when the pipe does.
int LdndScanCancel(struct LdndConnection*);

struct LdndConnectRequest {
	const struct LdndNetworkInfo* network; // one a scan returned
	const uint8_t* password; // the game's LDN passphrase: it goes into the link key, so a wrong one joins and then
	size_t passwordLength; //   hears nothing
	const char* name; // this participant's name, as others see it (at most 32 bytes)
	uint16_t appVersion;
	uint8_t platform; // 0 Switch, 1 Switch 2
	bool enableChallenge;
	uint64_t deviceId;
	// The join budget ldnd gets, retries included; 0 leaves it unbounded. ldnd stops starting new attempts when it
	// runs out, but can overrun it by up to one association timeout. The call itself waits a while longer than this.
	uint32_t timeoutMs;
};

struct LdndNetworkReply {
	uint32_t handle; // the network; 0 when the join failed
	bool haveAuthStatus;
	uint8_t authStatus; // LDN's AUTH_* code when the host refused (LDND_STATUS_AUTH_FAILED)
	bool haveNetwork;
	struct LdndNetworkInfo network; // the joined network, with us in its participant table
	uint8_t participantIndex; // our slot in network.participants
};

// Joins a network; this blocks while ldnd associates and authenticates, which can take several seconds and, with
// ldnd's retries, far longer. `reply` is filled in on failure too (authStatus).
int LdndConnect(struct LdndConnection*, const struct LdndConnectRequest*, struct LdndNetworkReply* reply, struct LdndResult* result);
// Leaves the network, closing its channels first.
int LdndCloseNetwork(struct LdndConnection*, uint32_t network, struct LdndResult* result);

// Opens a UDP channel on a joined network, bound to `port` (0 lets ldnd's kernel choose; `*boundPort` says which).
int LdndOpenDatagram(struct LdndConnection*, uint32_t network, uint16_t port, uint32_t* channel, uint16_t* boundPort, struct LdndResult* result);
int LdndCloseChannel(struct LdndConnection*, uint32_t channel, struct LdndResult* result);

// Sends one datagram from a datagram channel to `peer`:`port`. Not acknowledged: a failure arrives later as a
// CHANNEL_ERROR event. Returns LDND_OK or LDND_ERR_PIPE/LDND_ERR_ARGS. (ldnd cannot send broadcasts yet - unicast to
// each participant.)
int LdndSendDatagram(struct LdndConnection*, uint32_t channel, const uint8_t peer[4], uint16_t port, const void* data, size_t length);
// Splits a datagram channel's DATA payload (see LdndCallbacks.data) into its sender and the datagram.
bool LdndParseDatagram(const uint8_t* payload, size_t length, uint8_t peer[4], uint16_t* port, const uint8_t** data, size_t* dataLength);

// A short name for a request's result code ("BUSY", "NO_RADIO", "pipe closed", ...).
const char* LdndStatusName(int code);

#endif
