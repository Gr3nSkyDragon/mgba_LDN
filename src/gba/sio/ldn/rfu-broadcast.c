/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/rfu-broadcast.h>

#include "ldn-auth.h"
#include "ldn-monitor.h"
#include "ldn-pia-connect.h"
#include "ldn-pia-reliable.h"
#include "ldn-pia.h"
#include "ldn-station.h"
#include "ldn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

enum {
	// The channels a Switch running FRLG's Direct Corner has been seen to use; hop between all three while
	// searching (see the project notes: the Switch itself moves between them, so scanning only one is not enough).
	kChannelCount = 3,
	kHopMs = 400,

	// English FireRed's compat+serial word (see rfu.c's BCAST trace of a real cartridge). The Switch's own beacon
	// does not carry this, so it is filled in here. TODO: unconfirmed for LeafGreen or other languages/versions -
	// revisit once a LeafGreen Switch capture is available.
	kAssumedCompat = 0x13820002,
	// The only adapter "activity" this project cares about (see rfu.c's command trace of a real cartridge).
	kTradeActivity = 0x04,

	// How often the same trainer id may be re-reported to the driver (it debounces anyway via its peer TTL, but
	// there is no reason to decode+re-encode a fresh copy of the same beacon on every single radio frame).
	kReannounceMs = 1000,

	// How long the connect thread will wait for the Pia CONNECTION layer (Net/Session/RTT - see ldn-pia-connect.c)
	// to reach ST_CONNECTED after a successful WPA2 association + LDN authentication, before giving up on the
	// whole connect attempt. Generous vs. the live-observed real timing (well under a second in every live test),
	// since this runs on its own thread and does not block the emulation thread either way.
	kPiaConnectTimeoutMs = 8000,

	// The largest single tiled message this backend ever sends: a Reliable(10) frame wrapping up to
	// LDN_PIA_RELIABLE_MAX_PAYLOAD bytes of inner payload (8-byte sub-header + payload), plus the message-tiling
	// layer's own 5-byte header, a 2-byte footer and up to 15 bytes of 0xFF padding. Mirrors ldn-pia-join.c's own
	// PIA_JOIN_MAX_TILED sizing exactly.
	kPiaMaxTiled = 5 + 8 + LDN_PIA_RELIABLE_MAX_PAYLOAD + 2 + 16,
};

static const unsigned kChannels[kChannelCount] = {1, 6, 11};

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

struct GBASIORFUBroadcast {
	struct GBASIORFUBackend d;
	struct GBASIORFU* rfu;

	char keysPath[512];
	struct LdnKeys keys;
	bool haveKeys;

	struct LdnMonitor* monitor;
#ifdef _WIN32
	HANDLE hopThread;
	HANDLE stopEvent;
	CRITICAL_SECTION lastAdLock;
	HANDLE connectThread;

	// Opening the monitor is several round trips to ldnd, each allowed 5s; a radio that stops answering used to freeze
	// the emulation thread for 10s inside the game's search-start command. It is opened on openThread instead (see
	// _openThreadProc). monitorLock guards `monitor`, `hopThread` and the three flags below:
	// - monitorWanted: a search asked for the monitor and no reset/deinit has dropped it since,
	// - hopWanted: channel hopping should run once the monitor is up (cleared when the game stops searching),
	// - monitorOpening: openThread is running.
	CRITICAL_SECTION monitorLock;
	HANDLE openThread;
	bool monitorWanted;
	bool hopWanted;
	bool monitorOpening;
#endif

	struct {
		uint16_t trainerId;
		uint32_t lastMs;
	} lastSeen[8];

	// The most recently decoded advertisement (this project bridges one joiner to one host, so remembering only
	// the last one seen is enough): what connect(deviceId) needs to actually associate with it. Guarded by
	// lastAdLock (written from ldnd's reader thread in _onAdvertisement, read from the connect thread).
	bool haveLastAd;
	uint16_t lastTrainerId;
	unsigned lastChannel;
	uint8_t lastAdMac[6]; // the advertiser's own MAC (BSSID) - see LdnStationConnect's targetBssid parameter
	struct LdnAdvertisement lastAd;

	// The live Pia session, once a connect attempt has taken it all the way to ST_CONNECTED. Set up entirely on
	// the connect thread (see _connectThreadProc), then handed off: `piaActive` is the one field written under
	// `piaLock` (a single true/false handoff), guaranteeing the emulation thread (frame()/sendData()/disconnect())
	// sees a fully-initialized session before it ever touches the rest of these fields. After the handoff, only
	// the emulation thread ever touches them again (the connect thread that set them up is finished and about to
	// exit) - so nothing past `piaActive` itself needs its own lock.
#ifdef _WIN32
	CRITICAL_SECTION piaLock;
#endif
	bool piaActive;
	uint16_t piaDeviceId;
	uint32_t piaIfIndex;
	struct LdndConnection* piaConnHandle; // kept open for the whole session (LdnPiaSocket + LdnStation share it)
	struct LdnStation* piaStation; // kept open only so the final disconnect() can deauth cleanly
	struct LdnPiaSocket* piaSocket;
	struct LdnPiaCrypto piaCrypto;
	struct LdnPiaConnect piaConn;
	struct LdnPiaReliable* piaReliable; // heap-allocated - too large for an inline struct member (see the project notes)
	bool piaOpenedStream;
	uint8_t piaOurMac[6];
	uint8_t piaHostMac[6];
	uint8_t piaOurIp[4];
	uint8_t piaHostIp[4];
	uint16_t piaHostVar; // mirrors ldn-pia-join.c's PiaSender.hostVar - resynced from piaConn.hostVar once known
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

void GBASIORFUBroadcastSetKeysPath(struct GBASIORFUBackend* backend, const char* prodKeysPath) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
	if (!prodKeysPath) {
		broadcast->keysPath[0] = 0;
		return;
	}
	size_t length = strlen(prodKeysPath);
	if (length >= sizeof(broadcast->keysPath)) {
		length = sizeof(broadcast->keysPath) - 1;
	}
	memcpy(broadcast->keysPath, prodKeysPath, length);
	broadcast->keysPath[length] = 0;
}

static void _onAdvertisement(void* context, const uint8_t* mac, const uint8_t* body, size_t bodyLength, unsigned channel) {
	struct GBASIORFUBroadcast* broadcast = context;
	(void) channel;
	struct LdnAdvertisement ad;
	static unsigned heard;
	bool traceHeard = (heard++ % 20) == 0; // an LDN advertisement of any kind reached us: proves the radio is receiving
	if (!LdnDecodeAdvertisement(body, bodyLength, broadcast->haveKeys ? &broadcast->keys : NULL, &ad) || !ad.infoDecoded) {
		if (traceHeard) {
			GBASIORFUTrace(broadcast->rfu, "LDN    heard an LDN advertisement (%zu bytes, channel %u) that could not be decoded (keys loaded: %d)", bodyLength, channel,
			               broadcast->haveKeys);
		}
		return;
	}
	struct LdnRfuBeacon beacon;
	if (!LdnDecodeRfuBeacon(ad.appData, ad.appDataSize, &beacon)) {
		if (traceHeard) {
			GBASIORFUTrace(broadcast->rfu, "LDN    heard a decoded advertisement (app data %zu bytes) that is not a Pokemon RFU beacon", (size_t) ad.appDataSize);
		}
		return;
	}

#ifdef _WIN32
	EnterCriticalSection(&broadcast->lastAdLock);
	broadcast->haveLastAd = true;
	broadcast->lastTrainerId = beacon.trainerId;
	broadcast->lastChannel = channel;
	memcpy(broadcast->lastAdMac, mac, 6);
	broadcast->lastAd = ad;
	LeaveCriticalSection(&broadcast->lastAdLock);
#endif

#ifdef _WIN32
	uint32_t now = GetTickCount();
	size_t oldest = 0;
	for (size_t i = 0; i < sizeof(broadcast->lastSeen) / sizeof(broadcast->lastSeen[0]); ++i) {
		if (broadcast->lastSeen[i].trainerId == beacon.trainerId) {
			if (now - broadcast->lastSeen[i].lastMs < kReannounceMs) {
				return;
			}
			oldest = i;
			break;
		}
		if (now - broadcast->lastSeen[i].lastMs > now - broadcast->lastSeen[oldest].lastMs) {
			oldest = i;
		}
	}
	broadcast->lastSeen[oldest].trainerId = beacon.trainerId;
	broadcast->lastSeen[oldest].lastMs = now;
#endif

	uint32_t words[RFU_BROADCAST_WORDS];
	LdnBeaconToBroadcastWords(&beacon, kAssumedCompat, kTradeActivity, words);
	GBASIORFUTrace(broadcast->rfu, "LDN    beacon dev=%04X name=\"%s\" -> BCAST %08X %08X %08X %08X %08X %08X", beacon.trainerId, beacon.name,
	               words[0], words[1], words[2], words[3], words[4], words[5]);
	// The host's next free slot is unknown to us (the Switch is not running our RFU state machine); this project
	// only bridges a single joiner, so 0 (a slot is free) is always correct for now.
	GBASIORFUBroadcastReceived(broadcast->rfu, beacon.trainerId, 0, words);
}

#ifdef _WIN32
static DWORD WINAPI _hopThread(LPVOID context) {
	struct GBASIORFUBroadcast* broadcast = context;
	unsigned index = 1; // LdnMonitorOpen already set kChannels[0]
	while (WaitForSingleObject(broadcast->stopEvent, kHopMs) == WAIT_TIMEOUT) {
		unsigned channel = kChannels[index++ % kChannelCount];
		if (LdnMonitorSetChannel(broadcast->monitor, channel)) {
			GBASIORFUTrace(broadcast->rfu, "LDN    %s", LdnMonitorLastError());
		}
	}
	return 0;
}
#endif

static void _joinConnectThread(struct GBASIORFUBroadcast* broadcast);

#ifdef _WIN32
// Caller holds monitorLock, and broadcast->monitor is set.
static void _startHopLocked(struct GBASIORFUBroadcast* broadcast) {
	if (!broadcast->hopThread) {
		ResetEvent(broadcast->stopEvent);
		broadcast->hopThread = CreateThread(NULL, 0, _hopThread, broadcast, 0, NULL);
	}
}

// Opens the monitor off the emulation thread. The game keeps polling its search (it just sees no rooms yet) while
// this waits on ldnd. When it finishes, the result is installed only if a search still wants it; a reset that came
// in meanwhile gets it closed again right here. If a new search starts while that close is running, it opens again.
static DWORD WINAPI _openThreadProc(LPVOID context) {
	struct GBASIORFUBroadcast* broadcast = context;
	while (true) {
		DWORD start = GetTickCount();
		struct LdnMonitor* monitor = LdnMonitorOpen(NULL, kChannels[0], NULL, _onAdvertisement, broadcast);
		if (!monitor) {
			GBASIORFUTrace(broadcast->rfu, "LDN    %s (after %lums)", LdnMonitorLastError(), GetTickCount() - start);
		}
		EnterCriticalSection(&broadcast->monitorLock);
		if (broadcast->monitorWanted) {
			broadcast->monitor = monitor;
			if (monitor) {
				GBASIORFUTrace(broadcast->rfu, "LDN    monitor open after %lums", GetTickCount() - start);
				if (broadcast->hopWanted) {
					_startHopLocked(broadcast);
					GBASIORFUTrace(broadcast->rfu, "LDN    searching (channels 1, 6, 11)");
				}
			}
			broadcast->monitorOpening = false;
			LeaveCriticalSection(&broadcast->monitorLock);
			return 0;
		}
		LeaveCriticalSection(&broadcast->monitorLock);

		if (monitor) {
			LdnMonitorClose(monitor);
			GBASIORFUTrace(broadcast->rfu, "LDN    monitor closed (reset while it was opening)");
		}
		EnterCriticalSection(&broadcast->monitorLock);
		if (!broadcast->monitorWanted) {
			broadcast->monitorOpening = false;
			LeaveCriticalSection(&broadcast->monitorLock);
			return 0;
		}
		LeaveCriticalSection(&broadcast->monitorLock);
	}
}
#endif

static void _startSearching(struct GBASIORFUBroadcast* broadcast) {
#ifdef _WIN32
	// A monitor left parked by _stopSearching (see there) is shared with any earlier connect attempt, which must be
	// finished first - it would otherwise be associating while the hop thread yanks the radio between channels.
	_joinConnectThread(broadcast);
	if (broadcast->keysPath[0] && !broadcast->haveKeys) {
		broadcast->haveKeys = LdnKeysLoad(broadcast->keysPath, &broadcast->keys);
		if (!broadcast->haveKeys) {
			GBASIORFUTrace(broadcast->rfu, "LDN    could not read the four LDN keys from \"%s\"", broadcast->keysPath);
		}
	}
	if (!broadcast->haveKeys) {
		GBASIORFUTrace(broadcast->rfu, "LDN    no prod.keys configured (Tools > Settings > BIOS); advertisements cannot be decoded");
	}

	EnterCriticalSection(&broadcast->monitorLock);
	broadcast->monitorWanted = true;
	broadcast->hopWanted = true;
	if (broadcast->monitor) {
		if (!broadcast->hopThread) {
			_startHopLocked(broadcast);
			GBASIORFUTrace(broadcast->rfu, "LDN    searching again (monitor was parked)");
		}
	} else if (!broadcast->monitorOpening) {
		if (broadcast->openThread) {
			// The previous opener already cleared monitorOpening, so it is exiting (or has exited).
			WaitForSingleObject(broadcast->openThread, INFINITE);
			CloseHandle(broadcast->openThread);
		}
		broadcast->monitorOpening = true;
		broadcast->openThread = CreateThread(NULL, 0, _openThreadProc, broadcast, 0, NULL);
		if (broadcast->openThread) {
			GBASIORFUTrace(broadcast->rfu, "LDN    opening the monitor in the background...");
		} else {
			broadcast->monitorOpening = false;
			GBASIORFUTrace(broadcast->rfu, "LDN    could not start the monitor thread");
		}
	}
	LeaveCriticalSection(&broadcast->monitorLock);
#else
	(void) broadcast;
#endif
}

// Stops channel hopping but deliberately leaves the monitor vif and its ldnd connection open ("parked"). The game
// issues CONNECT within ~0.03s of the READ_END that calls this; tearing the monitor vif down right there left the
// radio mid-reconfiguration, and the connect thread's very next RTM_NEWLINK (interface up) and CMD_CONNECT each
// stalled ~2.3s waiting on it (measured: 2313ms + 2312ms + 2485ms) - long enough to blow through FRLG's ~4s
// IsConnectionComplete patience. The standalone ldn-pia-join tool never tears the monitor down before associating.
// Also keeps a monitor that is still opening from starting to hop once it is up.
static void _stopHop(struct GBASIORFUBroadcast* broadcast) {
#ifdef _WIN32
	EnterCriticalSection(&broadcast->monitorLock);
	broadcast->hopWanted = false;
	if (broadcast->hopThread) {
		SetEvent(broadcast->stopEvent);
		WaitForSingleObject(broadcast->hopThread, 5000);
		CloseHandle(broadcast->hopThread);
		broadcast->hopThread = NULL;
	}
	LeaveCriticalSection(&broadcast->monitorLock);
#else
	(void) broadcast;
#endif
}

static void _stopSearching(struct GBASIORFUBroadcast* broadcast) {
	_stopHop(broadcast);
#ifdef _WIN32
	EnterCriticalSection(&broadcast->monitorLock);
	bool open = broadcast->monitor != NULL;
	LeaveCriticalSection(&broadcast->monitorLock);
	if (open) {
		GBASIORFUTrace(broadcast->rfu, "LDN    stopped searching (monitor kept open)");
	}
#endif
}

// Full teardown of the monitor (vif + connection). Only for reset/deinit, after any connect thread has been joined.
// A monitor still being opened is not waited for: its opener sees monitorWanted cleared and closes it itself.
static void _closeMonitor(struct GBASIORFUBroadcast* broadcast) {
	_stopHop(broadcast);
#ifdef _WIN32
	EnterCriticalSection(&broadcast->monitorLock);
	broadcast->monitorWanted = false;
	struct LdnMonitor* monitor = broadcast->monitor;
	broadcast->monitor = NULL;
	LeaveCriticalSection(&broadcast->monitorLock);
	if (monitor) {
		LdnMonitorClose(monitor);
		GBASIORFUTrace(broadcast->rfu, "LDN    monitor closed");
	}
#endif
}

// Waits for any in-flight connect attempt to finish (it always reports a result and exits promptly - either
// association fails within LdnStationConnect's own timeouts, or it succeeds - so this is not an unbounded wait in
// practice) and releases its thread handle. Safe to call whether or not one is running.
static void _joinConnectThread(struct GBASIORFUBroadcast* broadcast) {
#ifdef _WIN32
	if (broadcast->connectThread) {
		WaitForSingleObject(broadcast->connectThread, INFINITE);
		CloseHandle(broadcast->connectThread);
		broadcast->connectThread = NULL;
	}
#else
	(void) broadcast;
#endif
}

// Tears down a live Pia session (socket, the kept-open station + its WPA2 association, the shared ldnd
// connection, the heap-allocated reliable window) if one is active. Safe to call whether or not one is running.
// Always runs on the emulation thread (disconnect()/deinit()/reset() are all driver->backend calls), so the
// `piaLock`-guarded flag write here is the same one-time handoff used to activate a session, just in reverse.
static void _piaTeardown(struct GBASIORFUBroadcast* broadcast) {
#ifdef _WIN32
	EnterCriticalSection(&broadcast->piaLock);
	bool active = broadcast->piaActive;
	broadcast->piaActive = false;
	LeaveCriticalSection(&broadcast->piaLock);
	if (!active) {
		return;
	}
	if (broadcast->piaReliable) {
		free(broadcast->piaReliable);
		broadcast->piaReliable = NULL;
	}
	if (broadcast->piaSocket) {
		LdnPiaSocketClose(broadcast->piaSocket);
		broadcast->piaSocket = NULL;
	}
	if (broadcast->piaStation) {
		// A courtesy WPA2 deauth - IMPORTANT: an association left up wedges the radio for every later attempt
		// until ldnd itself restarts (measured live - see the project notes). LdnStationDisconnect also brings
		// the interface back down, which a bare CMD_DISCONNECT was confirmed live to not be sufficient for.
		LdnStationDisconnect(broadcast->piaStation, broadcast->piaIfIndex);
		LdnStationClose(broadcast->piaStation);
		broadcast->piaStation = NULL;
	}
	// piaConnHandle is the parked monitor's connection (see _stopHop) - owned by the monitor, closed with it.
	broadcast->piaConnHandle = NULL;
	GBASIORFUTrace(broadcast->rfu, "LDN    Pia session with %04X ended", broadcast->piaDeviceId);
#else
	(void) broadcast;
#endif
}

static bool _init(struct GBASIORFUBackend* backend, struct GBASIORFU* rfu) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
	broadcast->rfu = rfu;
	// stopEvent/lastAdLock/piaLock are created in GBASIORFUBroadcastCreate(), not here: the SIO driver's own
	// GBASIORFUInit() calls _resetAdapter() - which calls backend->reset(), i.e. _reset() below - BEFORE it calls
	// backend->init() at all. _reset() -> _piaTeardown() takes piaLock, so on the very first reset (before this
	// function has ever run) that lock must already be a validly initialized CRITICAL_SECTION, not calloc's
	// zeroed bytes - EnterCriticalSection on a zeroed CRITICAL_SECTION is undefined behavior and was crashing the
	// process (ntdll.dll access violation) the instant a ROM loaded and the broadcast backend auto-attached.
	GBASIORFUTrace(rfu, "LDN    Broadcast backend attached (ldnd is expected to already be running)");
	return true;
}

static void _deinit(struct GBASIORFUBackend* backend) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
	_stopHop(broadcast);
	_joinConnectThread(broadcast);
	_piaTeardown(broadcast); // uses the monitor's connection for the deauth - close the monitor only afterward
	_closeMonitor(broadcast);
#ifdef _WIN32
	if (broadcast->openThread) {
		// It references this backend, so it must be gone first; monitorWanted is already cleared, so it closes
		// whatever it opened and exits.
		WaitForSingleObject(broadcast->openThread, INFINITE);
		CloseHandle(broadcast->openThread);
		broadcast->openThread = NULL;
	}
	if (broadcast->stopEvent) {
		CloseHandle(broadcast->stopEvent);
		broadcast->stopEvent = NULL;
	}
	DeleteCriticalSection(&broadcast->lastAdLock);
	DeleteCriticalSection(&broadcast->piaLock);
	DeleteCriticalSection(&broadcast->monitorLock);
#endif
}

static void _reset(struct GBASIORFUBackend* backend) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
	_stopHop(broadcast);
	_joinConnectThread(broadcast);
	_piaTeardown(broadcast);
	_closeMonitor(broadcast);
}

static void _searchStart(struct GBASIORFUBackend* backend) {
	_startSearching((struct GBASIORFUBroadcast*) backend);
}

static void _searchStop(struct GBASIORFUBackend* backend) {
	_stopSearching((struct GBASIORFUBroadcast*) backend);
}

#ifdef _WIN32
struct ConnectAttempt {
	struct GBASIORFUBroadcast* broadcast;
	uint16_t deviceId;
	unsigned channel;
	uint8_t adMac[6];
	struct LdnAdvertisement ad;
};

static DWORD WINAPI _connectThreadProc(LPVOID arg) {
	struct ConnectAttempt* attempt = arg;
	struct GBASIORFUBroadcast* broadcast = attempt->broadcast;
	bool ok = false;
	struct LdndConnection* conn = NULL;
	struct LdnStation* station = NULL;

	uint8_t wlanKey[16];
	if (!LdnDeriveWlanKey(&broadcast->keys, attempt->ad.protocol, attempt->ad.serverRandom, wlanKey)) {
		GBASIORFUTrace(broadcast->rfu, "LDN    could not derive the WLAN key for %04X", attempt->deviceId);
		goto done;
	}
	char ssid[33];
	LdnAdvertisementWlanSsid(&attempt->ad, ssid);

	// RFU_CMD_BROADCAST_READ_END (see rfu.c) always calls searchStop() before the game issues CONNECT; that now only
	// stops hopping (see _stopHop) and leaves the monitor's ldnd connection open, which is shared here exactly as
	// ldn-pia-join.c does - no teardown/reopen churn on the radio between the scan and the join. The connection is
	// owned by the monitor and is never closed from this thread.
	// reset()/deinit() join this thread before they close the monitor, so it stays valid for the whole attempt.
	EnterCriticalSection(&broadcast->monitorLock);
	struct LdnMonitor* monitor = broadcast->monitor;
	LeaveCriticalSection(&broadcast->monitorLock);
	if (!monitor) {
		GBASIORFUTrace(broadcast->rfu, "LDN    connect to %04X requested, but the search monitor is gone", attempt->deviceId);
		goto done;
	}
	conn = LdnMonitorConnection(monitor);
	// Pin the (no longer hopping) monitor to the host's channel first, so the radio is not left with a monitor vif on
	// one channel while the station associates on another.
	if (LdnMonitorSetChannel(monitor, attempt->channel)) {
		GBASIORFUTrace(broadcast->rfu, "LDN    %s", LdnMonitorLastError());
	}
	station = LdnStationOpen(conn);
	if (!station) {
		GBASIORFUTrace(broadcast->rfu, "LDN    %s", LdnStationLastError());
		goto done;
	}
	uint32_t ifIndex;
	uint8_t ourMac[6];
	if (LdnStationFindInterface(station, &ifIndex, ourMac)) {
		GBASIORFUTrace(broadcast->rfu, "LDN    %s", LdnStationLastError());
		goto done;
	}
	GBASIORFUTrace(broadcast->rfu, "LDN    associating with %04X (ssid %s, channel %u)...", attempt->deviceId, ssid, attempt->channel);
	uint8_t hostMac[6];
	int error = LdnStationConnect(station, ifIndex, ssid, attempt->channel, wlanKey, attempt->adMac, hostMac);
	if (error) {
		GBASIORFUTrace(broadcast->rfu, "LDN    association with %04X failed: %s", attempt->deviceId, LdnStationLastError());
		goto done;
	}
	GBASIORFUTrace(broadcast->rfu, "LDN    associated with %04X (BSSID %02X:%02X:%02X:%02X:%02X:%02X); authenticating...", attempt->deviceId,
	               hostMac[0], hostMac[1], hostMac[2], hostMac[3], hostMac[4], hostMac[5]);
	int authStatus =
	    LdnStationAuthenticate(station, ifIndex, hostMac, &attempt->ad, &broadcast->keys, "mGBA", attempt->ad.appVersion);
	if (authStatus == LDN_AUTH_SUCCESS) {
		GBASIORFUTrace(broadcast->rfu, "LDN    authenticated with %04X; bringing up the Pia session...", attempt->deviceId);
	} else if (authStatus == LDN_AUTH_NO_RESPONSE) {
		GBASIORFUTrace(broadcast->rfu, "LDN    %04X did not respond to authentication (timed out after 3 attempts)", attempt->deviceId);
		goto disconnectAndDone;
	} else if (authStatus < 0) {
		GBASIORFUTrace(broadcast->rfu, "LDN    authentication with %04X failed: %s", attempt->deviceId, LdnStationLastError());
		goto disconnectAndDone;
	} else {
		GBASIORFUTrace(broadcast->rfu, "LDN    %04X rejected authentication (status code %d)", attempt->deviceId, authStatus);
		goto disconnectAndDone;
	}

	{
		// IP derivation: NOT re-scanning for a post-join advertisement (see ldn-pia-join.c's own notes - closing
		// and reopening a monitor while the station stays associated reliably wedges the radio on this hardware).
		// `host_ip` comes straight from the PRE-JOIN advertisement (`participants[0].ip` - the host is always
		// participant 0); `our_ip` is assumed to be the same /24 subnet with the last octet 2, the same fallback
		// the LDN-0.0.3 reference client's own LiveTransport uses, and the only sane value for a single joiner.
		uint8_t ourIp[4], hostIp[4];
		memcpy(hostIp, attempt->ad.participants[0].ip, 4);
		memcpy(ourIp, hostIp, 3);
		ourIp[3] = 2;

		struct LdnPiaSocket* piaSocket = LdnPiaSocketOpen(conn, ifIndex, ourMac);
		if (!piaSocket) {
			GBASIORFUTrace(broadcast->rfu, "LDN    could not open the Pia socket for %04X: %s", attempt->deviceId, LdnPiaSocketLastError());
			goto disconnectAndDone;
		}

		struct LdnPiaCrypto crypto;
		LdnPiaCryptoInit(&crypto, attempt->ad.ssid);
		struct LdnPiaConnect piaConn;
		LdnPiaConnectInit(&piaConn, ourMac, hostMac, ourIp, "mGBA");
		struct LdnPiaReliable* reliable = malloc(sizeof(*reliable)); // too large for the stack - see the project notes
		if (!reliable) {
			LdnPiaSocketClose(piaSocket);
			goto disconnectAndDone;
		}
		LdnPiaReliableInit(reliable, LDN_PIA_RELIABLE_RTO_BASE_MS, 200);

		// Drives the SAME Net(1)/Session(13)/RTT(3) handshake loop as ldn-pia-join.c's own tick loop, live-proven
		// against the real Switch - see the project notes for the two real bugs (zstd decompression needing the
		// frame's own exact length, and the outgoing header's flags byte being dynamic, not a fixed constant) that
		// had to be fixed before this ever reached ST_CONNECTED. Blocking here is fine: this whole function
		// already runs on its own thread specifically so the emulation thread is never blocked by it.
		uint16_t hostVar = 0x7620;
		uint64_t nonceCounter = 0;
		struct PiaPktids pktids = {0};
		DWORD deadline = GetTickCount() + kPiaConnectTimeoutMs;
		unsigned tick = 0;
		unsigned rawReceived = 0;
		DWORD piaStart = GetTickCount();
		// The retail Switch needs an ARP mapping for us before it will send us anything unicast (Reliable acks and its
		// own stream): in the Ryubing HOST role, only a PAIRWISE ARP straight to the Switch's MAC unblocked the return
		// path (group broadcast ARP was not enough, and no ARP reply was needed). We never sent any ARP at all. Send a
		// broadcast who-has plus a pairwise reply now, and again on the first datagram received below.
		bool arpSentOnRx = false;
		int arpRc1 = LdnPiaSocketSendArp(piaSocket, NULL, 1, ourIp, hostIp, NULL);
		int arpRc2 = LdnPiaSocketSendArp(piaSocket, hostMac, 2, ourIp, hostIp, hostMac);
		GBASIORFUTrace(broadcast->rfu, "LDN    sent ARP: broadcast who-has rc=%d, pairwise reply to host rc=%d", arpRc1, arpRc2);
		unsigned decryptFailed = 0;
		unsigned decompressFailed = 0;
		int lastState = piaConn.state;
		while ((int32_t) (deadline - GetTickCount()) > 0 && !LdnPiaConnectIsConnected(&piaConn)) {
			uint8_t datagram[LDN_PIA_MAX_DATAGRAM];
			uint8_t srcIp[4];
			size_t datagramLength = sizeof(datagram);
			while (LdnPiaSocketPoll(piaSocket, srcIp, datagram, &datagramLength)) {
				++rawReceived;
				if (!arpSentOnRx) {
					arpSentOnRx = true;
					int rc1 = LdnPiaSocketSendArp(piaSocket, NULL, 1, ourIp, hostIp, NULL);
					int rc2 = LdnPiaSocketSendArp(piaSocket, hostMac, 2, ourIp, hostIp, hostMac);
					GBASIORFUTrace(broadcast->rfu, "LDN    sent ARP on first rx: broadcast rc=%d, pairwise rc=%d", rc1, rc2);
				}
				GBASIORFUTrace(broadcast->rfu, "LDN    pia rx #%u t+%lums from %u.%u.%u.%u len=%zu", rawReceived, GetTickCount() - piaStart,
				               srcIp[0], srcIp[1], srcIp[2], srcIp[3], datagramLength);
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
							// Reliable(10) frames during the handshake (before ST_CONNECTED) are not expected and
							// are simply ignored here - the real data stream is opened only once connected (see
							// _frame below), matching ldn-pia-join.c's own behavior.
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
				static const char* const kStateNames[] = {"NET", "FINALIZE", "CONNECTED"};
				GBASIORFUTrace(broadcast->rfu, "LDN    Pia session with %04X: %s -> %s (raw=%u decryptFail=%u decompressFail=%u)",
				               attempt->deviceId, kStateNames[lastState], kStateNames[piaConn.state], rawReceived, decryptFailed,
				               decompressFailed);
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
				if (LdnPiaEncrypt(&crypto, tiled, tiledLength, ourIp, &header, outDatagram, &outDatagramLength)) {
					int sendRc = LdnPiaSocketSend(piaSocket, hostMac, ourIp, hostIp, outDatagram, outDatagramLength);
					GBASIORFUTrace(broadcast->rfu, "LDN    pia tx t+%lums proto=%u dst=%04X len=%zu rc=%d flags=%02X nonce=%llu",
					               GetTickCount() - piaStart, outMsgs[i].proto, outMsgs[i].dst, outDatagramLength, sendRc, header.flags,
					               (unsigned long long) nonceCounter);
				}
			}
			Sleep(16);
		}

		if (!LdnPiaConnectIsConnected(&piaConn)) {
			static const char* const kStateNames[] = {"NET", "FINALIZE", "CONNECTED"};
			GBASIORFUTrace(broadcast->rfu,
			               "LDN    Pia session with %04X did not reach CONNECTED within %ums (stuck at %s, raw=%u decryptFail=%u "
			               "decompressFail=%u)",
			               attempt->deviceId, kPiaConnectTimeoutMs, kStateNames[piaConn.state], rawReceived, decryptFailed, decompressFailed);
			free(reliable);
			LdnPiaSocketClose(piaSocket);
			goto disconnectAndDone;
		}

		// Success: hand everything off to the broadcast backend for frame()/sendData()/disconnect() to drive from
		// here on. `station`/`conn` are deliberately NOT closed below (handedOff) - they now belong to the session.
		GBASIORFUTrace(broadcast->rfu, "LDN    Pia session with %04X CONNECTED", attempt->deviceId);
		broadcast->piaConnHandle = conn;
		broadcast->piaStation = station;
		broadcast->piaIfIndex = ifIndex;
		broadcast->piaDeviceId = attempt->deviceId;
		broadcast->piaSocket = piaSocket;
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
		memcpy(broadcast->piaOurMac, ourMac, 6);
		memcpy(broadcast->piaHostMac, hostMac, 6);
		memcpy(broadcast->piaOurIp, ourIp, 4);
		memcpy(broadcast->piaHostIp, hostIp, 4);
		broadcast->piaHostVar = hostVar;
		broadcast->piaNonceCounter = nonceCounter;
		broadcast->piaPktids = pktids;
		broadcast->piaTick = tick;
		EnterCriticalSection(&broadcast->piaLock);
		broadcast->piaActive = true;
		LeaveCriticalSection(&broadcast->piaLock);
		ok = true;
		station = NULL;
		conn = NULL;
	}

disconnectAndDone:
	if (station) {
		LdnStationDisconnect(station, ifIndex);
	}
done:
	if (station) {
		LdnStationClose(station);
	}
	// (conn belongs to the parked monitor - never closed here.)
	// The event is queued and picked up by the driver on its own thread; safe to call from here.
	GBASIORFUConnectResult(broadcast->rfu, ok, attempt->deviceId, 0);
	free(attempt);
	return 0;
}
#endif

// Attempts real Wi-Fi association with the host that most recently advertised this device id (see
// _onAdvertisement), then LDN's own authentication handshake (separate from WPA2), then brings the Pia
// CONNECTION layer (Net/Session/RTT - see ldn-pia-connect.c) all the way to ST_CONNECTED. Runs on its own thread -
// the whole sequence can take several seconds, and the driver requires this call to return immediately. On
// success, ownership of the live Pia session (socket, station, connection, reliable window) passes to the
// broadcast backend's own fields for _frame()/_sendData()/_disconnect() to drive from then on.
static void _connect(struct GBASIORFUBackend* backend, uint16_t deviceId) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
#ifdef _WIN32
	if (!broadcast->haveKeys) {
		GBASIORFUTrace(broadcast->rfu, "LDN    connect to %04X requested, but no prod.keys are configured", deviceId);
		GBASIORFUConnectResult(broadcast->rfu, false, deviceId, 0);
		return;
	}
	EnterCriticalSection(&broadcast->lastAdLock);
	bool matches = broadcast->haveLastAd && broadcast->lastTrainerId == deviceId;
	struct LdnAdvertisement ad = broadcast->lastAd;
	unsigned channel = broadcast->lastChannel;
	uint8_t adMac[6];
	memcpy(adMac, broadcast->lastAdMac, 6);
	LeaveCriticalSection(&broadcast->lastAdLock);
	if (!matches) {
		GBASIORFUTrace(broadcast->rfu, "LDN    connect to %04X requested, but no matching advertisement is cached", deviceId);
		GBASIORFUConnectResult(broadcast->rfu, false, deviceId, 0);
		return;
	}
	// Only one attempt at a time; a stale thread from an earlier attempt (which always finishes on its own, see
	// _joinConnectThread) is joined first if one is somehow still around.
	_stopHop(broadcast);
	_joinConnectThread(broadcast);
	struct ConnectAttempt* attempt = malloc(sizeof(*attempt));
	if (!attempt) {
		GBASIORFUConnectResult(broadcast->rfu, false, deviceId, 0);
		return;
	}
	attempt->broadcast = broadcast;
	attempt->deviceId = deviceId;
	// The channel the advertisement was HEARD on is not necessarily the network's: the monitor hops 1/6/11 and can catch a
	// frame from a neighbouring channel. Two of three attempts against the same room associated on the heard channel (6)
	// and were rejected (WLAN status 1); the third, on channel 1, worked. The advertisement itself names the channel the
	// host's network is on, so use that when it is a valid 2.4 GHz channel.
	if (ad.advertisedChannel >= 1 && ad.advertisedChannel <= 13) {
		GBASIORFUTrace(broadcast->rfu, "LDN    advertisement heard on channel %u, network advertises channel %u", channel, ad.advertisedChannel);
		channel = ad.advertisedChannel;
	}
	attempt->channel = channel;
	memcpy(attempt->adMac, adMac, 6);
	attempt->ad = ad;
	broadcast->connectThread = CreateThread(NULL, 0, _connectThreadProc, attempt, 0, NULL);
	if (!broadcast->connectThread) {
		free(attempt);
		GBASIORFUConnectResult(broadcast->rfu, false, deviceId, 0);
	}
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
#endif

#ifdef _WIN32
// Sends one Pia message as its own datagram - mirrors ldn-pia-join.c's own `_sendRaw`/`_sendMessage` exactly
// (including the live-confirmed dynamic header flags byte; see its comments for why), just operating on the
// broadcast backend's own persistent session fields instead of a separate PiaSender struct.
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
	int rc = LdnPiaSocketSend(broadcast->piaSocket, broadcast->piaHostMac, broadcast->piaOurIp, broadcast->piaHostIp, datagram, datagramLength);
	GBASIORFUTrace(broadcast->rfu, "PIA    tx hdr proto=%u dst=%04X src=%04X pktid=%04X flags=%02X footer=%u len=%zu rc=%d", proto, header.dst, header.src,
	               header.pktid, header.flags, header.footer, datagramLength, rc);
	return rc == 0;
}

static bool _piaSendMessage(struct GBASIORFUBroadcast* broadcast, const struct LdnPiaOutMessage* msg) {
	return _piaSendRaw(broadcast, msg->proto, msg->dst, msg->src, msg->establishing, msg->footer, msg->compress, false, 0, msg->payload, msg->length);
}
#endif

// Drives the live Pia session once connected: polls the raw socket, decrypts/decompresses/tiles each datagram,
// routes Net/Session/RTT messages to LdnPiaConnectOnMessage and Reliable(10) frames to LdnPiaReliableReceive
// (delivering each newly in-order payload to the driver via GBASIORFUDataReceived - skipping the very first frame
// in either direction, the FLAGSA_INIT stream-opening metadata frame, which is not real RFU data), then drains
// LdnPiaConnect's outbox and LdnPiaReliable's due retransmits/acks and sends them. Mirrors ldn-pia-join.c's own
// tick loop exactly, just spread across per-frame calls instead of a tight local loop, and delivering real data
// instead of only tracing it. Called once per emulated frame (~59.7 Hz, matching Pia's own real-hardware-measured
// cadence - see the project notes) on the emulation thread; `piaActive` is read without a lock (see the struct
// comment - after the connect thread's one-time handoff, only the emulation thread ever touches these fields).
static void _frame(struct GBASIORFUBackend* backend) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
#ifdef _WIN32
	if (!broadcast->piaActive) {
		return;
	}
	struct LdnPiaConnect* conn = &broadcast->piaConn;
	struct LdnPiaReliable* reliable = broadcast->piaReliable;

	uint8_t datagram[LDN_PIA_MAX_DATAGRAM];
	uint8_t srcIp[4];
	size_t datagramLength = sizeof(datagram);
	while (LdnPiaSocketPoll(broadcast->piaSocket, srcIp, datagram, &datagramLength)) {
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

	if (LdnPiaConnectIsConnected(conn) && !broadcast->piaOpenedStream) {
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
// stream. Silently dropped if the Pia session is not yet fully up (should not happen in practice - the driver
// only reaches a state where it issues SendData after GBASIORFUConnectResult(true) fired, which only happens
// after this backend's own connect thread already brought the Pia session to ST_CONNECTED and opened the stream).
static void _sendData(struct GBASIORFUBackend* backend, const uint8_t* data, size_t length) {
	struct GBASIORFUBroadcast* broadcast = (struct GBASIORFUBroadcast*) backend;
#ifdef _WIN32
	if (!broadcast->piaActive || !broadcast->piaOpenedStream) {
		GBASIORFUTrace(broadcast->rfu, "PIA    sendData %zu bytes DROPPED (active=%d opened=%d)", length, broadcast->piaActive, broadcast->piaOpenedStream);
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

// Client role: leave the host (slotMask is ignored - see rfu.h). Tears down the live Pia session, if any.
static void _disconnect(struct GBASIORFUBackend* backend, unsigned slotMask) {
	(void) slotMask;
	_piaTeardown((struct GBASIORFUBroadcast*) backend);
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
	// Created here, not in _init(): the SIO driver's GBASIORFUInit() calls backend->reset() (_reset(), which tears
	// down any Pia session via piaLock) BEFORE it ever calls backend->init() - so these must be valid from the
	// moment the backend exists, not from whenever _init() happens to run relative to the first reset.
#ifdef _WIN32
	broadcast->stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	InitializeCriticalSection(&broadcast->lastAdLock);
	InitializeCriticalSection(&broadcast->piaLock);
	InitializeCriticalSection(&broadcast->monitorLock);
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
