/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LDN_STATION_H
#define GBA_SIO_LDN_STATION_H

#include "ldnd.h"
#include "netlink.h"

#include <stdbool.h>

/*
 * Joining a Switch's LDN network at the Wi-Fi layer: WPA2-PSK/CCMP association (nl80211 CMD_CONNECT), installing
 * the pre-shared key (there is no real 4-way handshake - LDN gives both sides the same 16-byte key up front, via
 * LdnDeriveWlanKey, and it is installed directly as both the pairwise and group key) and marking the station
 * authorized to pass data.
 *
 * This is only the Wi-Fi layer. Above it, LDN has its own authentication handshake (separate from WPA2) before a
 * host actually accepts a joiner into its game session, and only after THAT does the Pia transport carry any game
 * data - neither is implemented yet (see rfu-broadcast.c's connect(), which stops here for now).
 */

struct LdnStation;

// `conn` is an ALREADY-OPEN ldnd connection, owned by the caller (LdnStationClose does not close it) - ldnd only
// ever accepts one pipe connection at a time, so a caller that is also scanning (ldn-monitor.c) or otherwise
// talking to ldnd must share its one connection rather than open a second one. Resolves nl80211 (a new virtual
// netlink socket over the same connection - that part ldnd is fine with any number of) and joins its "mlme"
// multicast group (where the asynchronous CMD_CONNECT result arrives). Returns NULL on failure - see
// LdnStationLastError().
struct LdnStation* LdnStationOpen(struct LdndConnection* conn);
void LdnStationClose(struct LdnStation*);

// Finds the adapter's station-mode interface (e.g. "wlan0" - distinct from the monitor interface ldn-monitor.c
// uses for scanning). Returns 0 and sets *ifIndex and, if `mac` is non-NULL, our own interface's MAC address
// (needed to build the Pia transport's own raw Ethernet frames - see ldn-pia.c), or a negative
// LdnStationLastError()-explained error.
int LdnStationFindInterface(struct LdnStation*, uint32_t* ifIndex, uint8_t mac[6]);

// Associates with `ssid` on `channel` (1, 6 or 11) as WPA2-PSK/CCMP using `key` (the 16 bytes from
// LdnDeriveWlanKey) as the pre-shared key, waits (up to a few seconds) for the result, and on success installs
// that key and marks the station authorized to pass data. `targetBssid` is the AP's own MAC, already known from
// the LDN monitor-mode scan that found this advertisement (see LdnMonitor's advertisement callback) - passing it
// as NL80211_ATTR_MAC lets the kernel associate directly instead of first running its own internal scan to
// discover the BSSID (cfg80211's BSS cache is never populated by our raw monitor-mode capture, since that is a
// separate mechanism from a normal managed-mode scan - live-measured to take several seconds without this hint,
// consistently long enough to blow through FRLG's own ~4-second IsConnectionComplete polling patience). `hostMac`
// receives the AP's BSSID back (redundant with `targetBssid` in practice, kept for existing callers). Returns 0
// on success, the WLAN status code (a small positive number) if the network rejected the association, or a
// negative LDND_ERR_*/LdnStationLastError()-explained error.
int LdnStationConnect(struct LdnStation*, uint32_t ifIndex, const char* ssid, unsigned channel, const uint8_t key[16], const uint8_t targetBssid[6],
                      uint8_t hostMac[6]);

// Leaves the network (best effort; safe to call even if never connected).
void LdnStationDisconnect(struct LdnStation*, uint32_t ifIndex);

// The largest NL80211_ATTR_FRAME payload LdnStationWaitControlPortFrame will accept - comfortably larger than the
// biggest frame ldn-auth.c builds or expects (an authentication frame tops out under 1000 bytes; see its header).
enum { LDN_STATION_MAX_FRAME = 2048 };

// Sends `frame` (a complete LDN control-port frame - see ldn-auth.c) to `destMac` over nl80211's control-port
// mechanism (NL80211_CMD_CONTROL_PORT_FRAME, tagged with LDN's own ethertype - the same one LdnStationConnect
// already registers this station for at CONNECT time). This is NOT a raw 802.11 action frame and NOT data-plane
// traffic; it is how LDN's own authentication handshake (on top of the WPA2 association LdnStationConnect
// completed) is carried. Returns 0, or a negative LDND_ERR_*/LdnStationLastError()-explained error.
int LdnStationSendControlPortFrame(struct LdnStation*, uint32_t ifIndex, const uint8_t destMac[6], const uint8_t* frame, size_t frameLength);

// Blocks (up to timeoutMs) for the next incoming control-port frame, copying its source MAC into `outMac` and its
// payload into `outFrame` (at most `*inOutFrameLength` bytes; `*inOutFrameLength` is set to the frame's real size
// on return, which may be larger than what was copied if the buffer was too small). Returns 0, or a negative
// LDND_ERR_*/LdnStationLastError()-explained error (LDND_ERR_TIMEOUT if nothing arrived in time).
int LdnStationWaitControlPortFrame(struct LdnStation*, uint8_t outMac[6], uint8_t* outFrame, size_t* inOutFrameLength, int timeoutMs);

const char* LdnStationLastError(void);

#endif
