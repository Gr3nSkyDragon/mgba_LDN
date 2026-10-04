/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * uds-air-probe: R3 and R4 of the real-air stage (doc/uds-wrapper-plan.md). Joins a retail 3DS that is hosting a Game Boy Virtual
 * Console trade, over the air through the ESP32 board, and runs the Pia session as far as "game stream may start".
 *
 *   uds-air-probe <key file> [COMx] [--seconds N] [--hold N]
 *
 * <key file> is the file Settings > BIOS > "3DS UDS key file" points at (uds-keyfile.h). Close Azahar first. Start a VC Pokemon game on the
 * 3DS and host a trade (the "wait for a trainer" screen); this program scans, joins the first Game Boy VC host it hears, and prints each
 * step with the time. The host needs to accept the trainer on its screen if the game asks. With --hold N it keeps the link up for N
 * more seconds after the Pia session is up, to see that it stays up. No Game Boy is attached: this is the network only.
 */
#include <mgba/internal/gb/sio/uds-joiner.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define NOW_MS() ((unsigned) GetTickCount64())
#define PAUSE_MS(ms) Sleep(ms)
#else
#include <time.h>
#include <unistd.h>
static unsigned NOW_MS(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#define PAUSE_MS(ms) usleep((ms) *1000)
#endif

int main(int argc, char** argv) {
	if (argc < 2) {
		printf("usage: %s <UDS key file> [COMx] [--seconds N] [--hold N]\n", argv[0]);
		return 2;
	}
	const char* keyFile = argv[1];
	const char* port = NULL;
	unsigned seconds = 60, hold = 0;
	int i;
	for (i = 2; i < argc; ++i) {
		if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			seconds = (unsigned) atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--hold") && i + 1 < argc) {
			hold = (unsigned) atoi(argv[++i]);
		} else if (argv[i][0] != '-') {
			port = argv[i];
		}
	}
	static struct UDSJoiner joiner;
	uint16_t name[UDS_NAME_WORDS] = {'M', 'G', 'B', 'A'};
	char error[256];
	printf("opening the board (it resets when the port opens; this takes a few seconds)...\n");
	if (!udsJoinerOpenRadio(&joiner, name, port, keyFile, error, sizeof(error))) {
		printf("cannot start: %s\n", error);
		return 1;
	}
	printf("port open, waiting for the board, then scanning for a Game Boy Virtual Console host (up to %u s)...\n", seconds);
	unsigned start = NOW_MS();
	int lastRoom = -1, lastSession = -1;
	bool up = false;
	unsigned upAt = 0;
	static const char* const roomNames[] = {"scanning", "authenticating", "associated, EAPoL start sent", "joined (node id assigned)"};
	static const char* const sessionNames[] = {"idle", "setup messages", "joined: the game stream may start", "closed"};
	bool wasReady = false;
	while (NOW_MS() - start < seconds * 1000u) {
		udsJoinerPoll(&joiner, NOW_MS() - start);
		if (joiner.radio.state == UDS_AIR_FAILED) {
			printf("%6u ms  radio failed: %s\n", NOW_MS() - start, joiner.radio.error);
			break;
		}
		if (!wasReady && udsAirRadioReady(&joiner.radio)) {
			wasReady = true;
			printf("%6u ms  radio up (firmware %u.%u)\n", NOW_MS() - start, joiner.radio.esp.info.major, joiner.radio.esp.info.minor);
		}
		if ((int) joiner.room.state != lastRoom) {
			lastRoom = joiner.room.state;
			printf("%6u ms  room: %s", NOW_MS() - start, roomNames[lastRoom]);
			if (lastRoom != UDS_ROOM_SCAN) {
				printf("  (host %02X:%02X:%02X:%02X:%02X:%02X, network %08X)", joiner.room.host.mac[0], joiner.room.host.mac[1],
				       joiner.room.host.mac[2], joiner.room.host.mac[3], joiner.room.host.mac[4], joiner.room.host.mac[5],
				       joiner.room.host.networkId);
			}
			if (lastRoom == UDS_ROOM_JOINED) {
				printf("  node %u of %u", joiner.room.host.nodeId, joiner.room.host.connectedNodes);
			}
			printf("\n");
		}
		int session = joiner.sessionActive ? (int) joiner.session.state : -1;
		if (session != lastSession) {
			lastSession = session;
			if (session >= 0) {
				printf("%6u ms  Pia session: %s (frames in %u, out %u)\n", NOW_MS() - start, sessionNames[session],
				       joiner.session.framesReceived, joiner.session.framesSent);
			}
		}
		if (!up && udsJoinerReady(&joiner)) {
			up = true;
			upAt = NOW_MS();
			if (!hold) {
				break;
			}
			printf("holding the link for %u s...\n", hold);
		}
		if (up && NOW_MS() - upAt >= hold * 1000u) {
			break;
		}
		if (up && !udsJoinerReady(&joiner)) {
			printf("%6u ms  the link was lost\n", NOW_MS() - start);
			break;
		}
		PAUSE_MS(1);
	}
	struct UDSAirRadio* r = &joiner.radio;
	printf("\n%s\n", up && udsJoinerReady(&joiner) ? "RESULT: joined and the Pia session is up" : up ? "RESULT: the session came up and was lost" : "RESULT: did not get as far as the Pia session");
	printf("radio: beacons %u, frames sent %u (send failures %u), delivered to the room %u; dropped: no key %u, did not decrypt %u, repeats %u, other %u\n",
	       r->beaconsSeen, r->framesSent, r->txFailed, r->framesReceived, r->droppedNoKey, r->droppedDecrypt, r->droppedReplay, r->droppedOther);
	printf("board: forwarded frames %u, status replies %u, bad serial frames %u\n", r->esp.rxFrames, r->esp.statusFrames, r->esp.decoder.framesBad);
	udsJoinerClose(&joiner);
	return up ? 0 : 1;
}
