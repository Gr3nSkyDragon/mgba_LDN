/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "esp32-serial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

struct Esp32Serial {
	HANDLE handle;
};

struct Esp32Serial* Esp32SerialOpen(const char* name, unsigned baud) {
	char path[64];
	snprintf(path, sizeof(path), "\\.\%s", name);
	HANDLE handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	DCB dcb;
	memset(&dcb, 0, sizeof(dcb));
	dcb.DCBlength = sizeof(dcb);
	if (!GetCommState(handle, &dcb)) {
		CloseHandle(handle);
		return NULL;
	}
	dcb.BaudRate = baud;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fBinary = TRUE;
	dcb.fOutxCtsFlow = FALSE;
	dcb.fOutxDsrFlow = FALSE;
	dcb.fDtrControl = DTR_CONTROL_ENABLE; // a USB CDC device often only talks once it sees DTR
	dcb.fRtsControl = RTS_CONTROL_DISABLE; // RTS/DTR edges are what ESP32 auto-reset watches; leave RTS low
	dcb.fOutX = FALSE;
	dcb.fInX = FALSE;
	dcb.fAbortOnError = FALSE;
	SetCommState(handle, &dcb); // not fatal: a USB CDC endpoint ignores most of this
	COMMTIMEOUTS timeouts;
	memset(&timeouts, 0, sizeof(timeouts));
	timeouts.ReadIntervalTimeout = MAXDWORD; // ReadFile returns immediately with whatever is there
	timeouts.WriteTotalTimeoutConstant = 2000;
	SetCommTimeouts(handle, &timeouts);
	SetupComm(handle, 1 << 16, 1 << 16);
	PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

	struct Esp32Serial* port = calloc(1, sizeof(*port));
	if (!port) {
		CloseHandle(handle);
		return NULL;
	}
	port->handle = handle;
	return port;
}

void Esp32SerialClose(struct Esp32Serial* port) {
	if (!port) {
		return;
	}
	CloseHandle(port->handle);
	free(port);
}

int Esp32SerialRead(struct Esp32Serial* port, uint8_t* buffer, size_t capacity) {
	DWORD got = 0;
	if (!ReadFile(port->handle, buffer, (DWORD) capacity, &got, NULL)) {
		return -1;
	}
	return (int) got;
}

bool Esp32SerialWrite(struct Esp32Serial* port, const void* data, size_t length) {
	const uint8_t* bytes = data;
	while (length) {
		DWORD put = 0;
		if (!WriteFile(port->handle, bytes, (DWORD) length, &put, NULL) || !put) {
			return false;
		}
		bytes += put;
		length -= put;
	}
	return true;
}

#else

struct Esp32Serial* Esp32SerialOpen(const char* name, unsigned baud) {
	(void) name;
	(void) baud;
	return NULL;
}
void Esp32SerialClose(struct Esp32Serial* port) {
	(void) port;
}
int Esp32SerialRead(struct Esp32Serial* port, uint8_t* buffer, size_t capacity) {
	(void) port;
	(void) buffer;
	(void) capacity;
	return -1;
}
bool Esp32SerialWrite(struct Esp32Serial* port, const void* data, size_t length) {
	(void) port;
	(void) data;
	(void) length;
	return false;
}

#endif
