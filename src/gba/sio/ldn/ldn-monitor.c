/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ldn-monitor.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	NL80211_CMD_GET_INTERFACE = 5,
	NL80211_CMD_NEW_INTERFACE = 7,
	NL80211_CMD_DEL_INTERFACE = 8,
	NL80211_CMD_SET_CHANNEL = 65,

	NL80211_ATTR_WIPHY = 1,
	NL80211_ATTR_IFINDEX = 3,
	NL80211_ATTR_IFNAME = 4,
	NL80211_ATTR_IFTYPE = 5,
	NL80211_ATTR_MNTR_FLAGS = 23,
	NL80211_ATTR_WIPHY_FREQ = 38,

	NL80211_IFTYPE_MONITOR = 6,
	NL80211_MNTR_FLAG_OTHER_BSS = 4,

	RTM_NEWLINK = 16,
	IFF_UP = 1,

	AF_PACKET_LINUX = 17,
	SOCK_RAW_LINUX = 3,
	ETH_P_ALL = 3,
};

static const char kMonitorName[] = "ldnmon0";
static char sLastError[160] = "";

const char* LdnMonitorLastError(void) {
	return sLastError;
}

static void _fail(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vsnprintf(sLastError, sizeof(sLastError), format, args);
	va_end(args);
}

struct LdnMonitor {
	struct LdndConnection* conn;
	struct NlClient* genl;
	struct NlClient* route;
	uint16_t family;
	uint32_t ifIndex;
	bool weCreatedTheInterface;
	uint32_t packetSocket;

	LdnMonitorRawCallback rawCallback;
	LdnMonitorAdvertisementCallback advertisementCallback;
	void* context;
	unsigned channel;
};

static uint32_t _frequency(unsigned channel) {
	if (channel == 14) {
		return 2484;
	}
	if (channel >= 1 && channel <= 13) {
		return 2407 + channel * 5;
	}
	return 5000 + channel * 5;
}

// One captured frame: radiotap header, then the 802.11 frame. Filters it down to an LDN advertisement (a vendor-
// specific action frame whose body starts with Nintendo's OUI marker, 0x7F) and reports that separately.
static void _onFrame(struct LdnMonitor* monitor, const uint8_t* data, size_t length) {
	if (monitor->rawCallback) {
		monitor->rawCallback(monitor->context, data, length);
	}
	if (!monitor->advertisementCallback || length < 8 || data[0] != 0) {
		return;
	}
	size_t radiotap = data[2] | (data[3] << 8);
	if (radiotap > length || length - radiotap < 24) {
		return;
	}
	const uint8_t* frame = data + radiotap;
	size_t frameLength = length - radiotap;
	uint8_t type = (frame[0] >> 2) & 3;
	uint8_t subtype = frame[0] >> 4;
	if (type != 0 || (subtype != 13 && subtype != 14)) { // management, action / action-no-ack
		return;
	}
	size_t bodyOffset = 24 + ((frame[1] & 0x80) ? 4 : 0); // +4 when the QoS/HTC "order" bit is set
	if (frameLength <= bodyOffset || frame[bodyOffset] != 0x7F) {
		return;
	}
	monitor->advertisementCallback(monitor->context, frame + 10, frame + bodyOffset, frameLength - bodyOffset, monitor->channel);
}

static bool _route(void* context, uint32_t socketId, const uint8_t* data, size_t length) {
	struct LdnMonitor* monitor = context;
	if (monitor->genl && NlInput(monitor->genl, socketId, data, length)) {
		return true;
	}
	if (monitor->route && NlInput(monitor->route, socketId, data, length)) {
		return true;
	}
	if (socketId == monitor->packetSocket) {
		_onFrame(monitor, data, length);
		return true;
	}
	return false;
}

struct FindResult {
	const char* name;
	bool found;
	uint32_t index;
	uint32_t type;
};

static void _findReply(void* context, const struct NlHeader* header, const uint8_t* payload, size_t payloadLength) {
	struct FindResult* result = context;
	(void) header;
	if (payloadLength < GENL_HEADER_LENGTH) {
		return;
	}
	const uint8_t* cursor = payload + GENL_HEADER_LENGTH;
	size_t remaining = payloadLength - GENL_HEADER_LENGTH;
	uint16_t type;
	const uint8_t* value;
	size_t valueLength;
	bool match = false;
	uint32_t index = 0, iftype = 0;
	while (NlNextAttr(&cursor, &remaining, &type, &value, &valueLength)) {
		switch (type & NL_ATTR_TYPE_MASK) {
		case NL80211_ATTR_IFNAME:
			match = strncmp((const char*) value, result->name, valueLength) == 0 && strlen(result->name) + 1 >= valueLength;
			break;
		case NL80211_ATTR_IFINDEX:
			index = valueLength >= 4 ? NlReadU32(value) : 0;
			break;
		case NL80211_ATTR_IFTYPE:
			iftype = valueLength >= 4 ? NlReadU32(value) : 0;
			break;
		}
	}
	if (match) {
		result->found = true;
		result->index = index;
		result->type = iftype;
	}
}

static bool _findInterface(struct LdnMonitor* monitor, const char* name, struct FindResult* result) {
	memset(result, 0, sizeof(*result));
	result->name = name;
	return GenlRequest(monitor->genl, monitor->family, NL_F_DUMP, NL80211_CMD_GET_INTERFACE, 1, NULL, 0, _findReply, result, 5000) == 0 &&
	       result->found;
}

static int _setLink(struct LdnMonitor* monitor, bool up) {
	uint8_t message[16] = {0}; // struct ifinfomsg
	message[4] = monitor->ifIndex;
	message[5] = monitor->ifIndex >> 8;
	message[6] = monitor->ifIndex >> 16;
	message[7] = monitor->ifIndex >> 24;
	uint32_t flags = up ? IFF_UP : 0, change = IFF_UP;
	for (int i = 0; i < 4; ++i) {
		message[8 + i] = flags >> (8 * i);
		message[12 + i] = change >> (8 * i);
	}
	return NlRequest(monitor->route, RTM_NEWLINK, NL_F_ACK, message, sizeof(message), NULL, NULL, 5000);
}

struct LdndConnection* LdnMonitorConnection(struct LdnMonitor* monitor) {
	return monitor->conn;
}

int LdnMonitorSetChannel(struct LdnMonitor* monitor, unsigned channel) {
	struct NlMessage attrs;
	attrs.length = 0;
	NlAddAttrU32(&attrs, NL80211_ATTR_IFINDEX, monitor->ifIndex);
	NlAddAttrU32(&attrs, NL80211_ATTR_WIPHY_FREQ, _frequency(channel));
	int error = GenlRequest(monitor->genl, monitor->family, NL_F_ACK, NL80211_CMD_SET_CHANNEL, 1, attrs.data, attrs.length, NULL, NULL, 5000);
	if (error) {
		_fail("could not switch to channel %u (error %d)", channel, error);
	} else {
		monitor->channel = channel;
	}
	return error;
}

static int _createInterface(struct LdnMonitor* monitor) {
	struct NlMessage attrs;
	attrs.length = 0;
	NlAddAttrU32(&attrs, NL80211_ATTR_WIPHY, 0);
	NlAddAttrString(&attrs, NL80211_ATTR_IFNAME, kMonitorName);
	NlAddAttrU32(&attrs, NL80211_ATTR_IFTYPE, NL80211_IFTYPE_MONITOR);
	// Nested flags: one empty attribute whose type is the flag (other-BSS: see frames of networks we are not part of)
	uint8_t flag[4] = {4, 0, NL80211_MNTR_FLAG_OTHER_BSS, 0};
	NlAddAttr(&attrs, NL80211_ATTR_MNTR_FLAGS | NL_ATTR_NESTED, flag, sizeof(flag));
	return GenlRequest(monitor->genl, monitor->family, NL_F_ACK, NL80211_CMD_NEW_INTERFACE, 1, attrs.data, attrs.length, NULL, NULL, 5000);
}

static int _openPacketSocket(struct LdnMonitor* monitor) {
	uint32_t socketId;
	int error = LdndSocket(monitor->conn, AF_PACKET_LINUX, SOCK_RAW_LINUX, ((ETH_P_ALL & 0xFF) << 8) | (ETH_P_ALL >> 8), &socketId);
	if (error) {
		return error;
	}
	uint8_t address[20] = {0}; // struct sockaddr_ll
	address[0] = AF_PACKET_LINUX;
	address[2] = ETH_P_ALL >> 8;
	address[3] = ETH_P_ALL & 0xFF;
	address[4] = monitor->ifIndex;
	address[5] = monitor->ifIndex >> 8;
	address[6] = monitor->ifIndex >> 16;
	address[7] = monitor->ifIndex >> 24;
	error = LdndBind(monitor->conn, socketId, address, sizeof(address));
	if (error) {
		return error;
	}
	monitor->packetSocket = socketId;
	return LdndStart(monitor->conn, socketId);
}

struct LdnMonitor* LdnMonitorOpen(const char* pipePath, unsigned firstChannel, LdnMonitorRawCallback rawCallback,
                                   LdnMonitorAdvertisementCallback advertisementCallback, void* context) {
	struct LdnMonitor* monitor = calloc(1, sizeof(*monitor));
	if (!monitor) {
		_fail("out of memory");
		return NULL;
	}
	monitor->rawCallback = rawCallback;
	monitor->advertisementCallback = advertisementCallback;
	monitor->context = context;

	monitor->conn = LdndOpen(pipePath);
	if (!monitor->conn) {
		_fail("could not open the ldnd pipe; is ldnd running?");
		free(monitor);
		return NULL;
	}
	LdndAddDataCallback(monitor->conn, _route, monitor);

	monitor->genl = NlOpen(monitor->conn, NL_PROTOCOL_GENERIC);
	monitor->route = NlOpen(monitor->conn, NL_PROTOCOL_ROUTE);
	if (!monitor->genl || !monitor->route) {
		_fail("could not open netlink sockets through ldnd");
		goto fail;
	}
	if (GenlResolveFamily(monitor->genl, "nl80211", &monitor->family)) {
		_fail("could not resolve the nl80211 netlink family");
		goto fail;
	}

	struct FindResult found;
	if (_findInterface(monitor, kMonitorName, &found) && found.type != NL80211_IFTYPE_MONITOR) {
		// Something else is using the name (unlikely, but leftover from a very different tool); replace it.
		GenlRequest(monitor->genl, monitor->family, NL_F_ACK, NL80211_CMD_DEL_INTERFACE, 1, NULL, 0, NULL, NULL, 5000);
		found.found = false;
	}
	if (found.found) {
		monitor->ifIndex = found.index;
	} else {
		int error = _createInterface(monitor);
		if (error) {
			_fail("could not create the monitor interface (error %d)", error);
			goto fail;
		}
		monitor->weCreatedTheInterface = true;
		if (!_findInterface(monitor, kMonitorName, &found)) {
			_fail("the monitor interface was created but cannot be found");
			goto fail;
		}
		monitor->ifIndex = found.index;
	}

	int error = _setLink(monitor, true);
	if (error) {
		_fail("could not bring the monitor interface up (error %d)", error);
		goto fail;
	}
	error = LdnMonitorSetChannel(monitor, firstChannel);
	if (error) {
		goto fail;
	}
	error = _openPacketSocket(monitor);
	if (error) {
		_fail("could not open the capture socket (error %d)", error);
		goto fail;
	}
	return monitor;

fail:
	LdnMonitorClose(monitor);
	return NULL;
}

void LdnMonitorClose(struct LdnMonitor* monitor) {
	if (!monitor) {
		return;
	}
	if (monitor->packetSocket) {
		LdndCloseSocket(monitor->conn, monitor->packetSocket);
	}
	if (monitor->genl && monitor->weCreatedTheInterface && monitor->ifIndex) {
		struct NlMessage attrs;
		attrs.length = 0;
		NlAddAttrU32(&attrs, NL80211_ATTR_IFINDEX, monitor->ifIndex);
		GenlRequest(monitor->genl, monitor->family, NL_F_ACK, NL80211_CMD_DEL_INTERFACE, 1, attrs.data, attrs.length, NULL, NULL, 5000);
	}
	NlClose(monitor->route);
	NlClose(monitor->genl);
	LdndRemoveDataCallback(monitor->conn, _route, monitor);
	LdndClose(monitor->conn);
	free(monitor);
}
