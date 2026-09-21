/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_RFU_UDP_H
#define GBA_SIO_RFU_UDP_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gba/sio/rfu.h>

/*
 * Local "air" for the wireless adapter: adapters in different mGBA processes on the same machine find each other over
 * UDP on 127.0.0.1 (ports base .. base + 7, first free one is taken by each process). The messages on the wire are
 * the adapter-level events - a host's broadcast record, connect / accept / refuse, payload data, disconnect - which
 * is deliberately the same vocabulary a bridge to another network (e.g. LDN) needs.
 *
 * Environment: MGBA_RFU_UDP_PORT sets the base port (default 45600).
 */

struct GBASIORFUBackend* GBASIORFUUDPCreate(void);
void GBASIORFUUDPDestroy(struct GBASIORFUBackend*);

CXX_GUARD_END

#endif
