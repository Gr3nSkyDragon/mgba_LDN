/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * ldnd-probe: connects to a running ldnd, resolves the nl80211 family and lists the Wi-Fi device(s) it exposes:
 * phy name, supported interface types (is monitor mode there?), the 2.4 GHz channels, and any interfaces.
 * Step 1 of the Broadcast (LDN) backend: proves the pipe, socket and netlink layers from C.
 *
 *   ldnd-probe [pipe path]
 */
#include "netlink.h"

#include <stdio.h>
#include <string.h>

enum {
	NL80211_CMD_GET_WIPHY = 1,
	NL80211_CMD_GET_INTERFACE = 5,

	NL80211_ATTR_WIPHY = 1,
	NL80211_ATTR_WIPHY_NAME = 2,
	NL80211_ATTR_IFINDEX = 3,
	NL80211_ATTR_IFNAME = 4,
	NL80211_ATTR_IFTYPE = 5,
	NL80211_ATTR_MAC = 6,
	NL80211_ATTR_WIPHY_BANDS = 22,
	NL80211_ATTR_SUPPORTED_IFTYPES = 32,

	NL80211_BAND_ATTR_FREQS = 1,
	NL80211_FREQUENCY_ATTR_FREQ = 1,
	NL80211_FREQUENCY_ATTR_DISABLED = 2,
};

static const char* _ifType(uint32_t type) {
	static const char* names[] = {"unspecified", "adhoc", "station", "ap", "ap_vlan", "wds", "monitor", "mesh point",
	                              "p2p client", "p2p go", "p2p device", "ocb", "nan"};
	return type < sizeof(names) / sizeof(names[0]) ? names[type] : "?";
}

static void _printFrequencies(const uint8_t* bands, size_t length) {
	unsigned band24 = 0, band5 = 0, disabled = 0;
	uint32_t channels24[32];
	const uint8_t* cursor = bands;
	size_t remaining = length;
	uint16_t type;
	const uint8_t* band;
	size_t bandLength;
	while (NlNextAttr(&cursor, &remaining, &type, &band, &bandLength)) {
		const uint8_t* bandCursor = band;
		size_t bandRemaining = bandLength;
		uint16_t bandType;
		const uint8_t* value;
		size_t valueLength;
		while (NlNextAttr(&bandCursor, &bandRemaining, &bandType, &value, &valueLength)) {
			if ((bandType & NL_ATTR_TYPE_MASK) != NL80211_BAND_ATTR_FREQS) {
				continue;
			}
			const uint8_t* freqCursor = value;
			size_t freqRemaining = valueLength;
			uint16_t freqType;
			const uint8_t* freq;
			size_t freqLength;
			while (NlNextAttr(&freqCursor, &freqRemaining, &freqType, &freq, &freqLength)) {
				uint32_t mhz = 0;
				bool off = false;
				const uint8_t* attrCursor = freq;
				size_t attrRemaining = freqLength;
				uint16_t attrType;
				const uint8_t* attrValue;
				size_t attrLength;
				while (NlNextAttr(&attrCursor, &attrRemaining, &attrType, &attrValue, &attrLength)) {
					if ((attrType & NL_ATTR_TYPE_MASK) == NL80211_FREQUENCY_ATTR_FREQ && attrLength >= 4) {
						mhz = NlReadU32(attrValue);
					} else if ((attrType & NL_ATTR_TYPE_MASK) == NL80211_FREQUENCY_ATTR_DISABLED) {
						off = true;
					}
				}
				if (off) {
					++disabled;
				} else if (mhz >= 2400 && mhz < 2500) {
					if (band24 < 32) {
						channels24[band24] = mhz;
					}
					++band24;
				} else if (mhz >= 5000) {
					++band5;
				}
			}
		}
	}
	printf("    2.4 GHz channels enabled: %u", band24);
	for (unsigned i = 0; i < band24 && i < 32; ++i) {
		printf("%s%u", i ? ", " : " (MHz: ", channels24[i]);
	}
	printf("%s\n", band24 ? ")" : "");
	printf("    5 GHz channels enabled: %u, disabled by regulatory: %u\n", band5, disabled);
}

static void _wiphyReply(void* context, const struct NlHeader* header, const uint8_t* payload, size_t payloadLength) {
	(void) context;
	(void) header;
	if (payloadLength < GENL_HEADER_LENGTH) {
		return;
	}
	const uint8_t* cursor = payload + GENL_HEADER_LENGTH;
	size_t remaining = payloadLength - GENL_HEADER_LENGTH;
	uint16_t type;
	const uint8_t* value;
	size_t valueLength;
	while (NlNextAttr(&cursor, &remaining, &type, &value, &valueLength)) {
		switch (type & NL_ATTR_TYPE_MASK) {
		case NL80211_ATTR_WIPHY:
			if (valueLength >= 4) {
				printf("  phy index %u\n", NlReadU32(value));
			}
			break;
		case NL80211_ATTR_WIPHY_NAME:
			printf("    name: %.*s\n", (int) valueLength, (const char*) value);
			break;
		case NL80211_ATTR_SUPPORTED_IFTYPES: {
			printf("    interface types:");
			const uint8_t* inner = value;
			size_t innerRemaining = valueLength;
			uint16_t innerType;
			const uint8_t* innerValue;
			size_t innerLength;
			while (NlNextAttr(&inner, &innerRemaining, &innerType, &innerValue, &innerLength)) {
				printf(" %s", _ifType(innerType & NL_ATTR_TYPE_MASK));
			}
			printf("\n");
			break;
		}
		case NL80211_ATTR_WIPHY_BANDS:
			_printFrequencies(value, valueLength);
			break;
		}
	}
}

static void _interfaceReply(void* context, const struct NlHeader* header, const uint8_t* payload, size_t payloadLength) {
	(void) context;
	(void) header;
	if (payloadLength < GENL_HEADER_LENGTH) {
		return;
	}
	const uint8_t* cursor = payload + GENL_HEADER_LENGTH;
	size_t remaining = payloadLength - GENL_HEADER_LENGTH;
	uint16_t type;
	const uint8_t* value;
	size_t valueLength;
	printf("  interface:");
	while (NlNextAttr(&cursor, &remaining, &type, &value, &valueLength)) {
		switch (type & NL_ATTR_TYPE_MASK) {
		case NL80211_ATTR_IFNAME:
			printf(" %.*s", (int) valueLength, (const char*) value);
			break;
		case NL80211_ATTR_IFINDEX:
			if (valueLength >= 4) {
				printf(" (index %u)", NlReadU32(value));
			}
			break;
		case NL80211_ATTR_IFTYPE:
			if (valueLength >= 4) {
				printf(" type=%s", _ifType(NlReadU32(value)));
			}
			break;
		case NL80211_ATTR_WIPHY:
			if (valueLength >= 4) {
				printf(" phy=%u", NlReadU32(value));
			}
			break;
		case NL80211_ATTR_MAC:
			if (valueLength >= 6) {
				printf(" mac=%02x:%02x:%02x:%02x:%02x:%02x", value[0], value[1], value[2], value[3], value[4], value[5]);
			}
			break;
		}
	}
	printf("\n");
}

static bool _route(void* context, uint32_t socketId, const uint8_t* data, size_t length) {
	return NlInput(context, socketId, data, length);
}

int main(int argc, char** argv) {
	const char* pipe = argc > 1 ? argv[1] : NULL;

	struct LdndConnection* conn = LdndOpen(pipe);
	if (!conn) {
		fprintf(stderr, "Could not open the ldnd pipe (%s). Is ldnd running?\n", pipe ? pipe : "\\\\.\\pipe\\ldnd");
		return 1;
	}
	printf("Connected to ldnd.\n");

	struct NlClient* genl = NlOpen(conn, NL_PROTOCOL_GENERIC);
	if (genl) {
		LdndAddDataCallback(conn, _route, genl);
	}
	if (!genl) {
		fprintf(stderr, "Could not open a generic netlink socket through ldnd.\n");
		LdndClose(conn);
		return 1;
	}
	printf("Generic netlink socket open.\n");

	uint16_t nl80211 = 0;
	int error = GenlResolveFamily(genl, "nl80211", &nl80211);
	if (error) {
		fprintf(stderr, "Could not resolve the nl80211 family (error %d).\n", error);
		NlClose(genl);
		LdndClose(conn);
		return 1;
	}
	printf("nl80211 family id: %u\n", nl80211);

	printf("Wi-Fi devices:\n");
	error = GenlRequest(genl, nl80211, NL_F_DUMP, NL80211_CMD_GET_WIPHY, 1, NULL, 0, _wiphyReply, NULL, 5000);
	if (error) {
		fprintf(stderr, "GET_WIPHY failed (error %d).\n", error);
	}

	printf("Interfaces:\n");
	error = GenlRequest(genl, nl80211, NL_F_DUMP, NL80211_CMD_GET_INTERFACE, 1, NULL, 0, _interfaceReply, NULL, 5000);
	if (error) {
		fprintf(stderr, "GET_INTERFACE failed (error %d).\n", error);
	}

	NlClose(genl);
	LdndClose(conn);
	return 0;
}
