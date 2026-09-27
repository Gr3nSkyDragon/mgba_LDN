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

static struct Esp32Serial* _platformOpen(const char* name, unsigned baud) {
	char path[64];
	snprintf(path, sizeof(path), "\\\\.\\%s", name);
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

static void _platformClose(struct Esp32Serial* port) {
	if (!port) {
		return;
	}
	CloseHandle(port->handle);
	free(port);
}

static int _platformRead(struct Esp32Serial* port, uint8_t* buffer, size_t capacity) {
	DWORD got = 0;
	if (!ReadFile(port->handle, buffer, (DWORD) capacity, &got, NULL)) {
		return -1;
	}
	return (int) got;
}

static bool _platformWrite(struct Esp32Serial* port, const void* data, size_t length) {
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

// Whether a COM port of that name exists right now. The registry keeps entries for unplugged devices.
static bool _portPresent(const char* name) {
	char device[80];
	snprintf(device, sizeof(device), "\\\\.\\%s", name);
	HANDLE probe = CreateFileA(device, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	bool present = probe != INVALID_HANDLE_VALUE || GetLastError() == ERROR_ACCESS_DENIED;
	if (probe != INVALID_HANDLE_VALUE) {
		CloseHandle(probe);
	}
	return present;
}

// The COM port of a present USB device whose enumeration name (HKLM\SYSTEM\CurrentControlSet\Enum\USB\<name>) starts with
// `prefix`. A composite device appears as VID_xxxx&PID_yyyy&MI_00 (its interface 0 is the serial function), a plain one
// without the MI_ suffix; both start with the vendor id. Each has one key per physical instance, whose Device Parameters
// hold the PortName.
static bool _findPortForVendor(const char* prefix, char* out, size_t capacity) {
	HKEY usb;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\USB", 0, KEY_READ, &usb) != ERROR_SUCCESS) {
		return false;
	}
	bool found = false;
	char device[256];
	for (DWORD d = 0; !found; ++d) {
		DWORD length = sizeof(device);
		if (RegEnumKeyExA(usb, d, device, &length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
			break;
		}
		if (_strnicmp(device, prefix, strlen(prefix)) != 0) {
			continue;
		}
		char devicePath[400];
		snprintf(devicePath, sizeof(devicePath), "SYSTEM\\CurrentControlSet\\Enum\\USB\\%s", device);
		HKEY deviceKey;
		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, devicePath, 0, KEY_READ, &deviceKey) != ERROR_SUCCESS) {
			continue;
		}
		char instance[256];
		for (DWORD i = 0; !found; ++i) {
			DWORD instanceLength = sizeof(instance);
			if (RegEnumKeyExA(deviceKey, i, instance, &instanceLength, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) {
				break;
			}
			char path[700];
			snprintf(path, sizeof(path), "%s\\%s\\Device Parameters", devicePath, instance);
			HKEY params;
			if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &params) != ERROR_SUCCESS) {
				continue;
			}
			char name[64];
			DWORD size = sizeof(name);
			DWORD type = 0;
			bool have = RegQueryValueExA(params, "PortName", NULL, &type, (LPBYTE) name, &size) == ERROR_SUCCESS && type == REG_SZ;
			RegCloseKey(params);
			if (have && strlen(name) < capacity && _portPresent(name)) {
				strcpy(out, name);
				found = true;
			}
		}
		RegCloseKey(deviceKey);
	}
	RegCloseKey(usb);
	return found;
}

static bool _platformFindEspressif(char* out, size_t capacity) {
	// Boards with a native USB port (the S3/C3/C6 "USB" connector) enumerate as Espressif's own device, which goes first.
	// Boards with only a UART bridge (a single-connector ESP32, or the "UART" connector of a two-port S3 board) show up
	// as whichever bridge chip they carry, so those vendors are tried next. When several are plugged in the order above
	// decides; MGBA_RFU_ESP32_PORT names one explicitly.
	static const char* const kVendors[] = {
		"VID_303A", // Espressif (native USB Serial/JTAG)
		"VID_10C4", // Silicon Labs CP210x
		"VID_1A86", // WCH CH340 / CH9102
		"VID_0403", // FTDI
	};
	for (size_t i = 0; i < sizeof(kVendors) / sizeof(kVendors[0]); ++i) {
		if (_findPortForVendor(kVendors[i], out, capacity)) {
			return true;
		}
	}
	return false;
}

#else

static struct Esp32Serial* _platformOpen(const char* name, unsigned baud) {
	(void) name;
	(void) baud;
	return NULL;
}
static void _platformClose(struct Esp32Serial* port) {
	(void) port;
}
static int _platformRead(struct Esp32Serial* port, uint8_t* buffer, size_t capacity) {
	(void) port;
	(void) buffer;
	(void) capacity;
	return -1;
}
static bool _platformWrite(struct Esp32Serial* port, const void* data, size_t length) {
	(void) port;
	(void) data;
	(void) length;
	return false;
}
static bool _platformFindEspressif(char* out, size_t capacity) {
	(void) out;
	(void) capacity;
	return false;
}

#endif

// ---- Dispatch ---------------------------------------------------------------------------------------------------
// The backend only ever calls the Esp32Serial* functions below. They forward to whichever implementation is installed:
// the built-in one for this platform (Windows COM port; a stub elsewhere) unless a host application - e.g. an Android app
// that owns the USB permission and device handle - replaces it with Esp32SerialSetOps.

static const struct Esp32SerialOps kPlatformOps = {
	_platformOpen,
	_platformClose,
	_platformRead,
	_platformWrite,
	_platformFindEspressif,
};

static const struct Esp32SerialOps* sOps = &kPlatformOps;

void Esp32SerialSetOps(const struct Esp32SerialOps* ops) {
	sOps = ops ? ops : &kPlatformOps;
}

struct Esp32Serial* Esp32SerialOpen(const char* name, unsigned baud) {
	return sOps->open(name, baud);
}

void Esp32SerialClose(struct Esp32Serial* port) {
	sOps->close(port);
}

int Esp32SerialRead(struct Esp32Serial* port, uint8_t* buffer, size_t capacity) {
	return sOps->read(port, buffer, capacity);
}

bool Esp32SerialWrite(struct Esp32Serial* port, const void* data, size_t length) {
	return sOps->write(port, data, length);
}

bool Esp32SerialFindEspressif(char* out, size_t capacity) {
	return sOps->find(out, capacity);
}
