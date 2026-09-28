/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_RFU_LDND_H
#define GBA_SIO_RFU_LDND_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gba/sio/rfu.h>

/*
 * The "ldnd" wireless adapter backend: a real Switch over local wireless (LDN), reached through a running
 * ldnd (protocol 7; its pipe is \\.\pipe\ldnd unless the LDN_DAEMON environment variable names another). mGBA never
 * starts or manages ldnd itself - the user runs it separately, with their adapter and prod.keys, before choosing
 * ldnd; if ldnd is not reachable, the adapter is simply left with nobody in range.
 *
 * Only built when USE_LDND is on (Windows only for now; see src/gba/sio/ldn/rfu-ldnd.c and its
 * siblings). When it is off, GBASIORFUBackendCreate("ldnd") returns a stub instead, and this header is simply
 * not used.
 *
 * Client role only: while the game searches, ldnd scans and every FireRed/LeafGreen room it finds becomes an RFU
 * broadcast record for the game; connecting joins that room through ldnd and runs the Switch game's Pia session
 * over it. Hosting is not implemented.
 */

struct GBASIORFUBackend* GBASIORFULdndCreate(void);

CXX_GUARD_END

#endif
