/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/rfu-broadcast.h>

#include "ldn-pia-connect.h"
#include "ldn-pia-reliable.h"
#include "ldn-pia.h"
#include "ldn.h"
#include "ldnd.h"

#include <mgba/core/version.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

enum {
	// The channels a Switch running FRLG's Direct Corner has been seen to use (it moves between them). Each scan visits
	// one, so the worker gets to look at what the game wants between channels.
	kChannelCount = 3,
	kScanDwellMs = 400,

	// English FireRed's compat+serial word (see rfu.c's BCAST trace of a real cartridge). The Switch's own beacon
	// does not carry this, so it is filled in here. TODO: unconfirmed for LeafGreen or other languages/versions -
	// revisit once a LeafGreen Switch capture is available.
	kAssumedCompat = 0x13820002,
	// The only adapter "activity" this project cares about (see rfu.c's command trace of a real cartridge).
	kTradeActivity = 0x04,

	// Rooms remembered from recent scans, for the game to connect to by trainer id.
	kMaxRooms = 8,
	kMaxScanResults = 8,

	// ldnd's budget for a join. Measured on an RTL8822BU, one attempt is ~30 s of driver reconfiguration over USB, so a
	// smaller budget does not fail faster, it just fails (ldnrs's compat layer uses the same figure).
	kJoinBudgetMs = 70000,

	// How long the worker will wait for the Pia CONNECTION layer (Net/Session/RTT - see ldn-pia-connect.c) to reach
	// ST_CONNECTED after ldnd's join, before giving up on the whole connect attempt. Generous vs. the live-observed real
	// timing (well under a second in every live test).
	kPiaConnectTimeoutMs = 8000,

	// How long to wait before trying ldnd again after it could not be reached, or could not scan.
	kRetryMs = 2000,
	// The connection to ldnd is let go after this long with nothing to do (other clients are refused meanwhile), but
	// not in the moment between the game's search and its connect.
	kIdleReleaseMs = 10000,

	// While the game searches, the host being joined (or already joined) is re-reported this often, in frames: the
	// driver forgets a host after ~4 s of silence, and there is no scanning while ldnd holds a network.
	kTargetReportFrames = 60,
	// A session the game never got to use (the join outlasted its patience) is kept for its next attempt, but only
	// this many frames without the game searching or connecting.
	kLingerFrames = 60 * 60,

	kPiaQueueDepth = 32,

	// The largest single tiled message this backend ever sends: a Reliable(10) frame wrapping up to
	// LDN_PIA_RELIABLE_MAX_PAYLOAD bytes of inner payload (8-byte sub-header + payload), plus the message-tiling
	// layer's own 5-byte header, a 2-byte footer and up to 15 bytes of 0xFF padding.
	kPiaMaxTiled = 5 + 8 + LDN_PIA_RELIABLE_MAX_PAYLOAD + 2 + 16,
};

static const uint8_t kChannels[kChannelCount] = {1, 6, 11};

// Pia's header packet id is a per-CHANNEL counter keyed by the header's destination var-id, each channel counting
// from 1 (skipping 0 on rollover) - NOT one global counter (pokeldn/frlgsim sim.py `_next_pktid`: "keeps
// independent counters per dst - dst=0x0001 session/RTT, dst=host-var reliable/data ... so the reliable channel
// stays contiguous even when RTT/Session frames interleave on their own dst"). A single shared counter left gaps
// in the reliable channel's ids, and the host answered our RTT but silently ignored every Reliable datagram.
struct PiaPktids {
	uint16_t dst[4];
	uint16_t next[4];
	unsigned count;
};

static uint16_t _nextPktid(struct PiaPktids* ids, uint16_t dst) {
	for (unsigned i = 0; i < ids->count; ++i) {
		if (ids->dst[i] == dst) {
			uint16_t id = ids->next[i];
			ids->next[i] = id < 0xFFFF ? id + 1 : 1;
			return id;
		}
	}
	if (ids->count < sizeof(ids->dst) / sizeof(ids->dst[0])) {
		ids->dst[ids->count] = dst;
		ids->next[ids->count] = 2;
		++ids->count;
		return 1;
	}
	return 1;
}

struct Room {
	bool valid;
	uint16_t trainerId;
	uint32_t lastSeen;
	struct LdndNetworkInfo network;
};

struct PiaDatagram {
	uint8_t ip[4];
	size_t length;
	uint8_t payload[LDN_PIA_MAX_DATAGRAM];
};

/*
 * ldnd (see ldnd.h) does the radio work: it scans, joins a room (WPA2 association and LDN authentication), and hands
 * us a UDP channel on the joined network. What is left here is to turn scan results into rooms for the game, and to
 * run Pia - the Switch game's own session layer - over that channel.
 *
 * Threading: everything that waits on ldnd (connecting to it, scanning, joining, leaving) happens on the worker thread
 * (_workerProc), started by the first search or connect and stopped by deinit. A join in particular can take a minute.
 * The emulation thread and ldnd's reader thread (the callbacks) only post to the worker, under `lock`, and wake it.
 */
struct GBASIORFUBroadcast {
	struct GBASIORFUBackend d;
	struct GBASIORFU* rfu;

#ifdef _WIN32
	HANDLE worker;
	HANDLE wake;
	CRITICAL_SECTION lock;
#endif

	// Guarded by `lock`.
	bool stopping; // deinit: the worker is to exit
	bool searching; // the game is reading broadcasts
	bool connectWanted; // the game asked to join connectDeviceId and has had no answer yet
	uint16_t connectDeviceId;
	bool leaveWanted; // the Pia session is over; the worker is to close its network
	bool hostLost; // ldnd reported the network gone, or ldnd itself went away
	struct LdndConnection* conn; // opened and closed by the worker; anyone may use it while holding `lock`
	struct Room rooms[kMaxRooms];
	uint16_t targetDeviceId; // the host being joined or joined to (by trainer id), 0 for none
	struct LdndNetworkInfo target; // its network: from the scan, then the join's reply, then ldnd's events
	uint32_t network; // the joined network's handle, 0 for none
	uint32_t channel; // its Pia datagram channel
	struct PiaDatagram queue[kPiaQueueDepth]; // datagrams received on it, for whoever is driving Pia
	size_t queueHead;
	size_t queueCount;
	bool piaActive; // see below

	// The worker's own.
	unsigned scanIndex;
	int lastOpenError;
	int lastScanError;
	unsigned nonRoomsHeard;

	// The emulation thread's own.
	unsigned targetReportFrames;
	unsigned lingerFrames;

	// The live Pia session. The worker brings it up on its own and hands it to the emulation thread by setting
	// `piaActive` (under `lock`); from then until the emulation thread clears `piaActive` again (_endPiaSession), only
	// the emulation thread touches these.
	uint16_t piaDeviceId;
	struct LdnPiaCrypto piaCrypto;
	struct LdnPiaConnect piaConn;
	struct LdnPiaReliable* piaReliable; // heap-allocated - too large for an inline struct member (see the project notes)
	bool piaOpenedStream;
	uint8_t piaOurIp[4];
	uint8_t piaHostIp[4];
	uint16_t piaHostVar; // resynced from piaConn.hostVar once known
	uint64_t piaNonceCounter;
	struct PiaPktids piaPktids;
	unsigned piaTick;

	// Emulator-frame layer on top of the Reliable stream (see _gba* helpers): the Switch host does not understand raw
	// RFU bytes, only `57 <type> <len:u16 LE> <body>` frames - a 'C' connect request from us, its 'A' accept, 'T' slot
	// carriers both ways, and a 'K' ack from us for every host 'T'.
	uint16_t piaConnectId; // our self-chosen, nonzero RFU connection id (the host just echoes it back)
	bool piaConnectQueued; // 'C' has been queued
	bool piaHostTSeen; // the host's own 'T' slot stream has started (its first idle keepalive arrived)
	bool piaAccepted; // the host's 'A' arrived - only then do the game's slots go out as 'T' frames
	uint32_t piaTs; // per-NEW-frame 'T' counter
	uint32_t piaKSeq; // joiner-global 'K' counter (+1 from 1)
};

#ifdef _WIN32
static const char* _radioStateName(unsigned state) {
	static const char* const names[] = {"idle", "attaching", "ready", "lost", "failed"};
	return state < sizeof(names) / sizeof(names[0]) ? names[state] : "unknown";
}

static void _wakeWorker(struct GBASIORFUBroadcast* broadcast) {
	SetEvent(broadcast->wake);
}

// Sleeps up to `ms`, or until the worker is woken.
static void _nap(struct GBASIORFUBroadcast* broadcast, DWORD ms) {
	WaitForSingleObject(broadcast->wake, ms);
}

// ---- Traffic on the joined network --------------------------------------------------------------------------------

static int _sendDatagram(struct GBASIORFUBroadcast* broadcast, const uint8_t peer[4], const uint8_t* data, size_t length) {
	int code = LDND_ERR_PIPE;
	EnterCriticalSection(&broadcast->lock);
	if (broadcast->conn && broadcast->channel) {
		code = LdndSendDatagram(broadcast->conn, broadcast->channel, peer, LDN_PIA_PORT, data, length);
	}
	LeaveCriticalSection(&broadcast->lock);
	return code;
}

// Dequeues one received datagram (false if there is none). `*inOutLength` is the buffer size on entry and the
// datagram's length on return; `ip` is who sent it.
static bool _popDatagram(struct GBASIORFUBroadcast* broadcast, uint8_t ip[4], uint8_t* payload, size_t* inOutLength) {
	bool found = false;
	EnterCriticalSection(&broadcast->lock);
	if (broadcast->queueCount) {
		struct PiaDatagram* datagram = &broadcast->queue[broadcast->queueHead];
		memcpy(ip, datagram->ip, 4);
		size_t copy = datagram->length < *inOutLength ? datagram->length : *inOutLength;
		memcpy(payload, datagram->payload, copy);
		*inOutLength = datagram->length;
		broadcast->queueHead = (broadcast->queueHead + 1) % kPiaQueueDepth;
		--broadcast->queueCount;
		found = true;
	}
	LeaveCriticalSection(&broadcast->lock);
	return found;
}

// ldnd's reader thread: a datagram arrived on the Pia channel.
static void _onData(void* context, uint32_t channel, const uint8_t* payload, size_t length) {
	struct GBASIORFUBroadcast* broadcast = context;
	uint8_t peer[4];
	uint16_t port;
	const uint8_t* data;
	size_t dataLength;
	if (!LdndParseDatagram(payload, length, peer, &port, &data, &dataLength)) {
		return;
	}
	EnterCriticalSection(&broadcast->lock);
	// The channel is only known once OpenDatagram answers, and the host may already be talking by then.
	bool ours = broadcast->network && (!broadcast->channel || channel == broadcast->channel);
	// A full queue drops the newest datagram: Pia's own reliable layer recovers from loss, so this queue is not the place
	// to grow without bound.
	if (ours && broadcast->queueCount < kPiaQueueDepth) {
		struct PiaDatagram* datagram = &broadcast->queue[(broadcast->queueHead + broadcast->queueCount) % kPiaQueueDepth];
		memcpy(datagram->ip, peer, 4);
		datagram->length = dataLength < sizeof(datagram->payload) ? dataLength : sizeof(datagram->payload);
		memcpy(datagram->payload, data, datagram->length);
		++broadcast->queueCount;
	}
	LeaveCriticalSection(&broadcast->lock);
}

// ldnd's reader thread.
static void _onEvent(void* context, const struct LdndEvent* event) {
	struct GBASIORFUBroadcast* broadcast = context;
	switch (event->kind) {
	case LDND_EVENT_DISCONNECT: {
		EnterCriticalSection(&broadcast->lock);
		bool ours = event->handle && event->handle == broadcast->network;
		if (ours) {
			broadcast->hostLost = true;
		}
		LeaveCriticalSection(&broadcast->lock);
		if (ours) {
			GBASIORFUTrace(broadcast->rfu, "LDN    ldnd: the host's network is gone (reason %u)", event->reason);
			_wakeWorker(broadcast);
		}
		break;
	}
	case LDND_EVENT_APP_DATA_CHANGED:
		EnterCriticalSection(&broadcast->lock);
		if (event->handle && event->handle == broadcast->network && event->newDataLength <= LDND_MAX_APPLICATION_DATA) {
			memcpy(broadcast->target.applicationData, event->newData, event->newDataLength);
			broadcast->target.applicationDataLength = (uint16_t) event->newDataLength;
		}
		LeaveCriticalSection(&broadcast->lock);
		break;
	case LDND_EVENT_JOIN:
	case LDND_EVENT_LEAVE: {
		const struct LdndParticipant* participant = event->participant;
		GBASIORFUTrace(broadcast->rfu, "LDN    ldnd: participant %u %s: \"%.*s\" %u.%u.%u.%u", event->index,
		               event->kind == LDND_EVENT_JOIN ? "joined" : "left", participant->nameLength, (const char*) participant->name, participant->ip[0],
		               participant->ip[1], participant->ip[2], participant->ip[3]);
		break;
	}
	case LDND_EVENT_CHANNEL_ERROR:
		GBASIORFUTrace(broadcast->rfu, "LDN    ldnd: channel %u: %s: %s", event->handle, LdndStatusName(event->status), event->message);
		break;
	case LDND_EVENT_RADIO_STATE:
		GBASIORFUTrace(broadcast->rfu, "LDN    ldnd: radio %s%s%s", _radioStateName(event->radioState), event->message ? ": " : "",
		               event->message ? event->message : "");
		// A scan that is waiting out NO_RADIO can go again.
		_wakeWorker(broadcast);
		break;
	default:
		break;
	}
}

// ---- The worker ---------------------------------------------------------------------------------------------------

static struct LdndConnection* _openDaemon(struct GBASIORFUBroadcast* broadcast) {
	// LDN_DAEMON names a pipe other than ldnd's default, as ldnrs's own clients do.
	const char* pipe = getenv("LDN_DAEMON");
	if (pipe && !pipe[0]) {
		pipe = NULL;
	}
	struct LdndCallbacks callbacks = {broadcast, _onEvent, _onData};
	struct LdndHello hello;
	struct LdndResult result;
	struct LdndConnection* conn = LdndOpen(pipe, projectName, projectVersion, &callbacks, &hello, &result);
	if (!conn) {
		// Once per distinct problem: the worker retries every few seconds for as long as the game keeps searching.
		if (result.code != broadcast->lastOpenError) {
			GBASIORFUTrace(broadcast->rfu, "LDN    could not use ldnd: %s%s%s", LdndStatusName(result.code), result.message[0] ? ": " : "",
			               result.message);
		}
		broadcast->lastOpenError = result.code;
		return NULL;
	}
	broadcast->lastOpenError = LDND_OK;
	GBASIORFUTrace(broadcast->rfu, "LDN    connected to ldnd %s (protocol %u)%s", hello.daemonVersion, hello.protocolVersion,
	               hello.radioReady ? "" : "; its radio is not ready yet");
	EnterCriticalSection(&broadcast->lock);
	broadcast->conn = conn;
	LeaveCriticalSection(&broadcast->lock);
	return conn;
}

static struct LdndConnection* _daemon(struct GBASIORFUBroadcast* broadcast) {
	EnterCriticalSection(&broadcast->lock);
	struct LdndConnection* conn = broadcast->conn;
	LeaveCriticalSection(&broadcast->lock);
	return conn ? conn : _openDaemon(broadcast);
}

// Hangs up on ldnd, which then drops whatever network it held for us.
static void _closeDaemon(struct GBASIORFUBroadcast* broadcast) {
	EnterCriticalSection(&broadcast->lock);
	struct LdndConnection* conn = broadcast->conn;
	broadcast->conn = NULL;
	if (broadcast->network) {
		broadcast->hostLost = true;
	}
	LeaveCriticalSection(&broadcast->lock);
	// Outside the lock: closing waits for ldnd's reader thread, whose callbacks take it.
	LdndClose(conn);
}

// Closes the joined network and forgets its host. Any Pia session on it must already be over.
static void _leaveNetwork(struct GBASIORFUBroadcast* broadcast) {
	EnterCriticalSection(&broadcast->lock);
	struct LdndConnection* conn = broadcast->conn;
	uint32_t network = broadcast->network;
	uint16_t deviceId = broadcast->targetDeviceId;
	bool lost = broadcast->hostLost;
	broadcast->network = 0;
	broadcast->channel = 0;
	broadcast->targetDeviceId = 0;
	broadcast->hostLost = false;
	broadcast->leaveWanted = false;
	broadcast->queueCount = 0;
	LeaveCriticalSection(&broadcast->lock);
	if (!network) {
		return;
	}
	// Only this thread closes `conn`, so it is safe to use it outside the lock.
	struct LdndResult result;
	if (conn && LdndIsOpen(conn) && LdndCloseNetwork(conn, network, &result) != LDND_OK) {
		GBASIORFUTrace(broadcast->rfu, "LDN    could not leave %04X's network: %s%s%s", deviceId, LdndStatusName(result.code),
		               result.message[0] ? ": " : "", result.message);
	}
	GBASIORFUTrace(broadcast->rfu, "LDN    left %04X's network%s", deviceId, lost ? " (it was already gone)" : "");
}

static bool _joinAbandonedLocked(const struct GBASIORFUBroadcast* broadcast, uint16_t deviceId) {
	return broadcast->stopping || broadcast->hostLost || (broadcast->connectWanted && broadcast->connectDeviceId != deviceId);
}

// A join is only given up on for deinit, a lost host, or the game wanting a different host now. A reset, or the game
// giving up on waiting, does not stop it: see _piaHandshake.
static bool _joinAbandoned(struct GBASIORFUBroadcast* broadcast, uint16_t deviceId) {
	EnterCriticalSection(&broadcast->lock);
	bool abandoned = _joinAbandonedLocked(broadcast, deviceId);
	LeaveCriticalSection(&broadcast->lock);
	return abandoned;
}

// Answers the game's connect - unless it has moved on to another host since. The driver applies a result to whatever
// it is connecting to at the time, so a stale one must never reach it; hence the check and the answer under `lock`,
// which the emulation thread holds to ask for a connect.
static void _connectResult(struct GBASIORFUBroadcast* broadcast, uint16_t deviceId, bool ok) {
	EnterCriticalSection(&broadcast->lock);
	if (broadcast->connectWanted && broadcast->connectDeviceId == deviceId) {
		broadcast->connectWanted = false;
		GBASIORFUConnectResult(broadcast->rfu, ok, deviceId, 0);
	}
	LeaveCriticalSection(&broadcast->lock);
}

static void _reportRoom(struct GBASIORFUBroadcast* broadcast, const struct LdndNetworkInfo* network) {
	struct LdnRfuBeacon beacon;
	if (!LdnDecodeRfuBeacon(network->applicationData, network->applicationDataLength, &beacon)) {
		if (broadcast->nonRoomsHeard++ % 20 == 0) {
			GBASIORFUTrace(broadcast->rfu, "LDN    heard an LDN network (title %016llX, channel %u, app data %u bytes) that is not a Pokemon RFU room",
			               (unsigned long long) network->localCommunicationId, network->channel, network->applicationDataLength);
		}
		return;
	}
	uint32_t now = GetTickCount();
	EnterCriticalSection(&broadcast->lock);
	struct Room* room = NULL;
	for (size_t i = 0; i < kMaxRooms; ++i) {
		if (broadcast->rooms[i].valid && broadcast->rooms[i].trainerId == beacon.trainerId) {
			room = &broadcast->rooms[i];
			break;
		}
		if (!room || !broadcast->rooms[i].valid || (room->valid && now - broadcast->rooms[i].lastSeen > now - room->lastSeen)) {
			room = &broadcast->rooms[i];
		}
	}
	room->valid = true;
	room->trainerId = beacon.trainerId;
	room->lastSeen = now;
	room->network = *network;
	LeaveCriticalSection(&broadcast->lock);

	uint32_t words[RFU_BROADCAST_WORDS];
	LdnBeaconToBroadcastWords(&beacon, kAssumedCompat, kTradeActivity, words);
	GBASIORFUTrace(broadcast->rfu, "LDN    room dev=%04X name=\"%s\" channel %u -> BCAST %08X %08X %08X %08X %08X %08X", beacon.trainerId, beacon.name,
	               network->channel, words[0], words[1], words[2], words[3], words[4], words[5]);
	// The host's next free slot is unknown to us (the Switch is not running our RFU state machine); this project
	// only bridges a single joiner, so 0 (a slot is free) is always correct for now.
	GBASIORFUBroadcastReceived(broadcast->rfu, beacon.trainerId, 0, words);
}

static void _scanOnce(struct GBASIORFUBroadcast* broadcast, struct LdndConnection* conn) {
	uint8_t channel = kChannels[broadcast->scanIndex++ % kChannelCount];
	struct LdndScanRequest request = {&channel, 1, kScanDwellMs};
	struct LdndNetworkInfo networks[kMaxScanResults];
	size_t count;
	struct LdndResult result;
	int code = LdndScan(conn, &request, networks, kMaxScanResults, &count, &result);
	if (code != LDND_OK) {
		if (code != broadcast->lastScanError) {
			GBASIORFUTrace(broadcast->rfu, "LDN    ldnd could not scan: %s%s%s", LdndStatusName(code), result.message[0] ? ": " : "", result.message);
		}
		broadcast->lastScanError = code;
		if (code < 0) {
			_closeDaemon(broadcast);
		}
		// NO_RADIO while the adapter comes up, for one: its RADIO_STATE event wakes this early.
		_nap(broadcast, kRetryMs);
		return;
	}
	if (broadcast->lastScanError) {
		GBASIORFUTrace(broadcast->rfu, "LDN    searching (channels 1, 6, 11)");
		broadcast->lastScanError = LDND_OK;
	}
	for (size_t i = 0; i < count; ++i) {
		_reportRoom(broadcast, &networks[i]);
	}
}

// Brings the Pia connection layer (Net/Session/RTT - see ldn-pia-connect.c) up with the host ldnd just joined, then
// hands the session to the emulation thread. Answers the game if it is still waiting; if it is not (ldnd's join
// outlasted its patience, maybe with a reset since), the session is kept for the game's next attempt at this host.
static bool _piaHandshake(struct GBASIORFUBroadcast* broadcast, uint16_t deviceId, const struct LdndNetworkReply* reply) {
	const struct LdndNetworkInfo* network = &reply->network;
	// Addresses straight from ldnd: we are participant `participantIndex`, and the host is always participant 0.
	const struct LdndParticipant* us = &network->participants[reply->participantIndex];
	const uint8_t* hostIp = network->participants[0].ip;

	struct LdnPiaCrypto crypto;
	LdnPiaCryptoInit(&crypto, network->ssid);
	struct LdnPiaConnect piaConn;
	LdnPiaConnectInit(&piaConn, us->mac, network->address, us->ip, "mGBA");
	struct LdnPiaReliable* reliable = malloc(sizeof(*reliable)); // too large for the stack - see the project notes
	if (!reliable) {
		return false;
	}
	LdnPiaReliableInit(reliable, LDN_PIA_RELIABLE_RTO_BASE_MS, 200);

	// The same Net(1)/Session(13)/RTT(3) handshake loop that was live-proven against the real Switch - see the project
	// notes for the two real bugs (zstd decompression needing the frame's own exact length, and the outgoing header's
	// flags byte being dynamic, not a fixed constant) that had to be fixed before this ever reached ST_CONNECTED.
	uint16_t hostVar = 0x7620;
	uint64_t nonceCounter = 0;
	struct PiaPktids pktids;
	memset(&pktids, 0, sizeof(pktids));
	DWORD deadline = GetTickCount() + kPiaConnectTimeoutMs;
	unsigned tick = 0;
	unsigned rawReceived = 0;
	unsigned decryptFailed = 0;
	unsigned decompressFailed = 0;
	DWORD piaStart = GetTickCount();
	int lastState = piaConn.state;
	static const char* const kStateNames[] = {"NET", "FINALIZE", "CONNECTED"};
	while ((int32_t) (deadline - GetTickCount()) > 0 && !LdnPiaConnectIsConnected(&piaConn)) {
		if (_joinAbandoned(broadcast, deviceId)) {
			free(reliable);
			return false;
		}
		uint8_t datagram[LDN_PIA_MAX_DATAGRAM];
		uint8_t srcIp[4];
		size_t datagramLength = sizeof(datagram);
		while (_popDatagram(broadcast, srcIp, datagram, &datagramLength)) {
			++rawReceived;
			GBASIORFUTrace(broadcast->rfu, "LDN    pia rx #%u t+%lums from %u.%u.%u.%u len=%zu", rawReceived, GetTickCount() - piaStart, srcIp[0],
			               srcIp[1], srcIp[2], srcIp[3], datagramLength);
			uint8_t plain[LDN_PIA_MAX_DATAGRAM];
			size_t plainLength;
			if (LdnPiaDecrypt(&crypto, datagram, datagramLength, srcIp, plain, &plainLength)) {
				uint8_t decompressed[8192];
				size_t decompressedLength = sizeof(decompressed);
				if (LdnPiaDecompress(plain, plainLength, decompressed, &decompressedLength)) {
					struct LdnPiaMessage messages[8];
					size_t consumed;
					size_t n = LdnPiaParseMessages(decompressed, decompressedLength, messages, 8, &consumed);
					for (size_t i = 0; i < n; ++i) {
						if (messages[i].proto != LDN_PIA_PROTO_RELIABLE) {
							if (messages[i].proto == LDN_PIA_PROTO_SESSION || messages[i].proto == LDN_PIA_PROTO_NET) {
								char hex[600];
								size_t shown = messages[i].payloadLength < 250 ? messages[i].payloadLength : 250;
								for (size_t h = 0; h < shown; ++h) {
									snprintf(&hex[h * 2], 3, "%02X", messages[i].payload[h]);
								}
								hex[shown * 2] = 0;
								GBASIORFUTrace(broadcast->rfu, "LDN    pia msg proto=%u len=%zu: %s", messages[i].proto, messages[i].payloadLength, hex);
							}
							LdnPiaConnectOnMessage(&piaConn, messages[i].proto, messages[i].payload, messages[i].payloadLength);
						}
						// Reliable(10) frames during the handshake (before ST_CONNECTED) are not expected and are simply
						// ignored here - the real data stream is opened only once connected (see _frame).
					}
				} else {
					++decompressFailed;
				}
			} else {
				++decryptFailed;
			}
			datagramLength = sizeof(datagram);
		}
		if (piaConn.haveHostVar) {
			hostVar = piaConn.hostVar;
		}
		if (piaConn.state != lastState) {
			GBASIORFUTrace(broadcast->rfu, "LDN    Pia session with %04X: %s -> %s (raw=%u decryptFail=%u decompressFail=%u)", deviceId, kStateNames[lastState],
			               kStateNames[piaConn.state], rawReceived, decryptFailed, decompressFailed);
			lastState = piaConn.state;
		}
		LdnPiaConnectTick(&piaConn, tick++);
		struct LdnPiaOutMessage outMsgs[4];
		size_t nOut = LdnPiaConnectDrain(&piaConn, outMsgs, 4);
		for (size_t i = 0; i < nOut; ++i) {
			uint8_t tiled[kPiaMaxTiled];
			size_t tiledLength = LdnPiaBuildMessage(outMsgs[i].proto, outMsgs[i].payload, outMsgs[i].length, false, 0, tiled);
			bool compressed = false;
			if (outMsgs[i].compress) {
				uint8_t compbuf[sizeof(tiled)];
				size_t compLength = sizeof(compbuf);
				if (LdnPiaCompress(tiled, tiledLength, compbuf, &compLength)) {
					memcpy(tiled, compbuf, compLength);
					tiledLength = compLength;
					compressed = true;
				}
			}
			if (outMsgs[i].footer) {
				tiled[tiledLength++] = (uint8_t) (hostVar >> 8);
				tiled[tiledLength++] = (uint8_t) hostVar;
			}
			size_t beforePad = tiledLength;
			while (tiledLength % 16 != 0) {
				tiled[tiledLength++] = 0xFF;
			}
			uint8_t pad = (uint8_t) (tiledLength - beforePad);
			struct LdnPiaHeader header;
			header.dst = outMsgs[i].dst;
			header.src = outMsgs[i].src;
			header.pktid = outMsgs[i].establishing ? 0 : _nextPktid(&pktids, outMsgs[i].dst);
			header.enc = 0x90;
			header.flags = (uint8_t) ((pad << 4) | (compressed ? 1 : 0) | (outMsgs[i].establishing ? 2 : 0));
			header.footer = outMsgs[i].footer ? 2 : 0;
			++nonceCounter;
			for (int b = 0; b < 8; ++b) {
				header.nonce8[b] = (uint8_t) (nonceCounter >> (8 * (7 - b)));
			}
			uint8_t outDatagram[LDN_PIA_CIPHERTEXT_OFFSET + sizeof(tiled)];
			size_t outDatagramLength;
			if (LdnPiaEncrypt(&crypto, tiled, tiledLength, us->ip, &header, outDatagram, &outDatagramLength)) {
				int sendRc = _sendDatagram(broadcast, hostIp, outDatagram, outDatagramLength);
				GBASIORFUTrace(broadcast->rfu, "LDN    pia tx t+%lums proto=%u dst=%04X len=%zu rc=%d flags=%02X nonce=%llu", GetTickCount() - piaStart,
				               outMsgs[i].proto, outMsgs[i].dst, outDatagramLength, sendRc, header.flags, (unsigned long long) nonceCounter);
			}
		}
		Sleep(16);
	}

	if (!LdnPiaConnectIsConnected(&piaConn)) {
		GBASIORFUTrace(broadcast->rfu,
		               "LDN    Pia session with %04X did not reach CONNECTED within %ums (stuck at %s, raw=%u decryptFail=%u decompressFail=%u)", deviceId,
		               kPiaConnectTimeoutMs, kStateNames[piaConn.state], rawReceived, decryptFailed, decompressFailed);
		free(reliable);
		return false;
	}

	EnterCriticalSection(&broadcast->lock);
	bool abandoned = _joinAbandonedLocked(broadcast, deviceId) || broadcast->piaActive;
	bool answered = false;
	if (!abandoned) {
		broadcast->piaDeviceId = deviceId;
		broadcast->piaCrypto = crypto;
		broadcast->piaConn = piaConn;
		broadcast->piaReliable = reliable;
		broadcast->piaOpenedStream = false;
		broadcast->piaConnectId = (uint16_t) (0x1000 | (GetTickCount() & 0x0FFF)); // any nonzero value works
		broadcast->piaConnectQueued = false;
		broadcast->piaAccepted = false;
		broadcast->piaHostTSeen = false;
		broadcast->piaTs = 0x362D; // the first 'T' is 0x362E: the reference simulator's and the firmware's seed
		broadcast->piaKSeq = 0;
		memcpy(broadcast->piaOurIp, us->ip, 4);
		memcpy(broadcast->piaHostIp, hostIp, 4);
		broadcast->piaHostVar = hostVar;
		broadcast->piaNonceCounter = nonceCounter;
		broadcast->piaPktids = pktids;
		broadcast->piaTick = tick;
		broadcast->piaActive = true;
		if (broadcast->connectWanted && broadcast->connectDeviceId == deviceId) {
			broadcast->connectWanted = false;
			GBASIORFUConnectResult(broadcast->rfu, true, deviceId, 0);
			answered = true;
		}
	}
	LeaveCriticalSection(&broadcast->lock);
	if (abandoned) {
		free(reliable);
		return false;
	}
	GBASIORFUTrace(broadcast->rfu, "LDN    Pia session with %04X CONNECTED%s", deviceId,
	               answered ? "" : " (the game had stopped waiting for it; kept for its next attempt)");
	return true;
}

// Joins the room the game picked: ldnd's join, the Pia channel, then Pia's own handshake. Runs to the end even if the
// game gives up in the meantime (see _piaHandshake).
static void _join(struct GBASIORFUBroadcast* broadcast, uint16_t deviceId) {
	struct Room room;
	memset(&room, 0, sizeof(room));
	EnterCriticalSection(&broadcast->lock);
	for (size_t i = 0; i < kMaxRooms; ++i) {
		if (broadcast->rooms[i].valid && broadcast->rooms[i].trainerId == deviceId) {
			room = broadcast->rooms[i];
			break;
		}
	}
	LeaveCriticalSection(&broadcast->lock);
	if (!room.valid) {
		// connect() only asks for rooms it has seen, but a reset since then forgets them.
		GBASIORFUTrace(broadcast->rfu, "LDN    connect to %04X requested, but no room with that id is known any more", deviceId);
		_connectResult(broadcast, deviceId, false);
		return;
	}
	struct LdndConnection* conn = _daemon(broadcast);
	if (!conn) {
		GBASIORFUTrace(broadcast->rfu, "LDN    connect to %04X requested, but ldnd is not available", deviceId);
		_connectResult(broadcast, deviceId, false);
		return;
	}

	EnterCriticalSection(&broadcast->lock);
	broadcast->targetDeviceId = deviceId;
	broadcast->target = room.network;
	LeaveCriticalSection(&broadcast->lock);

	uint64_t ldnDeviceId = 0;
	LdnRandomBytes((uint8_t*) &ldnDeviceId, sizeof(ldnDeviceId));
	struct LdndConnectRequest request = {
		.network = &room.network,
		// The GBA emulator's own passphrase: it goes into the link key, so the join itself would work without it and
		// then every datagram would be dropped.
		.password = kLdnGbaPassphrase,
		.passwordLength = sizeof(kLdnGbaPassphrase),
		.name = "mGBA",
		.appVersion = room.network.appVersion,
		.platform = 0,
		.enableChallenge = true,
		.deviceId = ldnDeviceId,
		.timeoutMs = kJoinBudgetMs,
	};
	GBASIORFUTrace(broadcast->rfu, "LDN    joining %04X on channel %u (%u/%u participants)...", deviceId, room.network.channel, room.network.numParticipants,
	               room.network.maxParticipants);
	struct LdndNetworkReply reply;
	struct LdndResult result;
	DWORD start = GetTickCount();
	int code = LdndConnect(conn, &request, &reply, &result);
	DWORD elapsed = GetTickCount() - start;
	if (code != LDND_OK) {
		if (reply.haveAuthStatus) {
			GBASIORFUTrace(broadcast->rfu, "LDN    %04X refused the join after %lums (LDN auth status %u)%s%s", deviceId, elapsed, reply.authStatus,
			               result.message[0] ? ": " : "", result.message);
		} else {
			GBASIORFUTrace(broadcast->rfu, "LDN    joining %04X failed after %lums: %s%s%s", deviceId, elapsed, LdndStatusName(code),
			               result.message[0] ? ": " : "", result.message);
		}
		EnterCriticalSection(&broadcast->lock);
		broadcast->targetDeviceId = 0;
		LeaveCriticalSection(&broadcast->lock);
		if (code < 0) {
			// Given up on, or cut off: ldnd may still be in the middle of the join, and hanging up is the only way to
			// have it let go of whatever it ends up with.
			_closeDaemon(broadcast);
		}
		_connectResult(broadcast, deviceId, false);
		return;
	}

	const struct LdndParticipant* us = &reply.network.participants[reply.participantIndex];
	const struct LdndParticipant* host = &reply.network.participants[0];
	GBASIORFUTrace(broadcast->rfu, "LDN    joined %04X after %lums: participant %u at %u.%u.%u.%u, host at %u.%u.%u.%u", deviceId, elapsed,
	               reply.participantIndex, us->ip[0], us->ip[1], us->ip[2], us->ip[3], host->ip[0], host->ip[1], host->ip[2], host->ip[3]);
	EnterCriticalSection(&broadcast->lock);
	broadcast->network = reply.handle;
	broadcast->channel = 0;
	broadcast->target = reply.network;
	broadcast->hostLost = false;
	broadcast->leaveWanted = false;
	broadcast->queueCount = 0;
	LeaveCriticalSection(&broadcast->lock);

	uint32_t channel;
	uint16_t port;
	code = LdndOpenDatagram(conn, reply.handle, LDN_PIA_PORT, &channel, &port, &result);
	if (code != LDND_OK) {
		GBASIORFUTrace(broadcast->rfu, "LDN    could not open the Pia channel (UDP %u) on %04X's network: %s%s%s", LDN_PIA_PORT, deviceId, LdndStatusName(code),
		               result.message[0] ? ": " : "", result.message);
		_leaveNetwork(broadcast);
		_connectResult(broadcast, deviceId, false);
		return;
	}
	EnterCriticalSection(&broadcast->lock);
	broadcast->channel = channel;
	LeaveCriticalSection(&broadcast->lock);

	if (!_piaHandshake(broadcast, deviceId, &reply)) {
		_leaveNetwork(broadcast);
		_connectResult(broadcast, deviceId, false);
	}
}

static DWORD WINAPI _workerProc(LPVOID context) {
	struct GBASIORFUBroadcast* broadcast = context;
	while (true) {
		EnterCriticalSection(&broadcast->lock);
		bool stopping = broadcast->stopping;
		bool searching = broadcast->searching;
		bool connectWanted = broadcast->connectWanted;
		uint16_t deviceId = broadcast->connectDeviceId;
		uint32_t network = broadcast->network;
		bool piaActive = broadcast->piaActive;
		bool leave = network && !piaActive && (broadcast->leaveWanted || broadcast->hostLost);
		bool reuse = network && piaActive && !broadcast->hostLost && broadcast->targetDeviceId == deviceId;
		struct LdndConnection* conn = broadcast->conn;
		LeaveCriticalSection(&broadcast->lock);

		if (stopping) {
			break;
		}
		if (conn && !LdndIsOpen(conn)) {
			GBASIORFUTrace(broadcast->rfu, "LDN    lost the connection to ldnd");
			_closeDaemon(broadcast);
			continue;
		}
		if (leave) {
			_leaveNetwork(broadcast);
			continue;
		}
		if (connectWanted) {
			if (reuse) {
				GBASIORFUTrace(broadcast->rfu, "LDN    still joined to %04X from an earlier attempt; reusing that session", deviceId);
				_connectResult(broadcast, deviceId, true);
			} else if (network && piaActive) {
				// A session whose host just went away: the emulation thread is about to end it.
				_nap(broadcast, 100);
			} else {
				_leaveNetwork(broadcast);
				_join(broadcast, deviceId);
			}
			continue;
		}
		if (searching && !network) {
			if (!conn) {
				conn = _openDaemon(broadcast);
			}
			if (conn) {
				_scanOnce(broadcast, conn);
			} else {
				_nap(broadcast, kRetryMs);
			}
			continue;
		}
		if (network || !conn) {
			// Joined (the emulation thread drives it), or nothing going on at all; keep an eye on ldnd while it is open.
			_nap(broadcast, conn ? 1000 : INFINITE);
		} else if (WaitForSingleObject(broadcast->wake, kIdleReleaseMs) == WAIT_TIMEOUT) {
			GBASIORFUTrace(broadcast->rfu, "LDN    adapter idle; letting go of ldnd");
			_closeDaemon(broadcast);
		}
	}
	// deinit ended any Pia session before it stopped us.
	_leaveNetwork(broadcast);
	_closeDaemon(broadcast);
	return 0;
}

static void _ensureWorker(struct GBASIORFUBroadcast* broadcast) {
	if (!broadcast->worker) {
		broadcast->worker = CreateThread(NULL, 0, _workerProc, broadcast, 0, NULL);
		if (!broadcast->worker) {
			GBASIORFUTrace(broadcast->rfu, "LDN    could not start the ldnd worker thread");
		}
	}
}

// Emulation thread: stops driving the Pia session, if there is one; the worker then leaves its network.
static void _endPiaSession(struct GBASIORFUBroadcast* broadcast) {
	EnterCriticalSection(&broadcast->lock);
	bool active = broadcast->piaActive;
	struct LdnPiaReliable* reliable = NULL;
	if (active) {
		broadcast->piaActive = false;
		broadcast->leaveWanted = true;
		reliable = broadcast->piaReliable;
		broadcast->piaReliable = NULL;
	}
	LeaveCriticalSection(&broadcast->lock);
	if (!active) {
		return;
	}
	free(reliable);
	broadcast->lingerFrames = 0;
	GBASIORFUTrace(broadcast->rfu, "LDN    Pia session with %04X ended", broadcast->piaDeviceId);
	_wakeWorker(broadcast);
}

// Whether the game has connected over the Pia session (as opposed to one it gave up on before it was ready).
static bool _sessionUsed(struct GBASIORFUBroadcast* broadcast) {
	EnterCriticalSection(&broadcast->lock);
	bool active = broadcast->piaActive;
	LeaveCriticalSection(&broadcast->lock);
	return active && broadcast->piaOpenedStream;
}
#endif

static bool _init(struct GBASIORFUBackend* backend, struct GBASIORFU* rfu) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
	broadcast->rfu = rfu;
	// `lock` and `wake` are created in GBASIORFUBroadcastCreate(), not here: the SIO driver's own GBASIORFUInit() calls
	// backend->reset() - _reset() below, which takes `lock` - BEFORE it calls backend->init() at all.
	// EnterCriticalSection on a zeroed CRITICAL_SECTION is undefined behavior, and was crashing the process (ntdll.dll
	// access violation) the instant a ROM loaded and the broadcast backend auto-attached.
	GBASIORFUTrace(rfu, "LDN    Broadcast backend attached (ldnd is expected to already be running)");
	return true;
}

static void _deinit(struct GBASIORFUBackend* backend) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
#ifdef _WIN32
	_endPiaSession(broadcast);
	if (broadcast->worker) {
		EnterCriticalSection(&broadcast->lock);
		broadcast->stopping = true;
		if (broadcast->conn) {
			// A join can keep the worker inside ldnd for a minute; this cuts it short.
			LdndAbort(broadcast->conn);
		}
		LeaveCriticalSection(&broadcast->lock);
		_wakeWorker(broadcast);
		WaitForSingleObject(broadcast->worker, INFINITE);
		CloseHandle(broadcast->worker);
		broadcast->worker = NULL;
	}
	CloseHandle(broadcast->wake);
	broadcast->wake = NULL;
	DeleteCriticalSection(&broadcast->lock);
#else
	(void) broadcast;
#endif
}

static void _reset(struct GBASIORFUBackend* backend) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
#ifdef _WIN32
	// A session the game connected over is over. One it never got to use - ldnd's join finished after the game stopped
	// waiting - is kept for its next attempt, as is a join still in progress: the game resets the adapter before it
	// searches again, and starting over would only run into the same wait.
	if (_sessionUsed(broadcast)) {
		_endPiaSession(broadcast);
	}
	EnterCriticalSection(&broadcast->lock);
	broadcast->searching = false;
	broadcast->connectWanted = false;
	memset(broadcast->rooms, 0, sizeof(broadcast->rooms));
	if (broadcast->conn) {
		LdndScanCancel(broadcast->conn);
	}
	LeaveCriticalSection(&broadcast->lock);
	_wakeWorker(broadcast);
#else
	(void) broadcast;
#endif
}

static void _searchStart(struct GBASIORFUBackend* backend) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
#ifdef _WIN32
	EnterCriticalSection(&broadcast->lock);
	broadcast->searching = true;
	LeaveCriticalSection(&broadcast->lock);
	GBASIORFUTrace(broadcast->rfu, "LDN    searching (channels 1, 6, 11)");
	_ensureWorker(broadcast);
	_wakeWorker(broadcast);
#else
	(void) broadcast;
#endif
}

static void _searchStop(struct GBASIORFUBackend* backend) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
#ifdef _WIN32
	EnterCriticalSection(&broadcast->lock);
	broadcast->searching = false;
	// The game ends its search just before it connects; a scan still running would hold the join up.
	if (broadcast->conn) {
		LdndScanCancel(broadcast->conn);
	}
	LeaveCriticalSection(&broadcast->lock);
#else
	(void) broadcast;
#endif
}

// Hands the join to the worker (ldnd's join, then the Pia connection layer); the answer comes back through
// GBASIORFUConnectResult whenever the worker has one, as the driver requires this call to return immediately.
static void _connect(struct GBASIORFUBackend* backend, uint16_t deviceId) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
#ifdef _WIN32
	EnterCriticalSection(&broadcast->lock);
	bool known = broadcast->targetDeviceId == deviceId;
	for (size_t i = 0; i < kMaxRooms && !known; ++i) {
		known = broadcast->rooms[i].valid && broadcast->rooms[i].trainerId == deviceId;
	}
	bool otherSession = broadcast->piaActive && broadcast->piaDeviceId != deviceId;
	LeaveCriticalSection(&broadcast->lock);
	if (!known) {
		GBASIORFUTrace(broadcast->rfu, "LDN    connect to %04X requested, but no room with that id has been seen", deviceId);
		GBASIORFUConnectResult(broadcast->rfu, false, deviceId, 0);
		return;
	}
	if (otherSession) {
		// Still joined to a host the game gave up on earlier: ldnd holds one network at a time.
		_endPiaSession(broadcast);
	}
	_ensureWorker(broadcast);
	if (!broadcast->worker) {
		GBASIORFUConnectResult(broadcast->rfu, false, deviceId, 0);
		return;
	}
	EnterCriticalSection(&broadcast->lock);
	broadcast->connectWanted = true;
	broadcast->connectDeviceId = deviceId;
	if (broadcast->conn) {
		LdndScanCancel(broadcast->conn);
	}
	LeaveCriticalSection(&broadcast->lock);
	GBASIORFUTrace(broadcast->rfu, "LDN    connect to %04X", deviceId);
	_wakeWorker(broadcast);
#else
	GBASIORFUTrace(broadcast->rfu, "LDN    connect to %04X requested, but joining is not implemented on this platform", deviceId);
	GBASIORFUConnectResult(broadcast->rfu, false, deviceId, 0);
#endif
}

#ifdef _WIN32
// ---- Emulator ("gba") frames carried inside Reliable payloads - see pokeldn/frlgsim's gbaframe.py --------------

enum { kGbaMarker = 0x57, kGbaC = 0x43, kGbaA = 0x41, kGbaT = 0x54, kGbaK = 0x4B, kGbaD = 0x44 };

static bool _piaSendRaw(struct GBASIORFUBroadcast* broadcast, uint8_t proto, uint16_t dst, uint16_t src, bool establishing, bool footer,
                        bool compress, bool haveMsgFlags, uint8_t msgFlags, const uint8_t* payload, size_t length);

// A queued Reliable frame goes on the wire at once and is only retransmitted (LdnPiaReliablePoll) after the RTO. The
// window code only ever transmits from Poll, so without this every new frame - the stream-open metadata, the connect
// request, each K ack and each 'T' slot - waited a full RTO (200 ms bootstrap, and since a "retransmit" never yields an RTT
// sample it never shortened) before its first send. GB-Link's firmware (pia_link.c) and the reference simulator
// (_tx_reliable) both transmit immediately.
static void _reliableTransmit(struct GBASIORFUBroadcast* broadcast, uint16_t seq, uint8_t flagsA, const uint8_t* payload, size_t length) {
	uint8_t inner[8 + LDN_PIA_RELIABLE_MAX_PAYLOAD];
	size_t innerLength = LdnPiaBuildReliableFrame(seq, LdnPiaReliableSendLow(broadcast->piaReliable), flagsA, payload, length, inner);
	_piaSendRaw(broadcast, LDN_PIA_PROTO_RELIABLE, broadcast->piaConn.hostVar, broadcast->piaConn.ourVar, false, true, false, false, 0, inner,
	            innerLength);
}

static bool _reliableQueue(struct GBASIORFUBroadcast* broadcast, const uint8_t* payload, size_t length) {
	uint16_t seq = 0;
	bool queued = LdnPiaReliableSend(broadcast->piaReliable, payload, length, GetTickCount(), &seq);
	GBASIORFUTrace(broadcast->rfu, "PIA    queue reliable seq=%04X %zu bytes type=%c queued=%d", seq, length, length > 1 ? payload[1] : '?', queued);
	if (queued) {
		_reliableTransmit(broadcast, seq, LDN_PIA_FLAGSA_GBA, payload, length);
	}
	return queued;
}

static void _gbaSendConnect(struct GBASIORFUBroadcast* broadcast) {
	// 57 43 02 00 <connect_id:2> - the reference writes the id as the two bytes given (e.g. 67 79).
	uint8_t frame[6] = {kGbaMarker, kGbaC, 2, 0, (uint8_t) broadcast->piaConnectId, (uint8_t) (broadcast->piaConnectId >> 8)};
	_reliableQueue(broadcast, frame, sizeof(frame));
	broadcast->piaConnectQueued = true;
}

// The game's client slot (LLSF header + payload, exactly as it would have sent to a real adapter) -> a child 'T'
// frame: 57 54 <body_len:u16 LE> | <ts:u32 LE> 00 <slot_len:u8> 00 00 | <slot, zero-padded to a multiple of 4>.
static void _gbaSendSlot(struct GBASIORFUBroadcast* broadcast, const uint8_t* slot, size_t length) {
	uint8_t frame[4 + 8 + LDN_PIA_RELIABLE_MAX_PAYLOAD];
	size_t padded = (length + 3) & ~(size_t) 3;
	if (length > 255 || 8 + padded > LDN_PIA_RELIABLE_MAX_PAYLOAD) {
		return;
	}
	uint32_t ts = ++broadcast->piaTs;
	size_t bodyLength = 8 + padded;
	frame[0] = kGbaMarker;
	frame[1] = kGbaT;
	frame[2] = (uint8_t) bodyLength;
	frame[3] = (uint8_t) (bodyLength >> 8);
	frame[4] = (uint8_t) ts;
	frame[5] = (uint8_t) (ts >> 8);
	frame[6] = (uint8_t) (ts >> 16);
	frame[7] = (uint8_t) (ts >> 24);
	frame[8] = 0;
	frame[9] = (uint8_t) length;
	frame[10] = 0;
	frame[11] = 0;
	memset(&frame[12], 0, padded);
	memcpy(&frame[12], slot, length);
	_reliableQueue(broadcast, frame, 4 + bodyLength);
}

// 57 4b 0c 00 <k_seq:u32><mid:u32><acked_host_ts:u32>, all LE - one per unique host 'T'.
static void _gbaSendAck(struct GBASIORFUBroadcast* broadcast, uint32_t ackedTs) {
	uint8_t frame[16] = {kGbaMarker, kGbaK, 12, 0};
	uint32_t kSeq = ++broadcast->piaKSeq;
	uint32_t mid = 1;
	for (int i = 0; i < 4; ++i) {
		frame[4 + i] = (uint8_t) (kSeq >> (8 * i));
		frame[8 + i] = (uint8_t) (mid >> (8 * i));
		frame[12 + i] = (uint8_t) (ackedTs >> (8 * i));
	}
	_reliableQueue(broadcast, frame, sizeof(frame));
}

// One delivered (in-order, non-stream-open) Reliable payload: zero or more frames back to back.
static void _gbaReceive(struct GBASIORFUBroadcast* broadcast, const uint8_t* data, size_t length) {
	while (length >= 4 && data[0] == kGbaMarker) {
		uint8_t type = data[1];
		size_t bodyLength = data[2] | ((size_t) data[3] << 8);
		if (4 + bodyLength > length) {
			GBASIORFUTrace(broadcast->rfu, "PIA    gba frame '%c' truncated (%zu of %zu body bytes)", type, length - 4, bodyLength);
			return;
		}
		const uint8_t* body = &data[4];
		if (type == kGbaA) {
			broadcast->piaAccepted = true;
			GBASIORFUTrace(broadcast->rfu, "PIA    host ACCEPTED our connect ('A' body %zu bytes)", bodyLength);
		} else if (type == kGbaT && bodyLength >= 8) {
			uint32_t ts = body[0] | (body[1] << 8) | (body[2] << 16) | ((uint32_t) body[3] << 24);
			size_t slotLength = body[4];
			GBASIORFUTrace(broadcast->rfu, "PIA    host 'T' ts=%u slot_len=%zu", ts, slotLength);
			broadcast->piaHostTSeen = true;
			_gbaSendAck(broadcast, ts);
			if (slotLength > 1 && 8 + slotLength <= bodyLength) {
				GBASIORFUDataReceived(broadcast->rfu, 0, &body[8], slotLength);
			}
		} else if (type == kGbaD) {
			GBASIORFUTrace(broadcast->rfu, "PIA    host sent 'D' (disconnect)");
			GBASIORFUDisconnected(broadcast->rfu, 0);
		} else {
			GBASIORFUTrace(broadcast->rfu, "PIA    host gba frame '%c' (%zu body bytes) ignored", type, bodyLength);
		}
		data += 4 + bodyLength;
		length -= 4 + bodyLength;
	}
}

// Sends one Pia message as its own datagram (including the live-confirmed dynamic header flags byte), operating on
// the backend's persistent session fields.
static bool _piaSendRaw(struct GBASIORFUBroadcast* broadcast, uint8_t proto, uint16_t dst, uint16_t src, bool establishing, bool footer,
                        bool compress, bool haveMsgFlags, uint8_t msgFlags, const uint8_t* payload, size_t length) {
	uint8_t tiled[kPiaMaxTiled];
	size_t tiledLength = LdnPiaBuildMessage(proto, payload, length, haveMsgFlags, msgFlags, tiled);
	bool compressed = false;
	// The native client compresses any message body of 62 bytes or more (GB-Link's firmware does the same); the
	// Switch decompresses by the flag, so this is for fidelity rather than correctness.
	if (compress || tiledLength >= 62) {
		uint8_t compbuf[sizeof(tiled)];
		size_t compLength = sizeof(compbuf);
		if (LdnPiaCompress(tiled, tiledLength, compbuf, &compLength)) {
			memcpy(tiled, compbuf, compLength);
			tiledLength = compLength;
			compressed = true;
		}
	}
	if (footer) {
		tiled[tiledLength++] = (uint8_t) (broadcast->piaHostVar >> 8);
		tiled[tiledLength++] = (uint8_t) broadcast->piaHostVar;
	}
	size_t beforePad = tiledLength;
	while (tiledLength % 16 != 0) {
		tiled[tiledLength++] = 0xFF;
	}
	uint8_t pad = (uint8_t) (tiledLength - beforePad);

	struct LdnPiaHeader header;
	header.dst = dst;
	header.src = src;
	header.pktid = establishing ? 0 : _nextPktid(&broadcast->piaPktids, dst);
	header.enc = 0x90;
	header.flags = (uint8_t) ((pad << 4) | (compressed ? 1 : 0) | (establishing ? 2 : 0));
	header.footer = footer ? 2 : 0;
	++broadcast->piaNonceCounter;
	for (int i = 0; i < 8; ++i) {
		header.nonce8[i] = (uint8_t) (broadcast->piaNonceCounter >> (8 * (7 - i)));
	}

	uint8_t datagram[LDN_PIA_CIPHERTEXT_OFFSET + sizeof(tiled)];
	size_t datagramLength;
	if (!LdnPiaEncrypt(&broadcast->piaCrypto, tiled, tiledLength, broadcast->piaOurIp, &header, datagram, &datagramLength)) {
		return false;
	}
	int rc = _sendDatagram(broadcast, broadcast->piaHostIp, datagram, datagramLength);
	GBASIORFUTrace(broadcast->rfu, "PIA    tx hdr proto=%u dst=%04X src=%04X pktid=%04X flags=%02X footer=%u len=%zu rc=%d", proto, header.dst, header.src,
	               header.pktid, header.flags, header.footer, datagramLength, rc);
	return rc == LDND_OK;
}

static bool _piaSendMessage(struct GBASIORFUBroadcast* broadcast, const struct LdnPiaOutMessage* msg) {
	return _piaSendRaw(broadcast, msg->proto, msg->dst, msg->src, msg->establishing, msg->footer, msg->compress, false, 0, msg->payload, msg->length);
}

// While the game searches, the host being joined or already joined is shown from what is known of it: the worker
// cannot scan meanwhile, since that would take the radio from the joined network.
static void _reportTarget(struct GBASIORFUBroadcast* broadcast) {
	if (++broadcast->targetReportFrames < kTargetReportFrames) {
		return;
	}
	broadcast->targetReportFrames = 0;
	uint8_t appData[LDND_MAX_APPLICATION_DATA];
	size_t appDataLength = 0;
	EnterCriticalSection(&broadcast->lock);
	if (broadcast->searching && broadcast->targetDeviceId) {
		appDataLength = broadcast->target.applicationDataLength;
		memcpy(appData, broadcast->target.applicationData, appDataLength);
	}
	LeaveCriticalSection(&broadcast->lock);
	struct LdnRfuBeacon beacon;
	if (appDataLength && LdnDecodeRfuBeacon(appData, appDataLength, &beacon)) {
		uint32_t words[RFU_BROADCAST_WORDS];
		LdnBeaconToBroadcastWords(&beacon, kAssumedCompat, kTradeActivity, words);
		GBASIORFUBroadcastReceived(broadcast->rfu, beacon.trainerId, 0, words);
	}
}
#endif

// Drives the live Pia session once connected: takes each received datagram off the queue, decrypts/decompresses/tiles
// it, routes Net/Session/RTT messages to LdnPiaConnectOnMessage and Reliable(10) frames to LdnPiaReliableReceive
// (delivering each newly in-order payload to the driver via GBASIORFUDataReceived), then drains LdnPiaConnect's
// outbox and LdnPiaReliable's due retransmits/acks and sends them. Called once per emulated frame (~59.7 Hz, matching
// Pia's own real-hardware-measured cadence - see the project notes) on the emulation thread.
static void _frame(struct GBASIORFUBackend* backend) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
#ifdef _WIN32
	_reportTarget(broadcast);
	EnterCriticalSection(&broadcast->lock);
	bool active = broadcast->piaActive;
	bool lost = broadcast->hostLost;
	bool gameWaiting = broadcast->searching || broadcast->connectWanted;
	LeaveCriticalSection(&broadcast->lock);
	if (!active) {
		return;
	}
	if (lost) {
		_endPiaSession(broadcast);
		GBASIORFUDisconnected(broadcast->rfu, 0);
		return;
	}
	// The stream (and with it the emulator-level connect request) only opens once the game is connected: a session it
	// gave up on waiting for stays idle, and is only kept for a while unless the game comes back to it.
	bool gameConnected = broadcast->rfu->state == RFU_STATE_CLIENT;
	if (!broadcast->piaOpenedStream) {
		if (gameConnected || gameWaiting) {
			broadcast->lingerFrames = 0;
		} else if (++broadcast->lingerFrames > kLingerFrames) {
			GBASIORFUTrace(broadcast->rfu, "LDN    the game never came back for %04X", broadcast->piaDeviceId);
			_endPiaSession(broadcast);
			return;
		}
	}
	struct LdnPiaConnect* conn = &broadcast->piaConn;
	struct LdnPiaReliable* reliable = broadcast->piaReliable;

	uint8_t datagram[LDN_PIA_MAX_DATAGRAM];
	uint8_t srcIp[4];
	size_t datagramLength = sizeof(datagram);
	while (_popDatagram(broadcast, srcIp, datagram, &datagramLength)) {
		uint8_t plain[LDN_PIA_MAX_DATAGRAM];
		size_t plainLength;
		GBASIORFUTrace(broadcast->rfu, "PIA    raw datagram from %u.%u.%u.%u len=%zu", srcIp[0], srcIp[1], srcIp[2], srcIp[3], datagramLength);
		if (LdnPiaDecrypt(&broadcast->piaCrypto, datagram, datagramLength, srcIp, plain, &plainLength)) {
			struct LdnPiaHeader rxHeader;
			LdnPiaHeaderUnpack(datagram, &rxHeader);
			GBASIORFUTrace(broadcast->rfu, "PIA    rx hdr dst=%04X src=%04X pktid=%04X enc=%02X flags=%02X footer=%u len=%zu", rxHeader.dst, rxHeader.src,
			               rxHeader.pktid, rxHeader.enc, rxHeader.flags, rxHeader.footer, datagramLength);
			uint8_t decompressed[8192];
			size_t decompressedLength = sizeof(decompressed);
			if (!LdnPiaDecompress(plain, plainLength, decompressed, &decompressedLength)) {
				GBASIORFUTrace(broadcast->rfu, "PIA    DECOMPRESS FAILED (plain %zu bytes)", plainLength);
			} else {
				struct LdnPiaMessage messages[8];
				size_t consumed;
				size_t n = LdnPiaParseMessages(decompressed, decompressedLength, messages, 8, &consumed);
				if (!n) {
					GBASIORFUTrace(broadcast->rfu, "PIA    no tiled messages parsed (decompressed %zu bytes)", decompressedLength);
				}
				for (size_t i = 0; i < n; ++i) {
					GBASIORFUTrace(broadcast->rfu, "PIA    rx proto=%u len=%zu first=%02X", messages[i].proto, messages[i].payloadLength,
					               messages[i].payloadLength ? messages[i].payload[0] : 0);
					if (messages[i].proto == LDN_PIA_PROTO_RELIABLE) {
						struct LdnPiaReliableFrame frame;
						if (LdnPiaParseReliableFrame(messages[i].payload, messages[i].payloadLength, &frame)) {
							GBASIORFUTrace(broadcast->rfu, "PIA    rx reliable flagsA=%02X seq=%04X ack=%04X payload=%zu", frame.flagsA, frame.seq,
							               frame.ack, frame.payloadLength);
							if (!(frame.flagsA & LDN_PIA_FLAGSA_APP_DATA)) {
								uint16_t ackId;
								uint8_t mask[16];
								if (LdnPiaParseBulkAck(frame.payload, frame.payloadLength, &ackId, mask)) {
									GBASIORFUTrace(broadcast->rfu, "PIA    host bulk-ack: everything before %04X received; mask %02X%02X%02X%02X (our next seq %04X)", ackId,
									               mask[0], mask[1], mask[2], mask[3], reliable->outSeq);
								}
							}
							struct LdnPiaReliableEntry delivered[8];
							size_t nd = LdnPiaReliableReceive(reliable, &frame, GetTickCount(), delivered, 8);
							for (size_t d = 0; d < nd; ++d) {
								bool streamOpen = (delivered[d].flagsA & LDN_PIA_FLAGSA_INITIALIZED) != 0;
								GBASIORFUTrace(broadcast->rfu, "PIA    deliver seq=%04X flagsA=%02X len=%zu%s", delivered[d].seq, delivered[d].flagsA,
								               delivered[d].length, streamOpen ? " (stream-open)" : "");
								// The host's stream-open frame is NOT bare metadata: it carries the host's emulator connect accept
								// ('A', 57 41 06 00 <host session id:2> <our connect id:2> 00 00) as its payload. Dropping it as
								// "not real RFU data" threw away the very frame the whole join waits for. Anything that is a gba
								// frame (marker 0x57) is delivered; the metadata frames we send ourselves start with 0x4A.
								if (!streamOpen || (delivered[d].length && delivered[d].payload[0] == kGbaMarker)) {
									_gbaReceive(broadcast, delivered[d].payload, delivered[d].length);
								}
							}
						}
					} else {
						LdnPiaConnectOnMessage(conn, messages[i].proto, messages[i].payload, messages[i].payloadLength);
					}
				}
			}
		} else {
			GBASIORFUTrace(broadcast->rfu, "PIA    DECRYPT FAILED len=%zu", datagramLength);
		}
		datagramLength = sizeof(datagram);
	}

	if (conn->haveHostVar) {
		broadcast->piaHostVar = conn->hostVar;
	}
	LdnPiaConnectTick(conn, broadcast->piaTick++);

	if (LdnPiaConnectIsConnected(conn) && !broadcast->piaOpenedStream && gameConnected) {
		uint16_t seq;
		LdnPiaReliableOpen(reliable, kLdnPiaMetadataFrame, sizeof(kLdnPiaMetadataFrame), GetTickCount(), &seq);
		broadcast->piaOpenedStream = true;
		GBASIORFUTrace(broadcast->rfu, "PIA    opened reliable stream (metadata frame seq=%04X)", seq);
		_reliableTransmit(broadcast, seq, LDN_PIA_FLAGSA_INIT, kLdnPiaMetadataFrame, sizeof(kLdnPiaMetadataFrame));
	} else if (broadcast->piaOpenedStream && !broadcast->piaConnectQueued) {
		// The stream opens with the metadata frame alone; the emulator connect request follows on the next frame (as the
		// reference simulator's _drive_reliable does). The host starts ITS stream only after it sees the connect request
		// (waiting for its 'T' first, as an earlier experiment here did, deadlocks).
		_gbaSendConnect(broadcast);
	}

	struct LdnPiaOutMessage outMsgs[4];
	size_t nOut = LdnPiaConnectDrain(conn, outMsgs, 4);
	for (size_t i = 0; i < nOut; ++i) {
		_piaSendMessage(broadcast, &outMsgs[i]);
	}

	if (broadcast->piaOpenedStream) {
		struct LdnPiaReliableEntry due[8];
		size_t nDue = LdnPiaReliablePoll(reliable, GetTickCount(), due, 8);
		for (size_t i = 0; i < nDue; ++i) {
			uint8_t inner[8 + LDN_PIA_RELIABLE_MAX_PAYLOAD];
			size_t innerLength =
			    LdnPiaBuildReliableFrame(due[i].seq, LdnPiaReliableSendLow(reliable), due[i].flagsA, due[i].payload, due[i].length, inner);
			GBASIORFUTrace(broadcast->rfu, "PIA    tx reliable seq=%04X flagsA=%02X payload=%zu", due[i].seq, due[i].flagsA, due[i].length);
			// A pure ack frame (flagsA 0) carries the message-level flags byte 0x40, as the firmware's does; data frames
			// (flagsA 7/15) carry none.
			_piaSendRaw(broadcast, LDN_PIA_PROTO_RELIABLE, conn->hostVar, conn->ourVar, false, true, false, due[i].flagsA == LDN_PIA_FLAGSA_CTRL,
			            0x40, inner, innerLength);
		}
	}
#else
	(void) broadcast;
#endif
}

// Payload of a SendData command while connected to a host (client role - see rfu.h): queues it on the Reliable(10)
// stream. Held back until the host has accepted our emulator-level connect.
static void _sendData(struct GBASIORFUBackend* backend, const uint8_t* data, size_t length) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
#ifdef _WIN32
	EnterCriticalSection(&broadcast->lock);
	bool active = broadcast->piaActive;
	LeaveCriticalSection(&broadcast->lock);
	if (!active || !broadcast->piaOpenedStream) {
		GBASIORFUTrace(broadcast->rfu, "PIA    sendData %zu bytes DROPPED (active=%d opened=%d)", length, active, active && broadcast->piaOpenedStream);
		return;
	}
	if (!broadcast->piaAccepted) {
		GBASIORFUTrace(broadcast->rfu, "PIA    sendData %zu bytes held back (host has not accepted our connect yet)", length);
		return;
	}
	GBASIORFUTrace(broadcast->rfu, "PIA    sendData %zu bytes -> 'T' frame", length);
	_gbaSendSlot(broadcast, data, length);
#else
	(void) broadcast;
	(void) data;
	(void) length;
#endif
}

// Client role: leave the host (slotMask is ignored - see rfu.h). A session the game has connected over ends here; a
// join it is walking away from carries on for its next attempt (see _reset).
static void _disconnect(struct GBASIORFUBackend* backend, unsigned slotMask) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
	(void) slotMask;
#ifdef _WIN32
	if (_sessionUsed(broadcast)) {
		_endPiaSession(broadcast);
	}
	EnterCriticalSection(&broadcast->lock);
	broadcast->connectWanted = false;
	LeaveCriticalSection(&broadcast->lock);
#else
	(void) broadcast;
#endif
}

// Hosting is not implemented yet.
static void _noop(struct GBASIORFUBackend* backend) {
	(void) backend;
}
static void _noopBroadcast(struct GBASIORFUBackend* backend, const uint32_t data[RFU_BROADCAST_WORDS]) {
	(void) backend;
	(void) data;
}
static void _noopDeviceId(struct GBASIORFUBackend* backend, uint16_t deviceId) {
	(void) backend;
	(void) deviceId;
}
static void _noopReply(struct GBASIORFUBackend* backend, uint16_t clientId, bool accepted, unsigned slot) {
	(void) backend;
	(void) clientId;
	(void) accepted;
	(void) slot;
}

struct GBASIORFUBackend* GBASIORFUBroadcastCreate(void) {
	struct GBASIORFUBroadcast* broadcast = calloc(1, sizeof(*broadcast));
	if (!broadcast) {
		return NULL;
	}
	// Created here, not in _init(): the SIO driver's GBASIORFUInit() calls backend->reset() (_reset(), which takes
	// `lock`) BEFORE it ever calls backend->init() - so these must be valid from the moment the backend exists.
#ifdef _WIN32
	broadcast->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
	InitializeCriticalSection(&broadcast->lock);
#endif
	broadcast->d.init = _init;
	broadcast->d.deinit = _deinit;
	broadcast->d.reset = _reset;
	broadcast->d.frame = _frame;
	broadcast->d.setBroadcast = _noopBroadcast;
	broadcast->d.hostStart = _noopDeviceId;
	broadcast->d.hostStop = _noop;
	broadcast->d.connectReply = _noopReply;
	broadcast->d.searchStart = _searchStart;
	broadcast->d.searchStop = _searchStop;
	broadcast->d.connect = _connect;
	broadcast->d.disconnect = _disconnect;
	broadcast->d.sendData = _sendData;
	return &broadcast->d;
}
