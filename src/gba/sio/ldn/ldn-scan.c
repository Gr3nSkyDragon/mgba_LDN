/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * ldn-scan: puts the Wi-Fi adapter behind a running ldnd into monitor mode, hops the LDN channels and prints every
 * LDN advertisement (a Switch hosting a local-wireless game) it hears, decoded with the keys of a prod.keys file.
 * Development tool for the Broadcast (LDN) backend (see ldn-monitor.h, which this drives, and ldn.h for the
 * decoding); not part of the adapter itself.
 *
 *   ldn-scan --keys <prod.keys> [--seconds N] [--channels 1,6,11] [--debug]
 *   ldn-scan --self-test   (offline: replays a captured beacon through the decoder, no ldnd/Switch needed)
 */
#include "ldn-monitor.h"
#include "ldn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

enum {
	MAX_SEEN = 16,
};

struct Seen {
	bool used;
	uint8_t mac[6];
	uint8_t ssid[16];
	uint16_t appDataSize;
	uint8_t appData[LDN_MAX_APP_DATA];
	uint8_t numParticipants;
};

struct Scan {
	struct LdnKeys keys;
	bool haveKeys;

	unsigned frames, management, actions, vendor, advertisements, decoded;
	struct Seen seen[MAX_SEEN];
};

static void _printHex(const uint8_t* data, size_t length) {
	for (size_t i = 0; i < length; ++i) {
		printf("%02X", data[i]);
	}
}

static void _report(struct Scan* scan, const uint8_t* mac, unsigned channel, const struct LdnAdvertisement* ad) {
	struct Seen* slot = NULL;
	for (int i = 0; i < MAX_SEEN; ++i) {
		if (scan->seen[i].used && !memcmp(scan->seen[i].mac, mac, 6) && !memcmp(scan->seen[i].ssid, ad->ssid, 16)) {
			slot = &scan->seen[i];
			break;
		}
		if (!slot && !scan->seen[i].used) {
			slot = &scan->seen[i];
		}
	}
	bool isNew = slot && !slot->used;
	bool changed = slot && slot->used && (slot->appDataSize != ad->appDataSize || memcmp(slot->appData, ad->appData, ad->appDataSize) ||
	                                      slot->numParticipants != ad->numParticipants);
	if (!slot || (!isNew && !changed)) {
		return;
	}
	slot->used = true;
	memcpy(slot->mac, mac, 6);
	memcpy(slot->ssid, ad->ssid, 16);
	slot->appDataSize = ad->appDataSize;
	memcpy(slot->appData, ad->appData, ad->appDataSize);
	slot->numParticipants = ad->numParticipants;

	printf("\n%s: LDN advertisement from %02x:%02x:%02x:%02x:%02x:%02x on channel %u\n", isNew ? "NEW" : "UPDATE", mac[0], mac[1], mac[2],
	       mac[3], mac[4], mac[5], channel);
	printf("  local communication id: %016llX  scene: %u  version: %u  format: %u\n", (unsigned long long) ad->localCommunicationId, ad->sceneId,
	       ad->version, ad->format);
	char wlanSsid[33];
	LdnAdvertisementWlanSsid(ad, wlanSsid);
	printf("  WLAN SSID: %s\n", wlanSsid);
	if (!ad->infoDecoded) {
		printf("  (encrypted part not decoded%s)\n", scan->haveKeys ? "" : ": no keys given");
		return;
	}
	printf("  protocol %d, security mode %u, accept policy %u, app version %u, channel %u\n", ad->protocol, ad->securityMode,
	       ad->stationAcceptPolicy, ad->appVersion, ad->advertisedChannel);
	printf("  participants: %u of %u\n", ad->numParticipants, ad->maxParticipants);
	for (int i = 0; i < LDN_MAX_PARTICIPANTS; ++i) {
		const struct LdnParticipant* participant = &ad->participants[i];
		if (participant->present) {
			printf("    [%d] \"%s\"  ip %u.%u.%u.%u  platform %u\n", i, participant->name, participant->ip[0], participant->ip[1],
			       participant->ip[2], participant->ip[3], participant->platform);
		}
	}
	printf("  application data (%u bytes): ", ad->appDataSize);
	_printHex(ad->appData, ad->appDataSize);
	printf("\n  as text: ");
	for (unsigned i = 0; i < ad->appDataSize; ++i) {
		putchar(ad->appData[i] >= 32 && ad->appData[i] < 127 ? ad->appData[i] : '.');
	}
	printf("\n");
	struct LdnRfuBeacon beacon;
	if (LdnDecodeRfuBeacon(ad->appData, ad->appDataSize, &beacon)) {
		printf("  RFU beacon: trainer id 0x%04X, name \"%s\", RFU session 0x%04X, trade species %u, partner info: ", beacon.trainerId,
		       beacon.name, beacon.rfuSessionId, beacon.tradeSpecies);
		_printHex(beacon.partnerInfo, sizeof(beacon.partnerInfo));
		printf("\n");
		uint32_t words[6];
		LdnBeaconToBroadcastWords(&beacon, 0x13820002, 0x04, words);
		printf("  synthesized broadcast (assuming English FireRed): %08X %08X %08X %08X %08X %08X\n", words[0], words[1], words[2],
		       words[3], words[4], words[5]);
	} else {
		printf("  RFU beacon: could not decode (application data too short, or not valid base85)\n");
	}
	fflush(stdout);
}

static void _onRawFrame(void* context, const uint8_t* data, size_t length) {
	struct Scan* scan = context;
	(void) data;
	++scan->frames;
	// A rough duplicate of ldn-monitor's own filtering, just for the running totals this tool prints; the real
	// filtering (and the callback below) is done once, in ldn-monitor.c.
	if (length < 8 || data[0] != 0) {
		return;
	}
	size_t radiotap = data[2] | (data[3] << 8);
	if (radiotap > length || length - radiotap < 24) {
		return;
	}
	const uint8_t* frame = data + radiotap;
	size_t frameLength = length - radiotap;
	if (((frame[0] >> 2) & 3) != 0) {
		return;
	}
	++scan->management;
	uint8_t subtype = frame[0] >> 4;
	if (subtype != 13 && subtype != 14) {
		return;
	}
	++scan->actions;
	size_t bodyOffset = 24 + ((frame[1] & 0x80) ? 4 : 0);
	if (frameLength > bodyOffset && frame[bodyOffset] == 0x7F) {
		++scan->vendor;
	}
}

static void _onAdvertisement(void* context, const uint8_t* mac, const uint8_t* body, size_t bodyLength, unsigned channel) {
	struct Scan* scan = context;
	struct LdnAdvertisement ad;
	if (!LdnDecodeAdvertisement(body, bodyLength, scan->haveKeys ? &scan->keys : NULL, &ad)) {
		return;
	}
	++scan->advertisements;
	if (ad.infoDecoded) {
		++scan->decoded;
	}
	_report(scan, mac, channel, &ad);
}

// Offline regression check: the application data of a real LDN advertisement captured from a Switch running
// FRLG (2026-09-21, host "GSD"), decoded with no ldnd or Switch needed. Run with --self-test.
static int _selfTest(void) {
	static const uint8_t appData[] = {
		0x00, 0x5C, 0x16, 0x00, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x03, 0x01, 0x47, 0x53, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x2E, 0x47, 0x77, 0x2D, 0x66, 0x37, 0x28, 0x2C, 0x23, 0x23, 0x65, 0x30, 0x42, 0x46, 0x43, 0x23,
		0x23, 0x23, 0x23, 0x23, 0x3B, 0x60, 0x23, 0x23, 0x23, 0x23, 0x23, 0x23, 0x23, 0x23,
	};
	struct LdnRfuBeacon beacon;
	if (!LdnDecodeRfuBeacon(appData, sizeof(appData), &beacon)) {
		printf("SELF-TEST FAILED: could not decode the sample beacon\n");
		return 1;
	}
	printf("decoded: trainer id 0x%04X, name \"%s\", RFU session 0x%04X, trade species %u\n", beacon.trainerId, beacon.name,
	       beacon.rfuSessionId, beacon.tradeSpecies);
	bool ok = beacon.trainerId == 0x1D5E && !strcmp(beacon.name, "GSD") && beacon.rfuSessionId == 0x64DC && beacon.tradeSpecies == 0;
	if (!ok) {
		printf("SELF-TEST FAILED: decoded fields do not match the known-good capture\n");
		return 1;
	}
	uint32_t words[6];
	LdnBeaconToBroadcastWords(&beacon, 0x13820002, 0x04, words);
	printf("synthesized broadcast: %08X %08X %08X %08X %08X %08X\n", words[0], words[1], words[2], words[3], words[4], words[5]);
	// word3's top byte is the checksum librfu's search-parent list validates (see LdnBeaconToBroadcastWords);
	// 0xA8 is what it works out to for this beacon (verified by hand against a real capture's own checksum, which
	// this same formula reproduces exactly: 0x7C for trainer id 0x6717 name "PHOENIX").
	ok = words[0] == 0x13820002 && words[1] == 0x1D5E && words[2] == 0 && words[3] == 0xA8000004;
	if (!ok) {
		printf("SELF-TEST FAILED: synthesized broadcast words do not match\n");
		return 1;
	}
	printf("SELF-TEST PASSED\n");
	return 0;
}

int main(int argc, char** argv) {
	const char* keysPath = NULL;
	unsigned seconds = 30;
	unsigned channels[8] = {1, 6, 11};
	unsigned channelCount = 3;
	static struct Scan scan;

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
		} else if (!strcmp(argv[i], "--self-test")) {
			return _selfTest();
		}
	}
	if (keysPath) {
		scan.haveKeys = LdnKeysLoad(keysPath, &scan.keys);
		printf("Keys: %s\n", scan.haveKeys ? "loaded" : "could not read the four LDN keys from that file");
	} else {
		printf("Keys: none given (--keys <prod.keys>); advertisements can only be listed, not decoded.\n");
	}

	struct LdnMonitor* monitor = LdnMonitorOpen(NULL, channels[0], _onRawFrame, _onAdvertisement, &scan);
	if (!monitor) {
		fprintf(stderr, "%s\n", LdnMonitorLastError());
		return 1;
	}
	printf("Monitor interface open. Listening for %u seconds on channel(s)", seconds);
	for (unsigned i = 0; i < channelCount; ++i) {
		printf(" %u", channels[i]);
	}
	printf(" ...\n");
	fflush(stdout);

#ifdef _WIN32
	DWORD end = GetTickCount() + seconds * 1000;
	unsigned hop = 1; // channel[0] is already set by LdnMonitorOpen
	while ((int32_t) (end - GetTickCount()) > 0) {
		unsigned channel = channels[hop++ % channelCount];
		if (LdnMonitorSetChannel(monitor, channel)) {
			fprintf(stderr, "%s\n", LdnMonitorLastError());
		}
		Sleep(400);
	}
#endif
	printf("\nFrames captured: %u, management: %u, action: %u, vendor-specific: %u, LDN advertisements: %u (decoded: %u)\n", scan.frames,
	       scan.management, scan.actions, scan.vendor, scan.advertisements, scan.decoded);

	LdnMonitorClose(monitor);
	return 0;
}
