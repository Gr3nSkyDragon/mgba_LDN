/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/rfu.h>
#include <mgba/internal/gba/sio/rfu-udp.h>
#ifdef USE_LDN_BROADCAST
#include <mgba/internal/gba/sio/rfu-broadcast.h>
#endif

#include <stdlib.h>
#include <string.h>

/*
 * The backends a game's wireless adapter can be attached to, by name:
 *   "local"     adapters in other mGBA processes on this computer (UDP on 127.0.0.1, rfu-udp.c)
 *   "broadcast" a real Switch over local wireless, through a separately-running ldnd (ldn/rfu-broadcast.c).
 *               Searching only so far; joining always fails. Built only when USE_LDN_BROADCAST is on
 *               (Windows only); a stub elsewhere.
 *   "usb"       an external adapter on a USB port, e.g. an ESP32 - NOT IMPLEMENTED YET (stub)
 * A stub backend leaves the adapter present and working, but nobody is ever in range.
 */

struct StubBackend {
	struct GBASIORFUBackend d;
	const char* name;
};

static bool _stubInit(struct GBASIORFUBackend* backend, struct GBASIORFU* rfu) {
	struct StubBackend* stub = (struct StubBackend*) backend;
	mLOG(GBA_RFU, WARN, "The \"%s\" wireless adapter backend is not implemented yet; the adapter has nobody in range.", stub->name);
	GBASIORFUTrace(rfu, "BACKEND %s is a stub", stub->name);
	return true;
}

static struct GBASIORFUBackend* _createStub(const char* name) {
	struct StubBackend* stub = calloc(1, sizeof(*stub));
	if (!stub) {
		return NULL;
	}
	stub->name = name;
	stub->d.init = _stubInit;
	return &stub->d;
}

struct GBASIORFUBackend* GBASIORFUBackendCreate(const char* name) {
	if (!name) {
		return NULL;
	}
	if (!strcmp(name, "local")) {
		return GBASIORFUUDPCreate();
	}
	if (!strcmp(name, "broadcast")) {
#ifdef USE_LDN_BROADCAST
		return GBASIORFUBroadcastCreate();
#else
		return _createStub("broadcast");
#endif
	}
	if (!strcmp(name, "usb")) {
		return _createStub("usb");
	}
	return NULL;
}

void GBASIORFUBackendDestroy(struct GBASIORFUBackend* backend) {
	// Every backend is a heap allocation that starts with its struct GBASIORFUBackend.
	free(backend);
}
