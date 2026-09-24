/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ldn-station.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

enum {
	NL80211_CMD_GET_INTERFACE = 5,
	NL80211_CMD_CONNECT = 46,
	NL80211_CMD_NEW_KEY = 11,
	NL80211_CMD_SET_STATION = 18,
	// Verified against the upstream kernel's include/uapi/linux/nl80211.h (v6.6 tag) by counting the
	// nl80211_commands enum from NL80211_CMD_UNSPEC=0 - this and NL80211_ATTR_FRAME below are new for the
	// authentication handshake (ldn-auth.c) and had not been cross-checked before.
	NL80211_CMD_CONTROL_PORT_FRAME = 129,

	NL80211_ATTR_IFINDEX = 3,
	NL80211_ATTR_MAC = 6,
	NL80211_ATTR_IFNAME = 4,
	NL80211_ATTR_IFTYPE = 5,
	NL80211_ATTR_WIPHY_FREQ = 38,
	NL80211_ATTR_IE = 42,
	NL80211_ATTR_FRAME = 51,
	NL80211_ATTR_SSID = 52,
	NL80211_ATTR_AUTH_TYPE = 53,
	NL80211_ATTR_STA_FLAGS2 = 67,
	NL80211_ATTR_CONTROL_PORT = 68,
	NL80211_ATTR_PRIVACY = 70,
	NL80211_ATTR_STATUS_CODE = 72,
	NL80211_ATTR_CIPHER_SUITES_PAIRWISE = 73,
	NL80211_ATTR_CIPHER_SUITE_GROUP = 74,
	NL80211_ATTR_AKM_SUITES = 76,
	NL80211_ATTR_KEY = 80,
	NL80211_ATTR_CONTROL_PORT_ETHERTYPE = 102,
	// Was wrongly 108 (actually NL80211_ATTR_OFFCHANNEL_TX_OK) until this was cross-checked against the real
	// kernel header - harmless for CMD_CONNECT itself (association succeeded regardless, since this flag only
	// affects where the kernel routes control-port frames afterward) but would have broken CMD_CONTROL_PORT_FRAME
	// delivery outright, silently. Verified the same way as NL80211_CMD_CONTROL_PORT_FRAME above: 264.
	NL80211_ATTR_CONTROL_PORT_OVER_NL80211 = 264,
	NL80211_ATTR_SOCKET_OWNER = 204,

	NL80211_KEY_DATA = 1,
	NL80211_KEY_IDX = 2,
	NL80211_KEY_CIPHER = 3,

	NL80211_IFTYPE_STATION = 2,

	WLAN_CIPHER_SUITE_CCMP = 0x000FAC04,
	WLAN_AKM_SUITE_PSK = 0x000FAC02,
	WLAN_EID_RSN = 48,
	WLAN_AUTHTYPE_OPEN_SYSTEM = 0,
	// Nintendo's own control-port ethertype for LDN (0x88B7, "ETH_P_OUI" in the LDN-0.0.3 reference client's
	// wlan.py) - NOT the standard EAPOL ethertype (0x888E) this was wrongly set to before the authentication
	// handshake (ldn-auth.c) was implemented and this got cross-checked. Harmless for CMD_CONNECT itself (WPA2
	// association does not use a real 4-way handshake here - see LdnStationConnect), but the control port frames
	// LDN's own authentication step sends/receives are tagged with this ethertype, so CONNECT and
	// CMD_CONTROL_PORT_FRAME must agree on it.
	ETH_P_LDN = 0x88B7,

	NL80211_STA_FLAG_AUTHORIZED = 1u << 1,

	IFF_UP = 1,
};

static char sLastError[160] = "";

const char* LdnStationLastError(void) {
	return sLastError;
}

static void _fail(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vsnprintf(sLastError, sizeof(sLastError), format, args);
	va_end(args);
}

struct LdnStation {
	struct LdndConnection* conn; // not owned - see LdnStationOpen
	struct NlClient* genl;
	struct NlClient* route;
	uint16_t family;
};

// The connection may be shared with another owner (e.g. ldn-monitor.c, while a scan is still in progress) that
// has its own callback already registered - ldnd's reader thread calls every registered callback on a DATA frame
// until one claims it, so each owner only needs to recognise its own sockets.
static bool _route(void* context, uint32_t socketId, const uint8_t* data, size_t length) {
	struct LdnStation* station = context;
	if (station->genl && NlInput(station->genl, socketId, data, length)) {
		return true;
	}
	if (station->route && NlInput(station->route, socketId, data, length)) {
		return true;
	}
	return false;
}

struct LdnStation* LdnStationOpen(struct LdndConnection* conn) {
	struct LdnStation* station = calloc(1, sizeof(*station));
	if (!station) {
		_fail("out of memory");
		return NULL;
	}
	station->conn = conn;
	if (!LdndAddDataCallback(conn, _route, station)) {
		_fail("too many listeners already registered on this ldnd connection");
		free(station);
		return NULL;
	}
	station->genl = NlOpen(station->conn, NL_PROTOCOL_GENERIC);
	station->route = NlOpen(station->conn, NL_PROTOCOL_ROUTE);
	if (!station->genl || !station->route) {
		_fail("could not open netlink sockets through ldnd");
		goto fail;
	}
	if (GenlResolveFamily(station->genl, "nl80211", &station->family)) {
		_fail("could not resolve the nl80211 netlink family");
		goto fail;
	}
	uint32_t mlmeGroup;
	if (GenlResolveMulticastGroup(station->genl, "nl80211", "mlme", &mlmeGroup)) {
		_fail("could not resolve nl80211's \"mlme\" multicast group");
		goto fail;
	}
	if (NlJoinMulticastGroup(station->conn, station->genl, mlmeGroup)) {
		_fail("could not join nl80211's \"mlme\" multicast group");
		goto fail;
	}
	return station;

fail:
	LdnStationClose(station);
	return NULL;
}

void LdnStationClose(struct LdnStation* station) {
	if (!station) {
		return;
	}
	NlClose(station->route);
	NlClose(station->genl);
	LdndRemoveDataCallback(station->conn, _route, station);
	// station->conn is caller-owned; not closed here.
	free(station);
}

struct FindStationResult {
	bool found;
	uint32_t index;
	uint8_t mac[6];
};

static void _findStationReply(void* context, const struct NlHeader* header, const uint8_t* payload, size_t payloadLength) {
	struct FindStationResult* result = context;
	(void) header;
	if (payloadLength < GENL_HEADER_LENGTH) {
		return;
	}
	const uint8_t* cursor = payload + GENL_HEADER_LENGTH;
	size_t remaining = payloadLength - GENL_HEADER_LENGTH;
	uint16_t type;
	const uint8_t* value;
	size_t valueLength;
	uint32_t index = 0, iftype = 0;
	bool haveIndex = false;
	uint8_t mac[6] = {0};
	while (NlNextAttr(&cursor, &remaining, &type, &value, &valueLength)) {
		if ((type & NL_ATTR_TYPE_MASK) == NL80211_ATTR_IFINDEX && valueLength >= 4) {
			index = NlReadU32(value);
			haveIndex = true;
		} else if ((type & NL_ATTR_TYPE_MASK) == NL80211_ATTR_IFTYPE && valueLength >= 4) {
			iftype = NlReadU32(value);
		} else if ((type & NL_ATTR_TYPE_MASK) == NL80211_ATTR_MAC && valueLength >= 6) {
			memcpy(mac, value, 6);
		}
	}
	if (!result->found && haveIndex && iftype == NL80211_IFTYPE_STATION) {
		result->found = true;
		result->index = index;
		memcpy(result->mac, mac, 6);
	}
}

int LdnStationFindInterface(struct LdnStation* station, uint32_t* ifIndex, uint8_t mac[6]) {
	struct FindStationResult result = {0};
	int error = GenlRequest(station->genl, station->family, NL_F_DUMP, NL80211_CMD_GET_INTERFACE, 1, NULL, 0, _findStationReply, &result, 5000);
	if (error) {
		_fail("could not list interfaces (error %d)", error);
		return error;
	}
	if (!result.found) {
		_fail("no station-mode interface found");
		return LDND_ERR_ARGS;
	}
	*ifIndex = result.index;
	if (mac) {
		memcpy(mac, result.mac, 6);
	}
	return 0;
}

static int _setLink(struct LdnStation* station, uint32_t ifIndex, bool up) {
	uint8_t message[16] = {0}; // struct ifinfomsg
	message[4] = (uint8_t) ifIndex;
	message[5] = (uint8_t) (ifIndex >> 8);
	message[6] = (uint8_t) (ifIndex >> 16);
	message[7] = (uint8_t) (ifIndex >> 24);
	uint32_t flags = up ? IFF_UP : 0, change = IFF_UP;
	for (int i = 0; i < 4; ++i) {
		message[8 + i] = (uint8_t) (flags >> (8 * i));
		message[12 + i] = (uint8_t) (change >> (8 * i));
	}
	enum { RTM_NEWLINK = 16 };
	return NlRequest(station->route, RTM_NEWLINK, NL_F_ACK, message, sizeof(message), NULL, NULL, 5000);
}

// Standard 802.11i RSN information element for WPA2-PSK/CCMP: version 1, group cipher CCMP, one pairwise cipher
// (CCMP), one AKM (PSK), RSN capabilities 0x000C (pre-auth/no-pairwise off, replay counter = 4).
static size_t _buildRsnIe(uint8_t out[22]) {
	static const uint8_t body[] = {
		0x01, 0x00,             // version 1
		0x00, 0x0F, 0xAC, 0x04, // group cipher: CCMP
		0x01, 0x00,             // pairwise cipher count: 1
		0x00, 0x0F, 0xAC, 0x04, // pairwise cipher: CCMP
		0x01, 0x00,             // AKM count: 1
		0x00, 0x0F, 0xAC, 0x02, // AKM: PSK
		0x0C, 0x00,             // RSN capabilities
	};
	out[0] = WLAN_EID_RSN;
	out[1] = (uint8_t) sizeof(body);
	memcpy(&out[2], body, sizeof(body));
	return sizeof(body) + 2;
}

static uint32_t _channelFrequency(unsigned channel) {
	switch (channel) {
	case 1: return 2412;
	case 6: return 2437;
	case 11: return 2462;
	default: return 2407 + channel * 5;
	}
}

struct ConnectEventResult {
	bool matched;
	bool haveStatus;
	uint16_t status;
	uint8_t mac[6];
	bool haveMac;
};

static void _onConnectEvent(void* context, const struct NlHeader* header, const uint8_t* payload, size_t payloadLength) {
	struct ConnectEventResult* result = context;
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
		case NL80211_ATTR_STATUS_CODE:
			if (valueLength >= 2) {
				result->status = (uint16_t) (value[0] | (value[1] << 8));
				result->haveStatus = true;
			}
			break;
		case NL80211_ATTR_MAC:
			if (valueLength >= 6) {
				memcpy(result->mac, value, 6);
				result->haveMac = true;
			}
			break;
		}
	}
	result->matched = true;
}

static int _installKey(struct LdnStation* station, uint32_t ifIndex, const uint8_t* mac, uint8_t index, const uint8_t key[16]) {
	struct NlMessage keyAttrs;
	keyAttrs.length = 0;
	uint8_t idx = index;
	NlAddAttr(&keyAttrs, NL80211_KEY_IDX, &idx, 1);
	NlAddAttr(&keyAttrs, NL80211_KEY_DATA, key, 16);
	NlAddAttrU32(&keyAttrs, NL80211_KEY_CIPHER, WLAN_CIPHER_SUITE_CCMP);

	struct NlMessage attrs;
	attrs.length = 0;
	NlAddAttrU32(&attrs, NL80211_ATTR_IFINDEX, ifIndex);
	if (mac) {
		NlAddAttr(&attrs, NL80211_ATTR_MAC, mac, 6);
	}
	NlAddAttr(&attrs, NL80211_ATTR_KEY | NL_ATTR_NESTED, keyAttrs.data, keyAttrs.length);
	return GenlRequest(station->genl, station->family, NL_F_ACK, NL80211_CMD_NEW_KEY, 1, attrs.data, attrs.length, NULL, NULL, 5000);
}

static int _authorize(struct LdnStation* station, uint32_t ifIndex, const uint8_t mac[6]) {
	uint8_t flags[8] = {0};
	flags[0] = NL80211_STA_FLAG_AUTHORIZED; // mask
	flags[4] = NL80211_STA_FLAG_AUTHORIZED; // set
	struct NlMessage attrs;
	attrs.length = 0;
	NlAddAttrU32(&attrs, NL80211_ATTR_IFINDEX, ifIndex);
	NlAddAttr(&attrs, NL80211_ATTR_MAC, mac, 6);
	NlAddAttr(&attrs, NL80211_ATTR_STA_FLAGS2, flags, sizeof(flags));
	return GenlRequest(station->genl, station->family, NL_F_ACK, NL80211_CMD_SET_STATION, 1, attrs.data, attrs.length, NULL, NULL, 5000);
}

int LdnStationConnect(struct LdnStation* station, uint32_t ifIndex, const char* ssid, unsigned channel, const uint8_t key[16],
                      const uint8_t targetBssid[6], uint8_t hostMac[6]) {
#ifdef _WIN32
	// TEMPORARY diagnostic: which sub-step of association is actually slow. Visible via stderr (redirect mGBA's
	// stderr to a file to capture it - it is a GUI app with no visible console otherwise).
	DWORD tStart = GetTickCount();
#define STATION_CHECKPOINT(label) do { fprintf(stderr, "[ldn-station] %s t+%lums\n", (label), GetTickCount() - tStart); fflush(stderr); } while (0)
#else
#define STATION_CHECKPOINT(label)
#endif
	STATION_CHECKPOINT("enter");
	int error = _setLink(station, ifIndex, true);
	if (error) {
		_fail("could not bring the station interface up (error %d)", error);
		return error;
	}
	STATION_CHECKPOINT("interface up");

	uint8_t rsnIe[22];
	size_t rsnLength = _buildRsnIe(rsnIe);

	struct NlMessage attrs;
	attrs.length = 0;
	NlAddAttrU32(&attrs, NL80211_ATTR_IFINDEX, ifIndex);
	NlAddAttr(&attrs, NL80211_ATTR_SSID, ssid, strlen(ssid));
	NlAddAttrU32(&attrs, NL80211_ATTR_WIPHY_FREQ, _channelFrequency(channel));
	// Without a BSSID hint, cfg80211's CMD_CONNECT has to run its own internal scan to discover which AP is
	// advertising this SSID before it can associate - our own LDN discovery scan never populates that cache (it
	// captures raw 802.11 frames in monitor mode, a completely separate mechanism from a managed-mode scan).
	// Live-measured: without this, association alone took 5-6+ real seconds, comfortably longer than FRLG's own
	// ~4-second IsConnectionComplete polling patience (measured at ~239 polls, one per frame, no backoff, before
	// it gives up via FINISH_CONNECTION) - the connection was actually succeeding, just too late to matter.
	NlAddAttr(&attrs, NL80211_ATTR_MAC, targetBssid, 6);
	NlAddAttrU32(&attrs, NL80211_ATTR_AUTH_TYPE, WLAN_AUTHTYPE_OPEN_SYSTEM);
	NlAddAttr(&attrs, NL80211_ATTR_CONTROL_PORT, NULL, 0);
	uint8_t ethertype[2] = {(uint8_t) ETH_P_LDN, (uint8_t) (ETH_P_LDN >> 8)}; // NL80211_ATTR_CONTROL_PORT_ETHERTYPE is host-endian u16
	NlAddAttr(&attrs, NL80211_ATTR_CONTROL_PORT_ETHERTYPE, ethertype, sizeof(ethertype));
	NlAddAttr(&attrs, NL80211_ATTR_CONTROL_PORT_OVER_NL80211, NULL, 0);
	NlAddAttr(&attrs, NL80211_ATTR_SOCKET_OWNER, NULL, 0);
	NlAddAttrU32(&attrs, NL80211_ATTR_CIPHER_SUITES_PAIRWISE, WLAN_CIPHER_SUITE_CCMP);
	NlAddAttrU32(&attrs, NL80211_ATTR_CIPHER_SUITE_GROUP, WLAN_CIPHER_SUITE_CCMP);
	NlAddAttrU32(&attrs, NL80211_ATTR_AKM_SUITES, WLAN_AKM_SUITE_PSK);
	NlAddAttr(&attrs, NL80211_ATTR_IE, rsnIe, rsnLength);
	NlAddAttr(&attrs, NL80211_ATTR_PRIVACY, NULL, 0);

	STATION_CHECKPOINT("about to send CONNECT request");
	error = GenlRequest(station->genl, station->family, NL_F_ACK, NL80211_CMD_CONNECT, 1, attrs.data, attrs.length, NULL, NULL, 5000);
	STATION_CHECKPOINT("CONNECT request ACKed");
	if (error) {
		_fail("CONNECT request failed (error %d)", error);
		return error;
	}

	struct ConnectEventResult result = {0};
	error = NlWaitForEvent(station->genl, NL80211_CMD_CONNECT, _onConnectEvent, &result, 8000);
	STATION_CHECKPOINT("association result event received");
	if (error) {
		_fail("timed out waiting for the association result");
		// The CONNECT request was already accepted (its own ACK came back above) even though no result event
		// arrived, so the driver may still consider itself mid-connect - release the radio before giving up, or
		// the next attempt's own CONNECT (or even just a monitor's channel switch) can fail with EBUSY. Measured
		// live: a rejected/abandoned CONNECT that is never explicitly disconnected leaves the next association
		// attempt failing (WLAN status 1) and the one after THAT failing to even change channel (EBUSY) - see the
		// project notes.
		LdnStationDisconnect(station, ifIndex);
		return error;
	}
	if (!result.haveStatus) {
		_fail("the CONNECT event did not carry a status code");
		LdnStationDisconnect(station, ifIndex);
		return LDND_ERR_ARGS;
	}
	if (result.status != 0) {
		_fail("association rejected (WLAN status %u)", result.status);
		LdnStationDisconnect(station, ifIndex);
		return (int) result.status;
	}
	if (!result.haveMac) {
		_fail("association succeeded but no BSSID was given");
		LdnStationDisconnect(station, ifIndex);
		return LDND_ERR_ARGS;
	}
	memcpy(hostMac, result.mac, 6);

	error = _installKey(station, ifIndex, result.mac, 0, key);
	STATION_CHECKPOINT("pairwise key installed");
	if (error) {
		_fail("could not install the pairwise key (error %d)", error);
		LdnStationDisconnect(station, ifIndex);
		return error;
	}
	error = _installKey(station, ifIndex, NULL, 1, key);
	STATION_CHECKPOINT("group key installed");
	if (error) {
		_fail("could not install the group key (error %d)", error);
		LdnStationDisconnect(station, ifIndex);
		return error;
	}
	error = _authorize(station, ifIndex, result.mac);
	STATION_CHECKPOINT("authorized, returning");
	if (error) {
		_fail("could not mark the station authorized (error %d)", error);
		LdnStationDisconnect(station, ifIndex);
		return error;
	}
	return 0;
}
#undef STATION_CHECKPOINT

void LdnStationDisconnect(struct LdnStation* station, uint32_t ifIndex) {
	enum { NL80211_CMD_DISCONNECT = 48 };
	struct NlMessage attrs;
	attrs.length = 0;
	NlAddAttrU32(&attrs, NL80211_ATTR_IFINDEX, ifIndex);
	GenlRequest(station->genl, station->family, NL_F_ACK, NL80211_CMD_DISCONNECT, 1, attrs.data, attrs.length, NULL, NULL, 5000);
	// A CMD_DISCONNECT alone was not enough to release the radio on live hardware: the very next run's monitor
	// (or even a completely unrelated fresh scan) failed to change channel with EBUSY, exactly matching what the
	// LDN-0.0.3 reference client's own free_radio() exists to work around ("A failed [or ended] join leaks a
	// still-associated station vif that makes the next association fail" - the reference actually deletes the
	// vif entirely, not just disconnects it, before every attempt). Bringing the interface back down too - the
	// same thing `LdnStationConnect` un-does by bringing it up first - releases the channel/radio lock more
	// thoroughly than disconnecting while leaving it up. Best-effort: ignore the result, this runs during cleanup.
	_setLink(station, ifIndex, false);
}

int LdnStationSendControlPortFrame(struct LdnStation* station, uint32_t ifIndex, const uint8_t destMac[6], const uint8_t* frame, size_t frameLength) {
	struct NlMessage attrs;
	attrs.length = 0;
	NlAddAttrU32(&attrs, NL80211_ATTR_IFINDEX, ifIndex);
	NlAddAttr(&attrs, NL80211_ATTR_FRAME, frame, frameLength);
	NlAddAttr(&attrs, NL80211_ATTR_MAC, destMac, 6);
	uint8_t ethertype[2] = {(uint8_t) ETH_P_LDN, (uint8_t) (ETH_P_LDN >> 8)};
	NlAddAttr(&attrs, NL80211_ATTR_CONTROL_PORT_ETHERTYPE, ethertype, sizeof(ethertype));
	int error = GenlRequest(station->genl, station->family, NL_F_ACK, NL80211_CMD_CONTROL_PORT_FRAME, 1, attrs.data, attrs.length, NULL, NULL, 5000);
	if (error) {
		_fail("CONTROL_PORT_FRAME send failed (error %d)", error);
	}
	return error;
}

struct ControlPortFrameResult {
	bool haveMac;
	uint8_t mac[6];
	uint8_t frame[LDN_STATION_MAX_FRAME];
	size_t frameLength;
};

static void _onControlPortFrame(void* context, const struct NlHeader* header, const uint8_t* payload, size_t payloadLength) {
	struct ControlPortFrameResult* result = context;
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
		case NL80211_ATTR_MAC:
			if (valueLength >= 6) {
				memcpy(result->mac, value, 6);
				result->haveMac = true;
			}
			break;
		case NL80211_ATTR_FRAME:
			if (valueLength <= sizeof(result->frame)) {
				memcpy(result->frame, value, valueLength);
				result->frameLength = valueLength;
			}
			break;
		}
	}
}

int LdnStationWaitControlPortFrame(struct LdnStation* station, uint8_t outMac[6], uint8_t* outFrame, size_t* inOutFrameLength, int timeoutMs) {
	struct ControlPortFrameResult result;
	memset(&result, 0, sizeof(result));
	int error = NlWaitForEvent(station->genl, NL80211_CMD_CONTROL_PORT_FRAME, _onControlPortFrame, &result, timeoutMs);
	if (error) {
		return error;
	}
	if (!result.haveMac || !result.frameLength) {
		_fail("CONTROL_PORT_FRAME event did not carry a MAC and a frame");
		return LDND_ERR_ARGS;
	}
	memcpy(outMac, result.mac, 6);
	size_t copy = result.frameLength < *inOutFrameLength ? result.frameLength : *inOutFrameLength;
	memcpy(outFrame, result.frame, copy);
	*inOutFrameLength = result.frameLength;
	return 0;
}
