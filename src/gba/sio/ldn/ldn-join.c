/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * ldn-join: scans for a Switch's FRLG LDN advertisement (like ldn-scan), then attempts to actually associate with
 * it - WPA2-PSK/CCMP, using the key LDN derives for the session, exactly the way a real client would. This is the
 * first real test of ldn-station.c, the Wi-Fi-layer half of "joining". It does not go any further: LDN's own
 * authentication handshake (separate from WPA2) and the Pia transport on top of that are not implemented yet, so
 * a successful run here proves association only, not that trading would work.
 *
 *   ldn-join --keys <prod.keys> [--seconds N] [--channels 1,6,11]
 */
#include "ldn-auth.h"
#include "ldn-monitor.h"
#include "ldn-station.h"
#include "ldn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

struct Found {
	bool haveIt;
	uint8_t mac[6];
	unsigned channel;
	struct LdnAdvertisement ad;
};

struct ScanState {
	struct Found found;
	struct LdnKeys keys;
};

static void _onAdvertisement(void* context, const uint8_t* mac, const uint8_t* body, size_t bodyLength, unsigned channel) {
	struct ScanState* state = context;
	if (state->found.haveIt) {
		return;
	}
	struct LdnAdvertisement ad;
	if (!LdnDecodeAdvertisement(body, bodyLength, &state->keys, &ad) || !ad.infoDecoded) {
		return;
	}
	printf("Found it: \"%s\" on channel %u\n", ad.participants[0].present ? ad.participants[0].name : "?", channel);
	memcpy(state->found.mac, mac, 6);
	state->found.channel = channel;
	state->found.ad = ad;
	state->found.haveIt = true;
}

int main(int argc, char** argv) {
	const char* keysPath = NULL;
	unsigned seconds = 30;
	unsigned channels[8] = {1, 6, 11};
	unsigned channelCount = 3;

	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
			keysPath = argv[++i];
		} else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			seconds = (unsigned) atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--channels") && i + 1 < argc) {
			channelCount = 0;
			for (char* token = strtok(argv[++i], ","); token && channelCount < 8; token = strtok(NULL, ",")) {
				channels[channelCount++] = (unsigned) atoi(token);
			}
		} else if (!strcmp(argv[i], "--debug")) {
			gLdnDebug = 1;
		}
	}
	if (!keysPath) {
		fprintf(stderr, "--keys <prod.keys> is required (the WLAN key cannot be derived without it).\n");
		return 1;
	}

	struct ScanState state = {0};
	if (!LdnKeysLoad(keysPath, &state.keys)) {
		fprintf(stderr, "Could not read the four LDN keys from \"%s\".\n", keysPath);
		return 1;
	}

	struct LdnMonitor* monitor = LdnMonitorOpen(NULL, channels[0], NULL, _onAdvertisement, &state);
	if (!monitor) {
		fprintf(stderr, "%s\n", LdnMonitorLastError());
		return 1;
	}
	printf("Scanning for up to %u seconds...\n", seconds);
	fflush(stdout);

#ifdef _WIN32
	DWORD end = GetTickCount() + seconds * 1000;
	unsigned hop = 1;
	while (!state.found.haveIt && (int32_t) (end - GetTickCount()) > 0) {
		unsigned channel = channels[hop++ % channelCount];
		LdnMonitorSetChannel(monitor, channel);
		Sleep(400);
	}
#endif

	if (!state.found.haveIt) {
		LdnMonitorClose(monitor);
		printf("Nothing found.\n");
		return 1;
	}

	uint8_t wlanKey[16];
	if (!LdnDeriveWlanKey(&state.keys, state.found.ad.protocol, state.found.ad.serverRandom, wlanKey)) {
		fprintf(stderr, "Could not derive the WLAN key.\n");
		return 1;
	}
	char ssid[33];
	LdnAdvertisementWlanSsid(&state.found.ad, ssid);
	printf("SSID: %s\n", ssid);
	printf("WLAN key: ");
	for (int i = 0; i < 16; ++i) {
		printf("%02X", wlanKey[i]);
	}
	printf("\n");

	// Shares the monitor's still-open ldnd connection rather than opening a second one - ldnd only accepts one
	// pipe connection at a time, so a fresh LdndOpen() here would race the first connection's teardown.
	struct LdnStation* station = LdnStationOpen(LdnMonitorConnection(monitor));
	if (!station) {
		fprintf(stderr, "%s\n", LdnStationLastError());
		LdnMonitorClose(monitor);
		return 1;
	}
	uint32_t ifIndex;
	uint8_t ourMac[6];
	if (LdnStationFindInterface(station, &ifIndex, ourMac)) {
		fprintf(stderr, "%s\n", LdnStationLastError());
		LdnStationClose(station);
		LdnMonitorClose(monitor);
		return 1;
	}
	printf("Station interface index %u (our MAC %02X:%02X:%02X:%02X:%02X:%02X). Associating on channel %u...\n", ifIndex, ourMac[0],
	       ourMac[1], ourMac[2], ourMac[3], ourMac[4], ourMac[5], state.found.channel);
	fflush(stdout);

	uint8_t hostMac[6];
	int error = LdnStationConnect(station, ifIndex, ssid, state.found.channel, wlanKey, state.found.mac, hostMac);
	if (error) {
		fprintf(stderr, "FAILED: %s\n", LdnStationLastError());
		LdnStationClose(station);
		LdnMonitorClose(monitor);
		return 1;
	}
	printf("ASSOCIATED with %02X:%02X:%02X:%02X:%02X:%02X - keys installed, station authorized.\n", hostMac[0], hostMac[1], hostMac[2],
	       hostMac[3], hostMac[4], hostMac[5]);
	fflush(stdout);

	printf("Authenticating (LDN's own handshake, on top of the Wi-Fi association)...\n");
	fflush(stdout);
	int authStatus = LdnStationAuthenticate(station, ifIndex, hostMac, &state.found.ad, &state.keys, "mGBA", state.found.ad.appVersion);
	if (authStatus == LDN_AUTH_SUCCESS) {
		printf("AUTHENTICATED - host accepted this joiner into its LDN session.\n");
	} else if (authStatus == LDN_AUTH_NO_RESPONSE) {
		printf("No authentication response from the host (timed out after 3 attempts).\n");
	} else if (authStatus < 0) {
		printf("Authentication transport error: %s\n", LdnStationLastError());
	} else {
		printf("Host rejected authentication (status code %d).\n", authStatus);
	}
	printf("(Even on success, the Pia transport on top of this is not implemented yet, so no game data flows.)\n");

	LdnStationDisconnect(station, ifIndex);
	LdnStationClose(station);
	LdnMonitorClose(monitor);
	return 0;
}
