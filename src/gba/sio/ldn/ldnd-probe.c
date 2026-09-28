/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * ldnd-probe: checks a running ldnd from C, through the same client the ldnd backend uses.
 *
 *   ldnd-probe [--pipe <path>] [--scan] [--join] [--seconds N]
 *
 * Says Hello and prints what ldnd reports about itself: version, capabilities, radio. --scan then waits for the radio
 * and lists the LDN networks around for N seconds (default 10), decoding FireRed/LeafGreen rooms' beacons. --join goes
 * on to join the first FireRed/LeafGreen room it finds, opens the Pia UDP channel, and prints what arrives on it for N
 * seconds before leaving: a host starts talking Pia to a new participant straight away, so something should.
 * The pipe is LDN_DAEMON if that is set, otherwise ldnd's default (\\.\pipe\ldnd).
 */
#include "ldn.h"
#include "ldnd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

enum {
	kScanDwellMs = 400,
	kMaxNetworks = 16,
	kJoinBudgetMs = 70000,
	kPiaPort = 12345,
	kDatagramsShown = 10,
};

static const uint8_t kChannels[] = {1, 6, 11};

static HANDLE sRadioReady;
static volatile LONG sDatagrams;

static const char* _radioStateName(unsigned state) {
	static const char* const names[] = {"idle", "attaching", "ready", "lost", "failed"};
	return state < sizeof(names) / sizeof(names[0]) ? names[state] : "unknown";
}

static void _printResult(const char* operation, int code, const struct LdndResult* result) {
	printf("%s failed: %s%s%s\n", operation, LdndStatusName(code), result->message[0] ? ": " : "", result->message);
}

static void _printParticipant(unsigned index, const struct LdndParticipant* participant) {
	printf("      %u: %u.%u.%u.%u  %02x:%02x:%02x:%02x:%02x:%02x  \"%.*s\"\n", index, participant->ip[0], participant->ip[1], participant->ip[2],
	       participant->ip[3], participant->mac[0], participant->mac[1], participant->mac[2], participant->mac[3], participant->mac[4],
	       participant->mac[5], participant->nameLength, (const char*) participant->name);
}

static bool _printNetwork(const struct LdndNetworkInfo* network) {
	printf("  %02x:%02x:%02x:%02x:%02x:%02x  channel %u  title %016llx  scene %u  %u/%u participants  accept policy %u  app data %u bytes\n",
	       network->address[0], network->address[1], network->address[2], network->address[3], network->address[4], network->address[5],
	       network->channel, (unsigned long long) network->localCommunicationId, network->sceneId, network->numParticipants,
	       network->maxParticipants, network->acceptPolicy, network->applicationDataLength);
	for (unsigned i = 0; i < network->participantCount; ++i) {
		if (network->participants[i].connected) {
			_printParticipant(i, &network->participants[i]);
		}
	}
	struct LdnRfuBeacon beacon;
	if (!LdnDecodeRfuBeacon(network->applicationData, network->applicationDataLength, &beacon)) {
		return false;
	}
	printf("      FireRed/LeafGreen room: trainer %04X \"%s\"\n", beacon.trainerId, beacon.name);
	return true;
}

static void _onEvent(void* context, const struct LdndEvent* event) {
	(void) context;
	switch (event->kind) {
	case LDND_EVENT_RADIO_STATE:
		printf("  [ldnd] radio %s%s%s\n", _radioStateName(event->radioState), event->message ? ": " : "", event->message ? event->message : "");
		if (event->radioState == LDND_RADIO_READY) {
			SetEvent(sRadioReady);
		}
		break;
	case LDND_EVENT_JOIN:
	case LDND_EVENT_LEAVE:
		printf("  [ldnd] participant %s:\n", event->kind == LDND_EVENT_JOIN ? "joined" : "left");
		_printParticipant(event->index, event->participant);
		break;
	case LDND_EVENT_DISCONNECT:
		printf("  [ldnd] network %u is gone (reason %u)\n", event->handle, event->reason);
		break;
	case LDND_EVENT_APP_DATA_CHANGED:
		printf("  [ldnd] network %u's application data changed (%zu -> %zu bytes)\n", event->handle, event->oldDataLength, event->newDataLength);
		break;
	case LDND_EVENT_POLICY_CHANGED:
		printf("  [ldnd] network %u's accept policy changed (%u -> %u)\n", event->handle, event->oldPolicy, event->newPolicy);
		break;
	case LDND_EVENT_CHANNEL_ERROR:
		printf("  [ldnd] channel %u: %s: %s\n", event->handle, LdndStatusName(event->status), event->message);
		break;
	default:
		// Scan progress (the reply lists it all) and log lines (not subscribed to).
		return;
	}
	fflush(stdout);
}

static void _onData(void* context, uint32_t channel, const uint8_t* payload, size_t length) {
	(void) context;
	uint8_t peer[4];
	uint16_t port;
	const uint8_t* data;
	size_t dataLength;
	if (!LdndParseDatagram(payload, length, peer, &port, &data, &dataLength)) {
		return;
	}
	LONG count = InterlockedIncrement(&sDatagrams);
	if (count > kDatagramsShown) {
		return;
	}
	static const uint8_t kPiaMagic[4] = {0x32, 0xAB, 0x98, 0x64};
	bool pia = dataLength >= 4 && !memcmp(data, kPiaMagic, 4);
	printf("  [data] channel %u: %u bytes from %u.%u.%u.%u:%u%s\n", channel, (unsigned) dataLength, peer[0], peer[1], peer[2], peer[3], port,
	       pia ? " (Pia)" : "");
	fflush(stdout);
}

// Scans the three LDN channels over and over for up to `seconds`, printing each network the first time it is seen.
// With `room`, stops at the first FireRed/LeafGreen room and returns it there.
static bool _scan(struct LdndConnection* conn, unsigned seconds, struct LdndNetworkInfo* room) {
	uint8_t seen[kMaxNetworks][6];
	size_t seenCount = 0;
	bool scanned = false;
	DWORD deadline = GetTickCount() + seconds * 1000;
	unsigned pass = 0;
	while ((int32_t) (deadline - GetTickCount()) > 0) {
		uint8_t channel = kChannels[pass++ % (sizeof(kChannels) / sizeof(kChannels[0]))];
		struct LdndScanRequest request = {&channel, 1, kScanDwellMs};
		struct LdndNetworkInfo networks[kMaxNetworks];
		size_t count;
		struct LdndResult result;
		int code = LdndScan(conn, &request, networks, kMaxNetworks, &count, &result);
		if (code != LDND_OK) {
			_printResult("Scan", code, &result);
			if (code < 0) {
				return false;
			}
			Sleep(1000);
			continue;
		}
		scanned = true;
		for (size_t i = 0; i < count; ++i) {
			bool known = false;
			for (size_t j = 0; j < seenCount && !known; ++j) {
				known = !memcmp(seen[j], networks[i].address, 6);
			}
			if (known) {
				continue;
			}
			if (seenCount < kMaxNetworks) {
				memcpy(seen[seenCount++], networks[i].address, 6);
			}
			bool isRoom = _printNetwork(&networks[i]);
			fflush(stdout);
			if (room && isRoom) {
				*room = networks[i];
				return true;
			}
		}
	}
	printf("%zu network(s) seen in %u s.\n", seenCount, seconds);
	return scanned && !room;
}

static int _join(struct LdndConnection* conn, const struct LdndNetworkInfo* room, unsigned seconds) {
	uint64_t deviceId = 0;
	LdnRandomBytes((uint8_t*) &deviceId, sizeof(deviceId));
	struct LdndConnectRequest request = {
		.network = room,
		.password = kLdnGbaPassphrase,
		.passwordLength = sizeof(kLdnGbaPassphrase),
		.name = "mGBA",
		.appVersion = room->appVersion,
		.platform = 0,
		.enableChallenge = true,
		.deviceId = deviceId,
		.timeoutMs = kJoinBudgetMs,
	};
	printf("\nJoining (ldnd's budget: %u s; a join can take most of it)...\n", kJoinBudgetMs / 1000);
	fflush(stdout);
	struct LdndNetworkReply reply;
	struct LdndResult result;
	DWORD start = GetTickCount();
	int code = LdndConnect(conn, &request, &reply, &result);
	DWORD elapsed = GetTickCount() - start;
	if (code != LDND_OK) {
		_printResult("Connect", code, &result);
		if (reply.haveAuthStatus) {
			printf("  the host refused us with LDN auth status %u\n", reply.authStatus);
		}
		printf("  (after %lu ms)\n", (unsigned long) elapsed);
		return 1;
	}
	printf("Joined in %lu ms as participant %u (network handle %u):\n", (unsigned long) elapsed, reply.participantIndex, reply.handle);
	_printNetwork(&reply.network);

	int status = 0;
	uint32_t channel;
	uint16_t port;
	code = LdndOpenDatagram(conn, reply.handle, kPiaPort, &channel, &port, &result);
	if (code != LDND_OK) {
		_printResult("OpenDatagram", code, &result);
		status = 1;
	} else {
		printf("\nPia channel %u open on UDP %u; listening for %u s...\n", channel, port, seconds);
		fflush(stdout);
		DWORD deadline = GetTickCount() + seconds * 1000;
		while ((int32_t) (deadline - GetTickCount()) > 0 && LdndIsOpen(conn)) {
			Sleep(100);
		}
		LONG datagrams = InterlockedCompareExchange(&sDatagrams, 0, 0);
		printf("%ld datagram(s) received.\n", (long) datagrams);
		if (!datagrams) {
			printf("  The join worked, but nothing arrived: is the host still there?\n");
			status = 1;
		}
	}
	code = LdndCloseNetwork(conn, reply.handle, &result);
	if (code != LDND_OK) {
		_printResult("CloseNetwork", code, &result);
	} else {
		printf("Left the network.\n");
	}
	return status;
}

int main(int argc, char** argv) {
	const char* pipe = getenv("LDN_DAEMON");
	bool scan = false;
	bool join = false;
	unsigned seconds = 10;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--pipe") && i + 1 < argc) {
			pipe = argv[++i];
		} else if (!strcmp(argv[i], "--scan")) {
			scan = true;
		} else if (!strcmp(argv[i], "--join")) {
			join = true;
		} else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			seconds = (unsigned) strtoul(argv[++i], NULL, 10);
		} else {
			fprintf(stderr, "usage: ldnd-probe [--pipe <path>] [--scan] [--join] [--seconds N]\n");
			return 2;
		}
	}
	if (pipe && !pipe[0]) {
		pipe = NULL;
	}

	sRadioReady = CreateEventA(NULL, TRUE, FALSE, NULL);
	struct LdndCallbacks callbacks = {NULL, _onEvent, _onData};
	struct LdndHello hello;
	struct LdndResult result;
	struct LdndConnection* conn = LdndOpen(pipe, "ldnd-probe", "1", &callbacks, &hello, &result);
	if (!conn) {
		_printResult("Hello", result.code, &result);
		if (hello.protocolVersion) {
			printf("  ldnd %s speaks protocol %u; this client speaks %u\n", hello.daemonVersion, hello.protocolVersion, LDND_PROTOCOL_VERSION);
		}
		return 1;
	}
	printf("Connected to ldnd %s (protocol %u).\n", hello.daemonVersion, hello.protocolVersion);
	printf("  LDN: %s%s%s\n", hello.ldnCapabilities & LDND_CAP_SCAN ? "scan " : "", hello.ldnCapabilities & LDND_CAP_JOIN ? "join " : "",
	       hello.ldnCapabilities & LDND_CAP_HOST ? "host" : "");
	printf("  radio: %s\n", hello.radioReady ? "ready" : "not ready");
	fflush(stdout);

	int status = 0;
	if (scan || join) {
		if (!hello.radioReady) {
			printf("Waiting up to %u s for the radio...\n", seconds);
			fflush(stdout);
			if (WaitForSingleObject(sRadioReady, seconds * 1000) != WAIT_OBJECT_0) {
				printf("  still not ready; scanning anyway\n");
			}
		}
		printf("\nScanning channels 1, 6 and 11 for up to %u s...\n", seconds);
		fflush(stdout);
		struct LdndNetworkInfo room;
		if (!_scan(conn, seconds, join ? &room : NULL)) {
			status = 1;
			if (join) {
				printf("No FireRed/LeafGreen room found to join.\n");
			}
		} else if (join) {
			status = _join(conn, &room, seconds);
		}
	}
	LdndClose(conn);
	CloseHandle(sRadioReady);
	return status;
}

#else

int main(void) {
	fprintf(stderr, "ldnd-probe: ldnd is only reachable on Windows.\n");
	return 1;
}

#endif
