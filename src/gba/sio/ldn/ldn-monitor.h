/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LDN_MONITOR_H
#define GBA_SIO_LDN_MONITOR_H

#include "ldnd.h"
#include "netlink.h"

#include <stdbool.h>

/*
 * Puts the Wi-Fi adapter behind a running ldnd into monitor mode and delivers the 802.11 management frames it
 * captures: every raw frame (for callers that want their own statistics, e.g. ldn-scan), and separately, already
 * filtered down to LDN advertisements (a vendor-specific action frame carrying Nintendo's OUI), the piece a caller
 * such as the Broadcast backend actually wants.
 *
 * The monitor interface ("ldnmon0") is created once and torn down on LdnMonitorClose - reused if one is already
 * there from an earlier run that did not exit cleanly. Frame delivery happens on ldnd's own reader thread (see
 * ldnd.h); LdnMonitorSetChannel blocks the calling thread on a netlink round trip, so callers on the emulation
 * thread must not call it there - drive channel hopping from a thread of their own.
 */

struct LdnMonitor;

typedef void (*LdnMonitorRawCallback)(void* context, const uint8_t* radiotapFrame, size_t length);
typedef void (*LdnMonitorAdvertisementCallback)(void* context, const uint8_t* sourceMac, const uint8_t* actionBody, size_t actionBodyLength,
                                                 unsigned channel);

// `pipePath` may be NULL for ldnd's default (\\.\pipe\ldnd). `firstChannel` is a channel number (1-13); channel
// switches after this are up to the caller via LdnMonitorSetChannel. Either callback may be NULL. Returns NULL on
// failure - LdnMonitorLastError() explains why until the next call on any monitor.
struct LdnMonitor* LdnMonitorOpen(const char* pipePath, unsigned firstChannel, LdnMonitorRawCallback rawCallback,
                                   LdnMonitorAdvertisementCallback advertisementCallback, void* context);
void LdnMonitorClose(struct LdnMonitor*);

// Blocking: switches the monitor interface to a channel (1-13). Returns 0, or a positive ldnd/netlink error.
int LdnMonitorSetChannel(struct LdnMonitor*, unsigned channel);

// The monitor's underlying ldnd connection, for a caller that needs to open more virtual sockets on it itself
// (e.g. ldn-station.c's LdnStationOpen - ldnd accepts only one pipe connection at a time, so anything else that
// needs to talk to it while the monitor is open must share this one rather than open a second). Still owned by
// the monitor; do not close it directly.
struct LdndConnection* LdnMonitorConnection(struct LdnMonitor*);

// One line describing the last failure from this module (LdnMonitorOpen or LdnMonitorSetChannel). Valid until the
// next call; not thread-safe (call from the same thread that called the failing function).
const char* LdnMonitorLastError(void);

#endif
