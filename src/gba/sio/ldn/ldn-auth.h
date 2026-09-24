/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LDN_AUTH_H
#define GBA_SIO_LDN_AUTH_H

#include "ldn-station.h"
#include "ldn.h"

/*
 * LDN's own authentication handshake: a protocol layered ON TOP of a successful WPA2 association (see
 * ldn-station.h), exchanged as nl80211 control-port frames (NL80211_CMD_CONTROL_PORT_FRAME) rather than as raw
 * 802.11 action frames or actual data-plane traffic. Only after this succeeds does a host consider a joiner part
 * of its game session; only after THAT does the Pia transport (not implemented) carry any game data.
 *
 * Wire format and key derivation read from the LDN-0.0.3 reference client's own ldn/__init__.py
 * (AuthenticationFrame/ChallengeRequest encode/decode) and ldn/wlan.py (Station.send_custom_frame,
 * Station._connect_network's control-port attributes) - used only to learn the wire format, not copied, the same
 * way ldn.c's advertisement decoder was built from frlgsim/transport.py. Not yet live-tested against a real
 * Switch (see rfu-broadcast.c/the project notes) - the byte layout was cross-checked field by field against that
 * reference source but every offset and constant here should be treated as unverified until a live join actually
 * completes.
 */

enum {
	LDN_AUTH_SUCCESS = 0,
	LDN_AUTH_DENIED_BY_POLICY = 1,
	LDN_AUTH_MALFORMED_REQUEST = 2,
	LDN_AUTH_TIMEOUT = 3,
	LDN_AUTH_INVALID_VERSION = 4,
	LDN_AUTH_UNEXPECTED = 5,
	LDN_AUTH_CHALLENGE_FAILURE = 6,
	// Not one of LDN's own AUTH_* status codes: means no valid response arrived from the host at all (transport
	// timeout, as opposed to a response that explicitly rejected the request).
	LDN_AUTH_NO_RESPONSE = -100,
};

// Performs LDN's authentication handshake with the host described by `ad` (already associated at the Wi-Fi layer
// - see LdnStationConnect) over `station`/`ifIndex`/`hostMac`, identifying this joiner as `username` (up to 32
// bytes - this is the LDN-level participant name a host would show in its own participant list, unrelated to the
// RFU beacon's own name field) with `appVersion`. Retries up to 3 times (700ms each, matching the reference
// client) if no response arrives. Returns LDN_AUTH_SUCCESS (0), one of the other LDN_AUTH_* status codes the host
// rejected the request with, LDN_AUTH_NO_RESPONSE, or a negative LDND_ERR_*-style transport error (see
// LdnStationLastError()).
int LdnStationAuthenticate(struct LdnStation* station, uint32_t ifIndex, const uint8_t hostMac[6], const struct LdnAdvertisement* ad,
                            const struct LdnKeys* keys, const char* username, uint16_t appVersion);

#endif
