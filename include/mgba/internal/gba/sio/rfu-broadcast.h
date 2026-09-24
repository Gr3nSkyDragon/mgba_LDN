/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_RFU_BROADCAST_H
#define GBA_SIO_RFU_BROADCAST_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gba/sio/rfu.h>

/*
 * The "Broadcast" wireless adapter backend: a real Switch over local wireless (LDN), reached through a running
 * ldnd. mGBA never starts or manages ldnd itself - the user runs it separately, however their setup needs, before
 * choosing Broadcast; if ldnd is not reachable, the adapter is simply left with nobody in range, exactly like the
 * "usb"/"broadcast"-before-this-existed stubs did.
 *
 * Only built when USE_LDN_BROADCAST is on (Windows only for now; see src/gba/sio/ldn/rfu-broadcast.c and its
 * siblings). When it is off, GBASIORFUBackendCreate("broadcast") returns the old stub instead, and this header is
 * simply not used.
 *
 * Scope so far: searching only. While the game is polling for broadcasts (BroadcastReadStart/Poll/End), this
 * backend puts the Wi-Fi adapter into monitor mode, hops the LDN channels, and turns every FRLG advertisement it
 * decodes into a synthesized RFU broadcast record the driver hands the game. Joining (Connect) is not implemented
 * yet: it always fails immediately with a trace line, rather than hanging.
 */

struct GBASIORFUBackend* GBASIORFUBroadcastCreate(void);

// Give it the path to a prod.keys file before it is attached (before core->setPeripheral). Without it, the
// backend still puts the adapter in monitor mode while searching, but cannot decrypt what it captures.
void GBASIORFUBroadcastSetKeysPath(struct GBASIORFUBackend*, const char* prodKeysPath);

CXX_GUARD_END

#endif
