/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * uds-esp32-probe: R1 and the first half of R2 of the real-air stage (doc/uds-wrapper-plan.md).
 *
 *   uds-esp32-probe [COMx] [--seconds N] [--channel N] [--all]
 *
 * Opens Azahar's ESP32-S3 UDS radio board, waits for its Hello answer and prints the firmware version and the board's MAC, then
 * starts the radio and listens for 3DS local-wireless hosts (beacons with the Nintendo vendor element). It hops channels 1, 6 and 11
 * (or stays on --channel). Prints each new host once (transmitter, channel, signal, comm id, network id, application data) and a
 * summary. With --all it also counts every frame the board forwards. No key is needed or used.
 *
 * Close Azahar first (one program at a time can have the board), and give a retail 3DS a VC Pokemon game hosting a trade.
 */
#include <mgba/internal/gb/sio/uds-esp32.h>
#include <mgba/internal/gb/sio/uds-room.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define NOW_MS() ((unsigned) GetTickCount64())
#else
#include <time.h>
static unsigned NOW_MS(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#endif

#define MAX_HOSTS 32

struct Seen {
	uint8_t mac[6];
	unsigned count;
	int bestRssi;
};

static struct Seen sSeen[MAX_HOSTS];
static unsigned sSeenCount;
static unsigned sBeacons, sNintendoBeacons, sOther;
static bool sAll;

static void onRx(void* context, const struct UDSEsp32Rx* rx) {
	(void) context;
	if (rx->length < 24 + 12) {
		++sOther;
		return;
	}
	// Management frame (type 0), beacon (subtype 8)
	if ((rx->mpdu[0] & 0xFC) != 0x80) {
		++sOther;
		return;
	}
	++sBeacons;
	struct UDSRoomHost host;
	memset(&host, 0, sizeof(host));
	if (!udsRoomParseBeacon(rx->mpdu + 24, rx->length - 24, &host)) {
		return;
	}
	++sNintendoBeacons;
	const uint8_t* transmitter = &rx->mpdu[10];
	unsigned i;
	for (i = 0; i < sSeenCount; ++i) {
		if (!memcmp(sSeen[i].mac, transmitter, 6)) {
			++sSeen[i].count;
			if (rx->rssi > sSeen[i].bestRssi) {
				sSeen[i].bestRssi = rx->rssi;
			}
			return;
		}
	}
	if (sSeenCount < MAX_HOSTS) {
		memcpy(sSeen[sSeenCount].mac, transmitter, 6);
		sSeen[sSeenCount].count = 1;
		sSeen[sSeenCount].bestRssi = rx->rssi;
		++sSeenCount;
	}
	printf("host %02X:%02X:%02X:%02X:%02X:%02X  channel %u  rssi %d dBm  comm id %08X  network id %08X%s\n", transmitter[0], transmitter[1],
	       transmitter[2], transmitter[3], transmitter[4], transmitter[5], rx->channel, rx->rssi, host.commId, host.networkId,
	       host.commId == UDS_PIA_COMM_ID ? "  (Game Boy Virtual Console)" : "");
	printf("     application data (%u bytes, first 16):", host.appDataSize);
	for (i = 0; i < 16; ++i) {
		printf(" %02X", host.appData[i]);
	}
	printf("\n");
}

static void onStatus(void* context, uint8_t requestType, int32_t result) {
	(void) context;
	if (result != 0) {
		printf("board: request %02X failed (%s)\n", requestType, udsEsp32StatusText(result));
	}
}

static void onLog(void* context, const char* text, size_t length) {
	(void) context;
	printf("board log: %.*s\n", (int) length, text);
}

int main(int argc, char** argv) {
	const char* port = NULL;
	unsigned seconds = 20;
	int fixedChannel = 0;
	int i;
	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			seconds = (unsigned) atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--channel") && i + 1 < argc) {
			fixedChannel = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--all")) {
			sAll = true;
		} else if (argv[i][0] != '-') {
			port = argv[i];
		}
	}
	struct UDSEsp32 esp;
	struct UDSEsp32Handlers handlers = {NULL, onRx, onStatus, onLog, NULL};
	if (!udsEsp32Open(&esp, port, &handlers)) {
		printf("no board found%s%s (is another program using it? is it plugged in through the native USB port?)\n", port ? " on " : "", port ? port : "");
		return 1;
	}
	printf("port open; waiting for the board to boot and answer Hello (up to 15 s)...\n");
	if (!udsEsp32WaitReady(&esp, 15000)) {
		printf("no HelloAck. A board running the GB-Link LDN firmware speaks a different framing and will not answer this one; the board must run\n"
		       "Azahar's esp32-uds-bridge firmware.%s\n",
		       esp.decoder.framesBad ? " (some bytes arrived that were not a valid frame, which fits the wrong firmware)" : "");
		udsEsp32Close(&esp);
		return 1;
	}
	printf("firmware %u.%u, protocol %u, board MAC %02X:%02X:%02X:%02X:%02X:%02X\n", esp.info.major, esp.info.minor, esp.info.proto,
	       esp.info.factoryMac[0], esp.info.factoryMac[1], esp.info.factoryMac[2], esp.info.factoryMac[3], esp.info.factoryMac[4],
	       esp.info.factoryMac[5]);

	// A locally administered address of our own for the radio (the board listens on a twin of it; see the firmware's README).
	uint8_t mac[6] = {0x02, 0x47, 0x42, 0x55, 0x44, 0x53};
	static const uint8_t channels[3] = {1, 6, 11};
	uint8_t channel = fixedChannel ? (uint8_t) fixedChannel : channels[0];
	if (!udsEsp32Start(&esp, channel, mac, true)) {
		printf("could not send Start\n");
		udsEsp32Close(&esp);
		return 1;
	}
	printf("listening for %u s on %s...\n", seconds, fixedChannel ? "one channel" : "channels 1, 6 and 11");
	unsigned start = NOW_MS(), lastHop = start, hop = 0;
	while (NOW_MS() - start < seconds * 1000u) {
		if (!udsEsp32Poll(&esp)) {
			printf("the serial port failed\n");
			break;
		}
		if (!fixedChannel && NOW_MS() - lastHop >= 400) {
			hop = (hop + 1) % 3;
			udsEsp32SetChannel(&esp, channels[hop]);
			lastHop = NOW_MS();
		}
#ifdef _WIN32
		Sleep(2);
#endif
	}
	udsEsp32Stop(&esp);
	// Let the Stop answer arrive
	unsigned wait = NOW_MS();
	while (NOW_MS() - wait < 300) {
		udsEsp32Poll(&esp);
	}
	printf("\nforwarded frames: %u (beacons %u, of which Nintendo hosts %u, other %u); status replies %u; bad serial frames %u\n", esp.rxFrames,
	       sBeacons, sNintendoBeacons, sOther, esp.statusFrames, esp.decoder.framesBad);
	for (i = 0; i < (int) sSeenCount; ++i) {
		printf("  host %02X:%02X:%02X:%02X:%02X:%02X: %u beacons, best signal %d dBm\n", sSeen[i].mac[0], sSeen[i].mac[1], sSeen[i].mac[2],
		       sSeen[i].mac[3], sSeen[i].mac[4], sSeen[i].mac[5], sSeen[i].count, sSeen[i].bestRssi);
	}
	if (!sSeenCount) {
		printf("no 3DS host heard. Is a retail 3DS hosting a VC Pokemon trade (waiting at the trade screen) within a few metres?\n");
	}
	udsEsp32Close(&esp);
	return 0;
}
