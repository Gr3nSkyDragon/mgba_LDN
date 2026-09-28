/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/rfu.h>
#include <mgba/internal/gba/sio/rfu-udp.h>
#include <mgba/internal/gba/sio/rfu-esp32.h>
#ifdef USE_LDND
#include <mgba/internal/gba/sio/rfu-ldnd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The backends a game's wireless adapter can be attached to, by name:
 *   "local"     adapters in other mGBA processes on this computer (UDP on 127.0.0.1, rfu-udp.c)
 *   "ldnd"      a real Switch over local wireless, through a separately-running ldnd (ldn/rfu-ldnd.c).
 *               Built only when USE_LDND is on (Windows only); a stub elsewhere. "broadcast" (its old name) is
 *               still accepted.
 *   "esp32"     GB-Link's ESP32 LDN bridge board over USB serial (esp32/rfu-esp32.c). The serial port is the built-in
 *               Windows one unless the host application supplies its own (esp32/esp32-serial.h; the Android app does).
 * The stub backend leaves the adapter present and working, but nobody is ever in range.
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

static void _stubStatus(struct GBASIORFUBackend* backend, struct GBASIORFUBackendStatus* out) {
	struct StubBackend* stub = (struct StubBackend*) backend;
	out->link = RFU_BACKEND_UNAVAILABLE;
	snprintf(out->detail, sizeof(out->detail), "The \"%s\" backend is not available in this build", stub->name);
}

static struct GBASIORFUBackend* _createStub(const char* name) {
	struct StubBackend* stub = calloc(1, sizeof(*stub));
	if (!stub) {
		return NULL;
	}
	stub->name = name;
	stub->d.init = _stubInit;
	stub->d.status = _stubStatus;
	return &stub->d;
}

struct GBASIORFUBackend* GBASIORFUBackendCreate(const char* name) {
	if (!name) {
		return NULL;
	}
	if (!strcmp(name, "local")) {
		return GBASIORFUUDPCreate();
	}
	if (!strcmp(name, "ldnd") || !strcmp(name, "broadcast")) {
#ifdef USE_LDND
		return GBASIORFULdndCreate();
#else
		return _createStub("ldnd");
#endif
	}
	if (!strcmp(name, "esp32")) {
		return GBASIORFUESP32Create();
	}
	return NULL;
}

void GBASIORFUBackendGetStatus(struct GBASIORFUBackend* backend, struct GBASIORFUBackendStatus* out) {
	memset(out, 0, sizeof(*out));
	out->link = RFU_BACKEND_READY;
	out->hostsHeard = -1;
	if (backend && backend->status) {
		backend->status(backend, out);
	}
}

void GBASIORFUBackendProbe(struct GBASIORFUBackend* backend) {
	if (backend && backend->probe) {
		backend->probe(backend);
	}
}

void GBASIORFUBackendDestroy(struct GBASIORFUBackend* backend) {
	// Every backend is a heap allocation that starts with its struct GBASIORFUBackend.
	free(backend);
}
