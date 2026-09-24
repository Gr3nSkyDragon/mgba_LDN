/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LDN_LDND_H
#define GBA_SIO_LDN_LDND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Client for the ldnd daemon's named-pipe protocol (Windows).
 *
 * ldnd runs a small Linux kernel (LKL) around the USB Wi-Fi adapter and exposes *virtual sockets* of that kernel
 * over \\.\pipe\ldnd: netlink sockets (nl80211 control) and packet sockets (raw 802.11 frames). Frames on the pipe:
 *   u8 op, u32 socket id, u32 arg0, arg1, arg2, u32 blob length (all little endian, 21 bytes), then the blob.
 * The client picks the socket ids. Ops: SOCKET, BIND, SETSOCKOPT, GETSOCKNAME, START, SENDTO, CLOSE (client to
 * daemon) and REPLY, DATA (daemon to client). REPLY carries an error code in arg0 (0 = ok). SENDTO and CLOSE are not
 * answered. DATA delivers what a started socket received.
 *
 * Requests are synchronous (one at a time); data is delivered on the connection's reader thread.
 */

struct LdndConnection;

enum {
	LDND_OK = 0,
	// Positive results are ldnd error codes (1 sid taken, 2 too many sockets, 3 invalid sid, 4 invalid op, 5 other).
	LDND_ERR_PIPE = -1,    // the pipe is gone or a read/write failed
	LDND_ERR_TIMEOUT = -2, // no reply in time
	LDND_ERR_ARGS = -3,
};

// Returns true if this callback's owner recognised `socketId` and handled the data (dispatch stops there);
// false to let another registered callback on the same connection try it.
typedef bool (*LdndDataCallback)(void* context, uint32_t socketId, const uint8_t* data, size_t length);

// Opens the pipe (path NULL means \\.\pipe\ldnd). Returns NULL when ldnd is not running.
struct LdndConnection* LdndOpen(const char* pipePath);
void LdndClose(struct LdndConnection*);

// ldnd accepts only one pipe connection at a time, but that one connection carries any number of independent
// virtual sockets - so more than one module (e.g. ldn-monitor.c's scan and ldn-station.c's association) may need
// to share a single LdndConnection. Each registers its own callback with LdndAddDataCallback (called on the
// reader thread for every DATA frame, in registration order, until one returns true) and unregisters it with
// LdndRemoveDataCallback before it goes away. Up to 8 callbacks per connection.
bool LdndAddDataCallback(struct LdndConnection*, LdndDataCallback callback, void* context);
void LdndRemoveDataCallback(struct LdndConnection*, LdndDataCallback callback, void* context);

int LdndSocket(struct LdndConnection*, int domain, int type, int protocol, uint32_t* socketId);
int LdndBind(struct LdndConnection*, uint32_t socketId, const void* sockaddr, size_t length);
int LdndSetSockOpt(struct LdndConnection*, uint32_t socketId, int level, int option, const void* value, size_t length);
// length is the size of `out` on entry and the size of the address on return.
int LdndGetSockName(struct LdndConnection*, uint32_t socketId, void* out, size_t* length);
// Makes the daemon-side socket non-blocking and starts delivering its data as DATA frames.
int LdndStart(struct LdndConnection*, uint32_t socketId);
int LdndSendTo(struct LdndConnection*, uint32_t socketId, int flags, const void* data, size_t length);
int LdndCloseSocket(struct LdndConnection*, uint32_t socketId);

#endif
