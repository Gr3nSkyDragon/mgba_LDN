/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * uds-gblink-relay: a real Game Boy cartridge trades with a 3DS's Virtual Console Pokemon game through a GB-Link and an ESP32 board,
 * both plugged into this PC's USB. No wires between the boards: this program is the host in the middle, as GB-Link's own web page is
 * between a GB-Link and their ESP32 firmware.
 *
 *   Game Boy --link cable-- GB-Link (Pico, GBLink-Firmware 2.2.7) --USB-- this program --USB-- ESP32 (esp32-uds-bridge 1.7) ~~ 3DS
 *
 * The board runs the Virtual Console wrapper and drives the link clock (uds-clock.c), and speaks the Pico's own serial protocol
 * (| 47 42 | channel | length:u16 LE | payload |) inside its GB_PICO messages. The GB-Link's USB serial port speaks the same frames
 * (its SerialLayer on CDC-ACM; GB-Link's web page uses that port too, USB vendor id 2FE3). So this program only carries bytes: the
 * board's GB_PICO frames go to the GB-Link as they are, and what the GB-Link sends goes back to the board.
 *
 * The cartridge's generation is detected by the board from its first sync (the Cable Club receptionist's: 60 in Gen 1, 61 in Gen 2),
 * since nobody can read the header of a cartridge in a Game Boy. --gen 1 or --gen 2 tells it instead (needed for firmware 1.6).
 *
 * With --port there is no GB-Link: the console's link port is wired to the board's own GPIOs (esp32-uds-bridge 1.8 built with
 * CONFIG_UDS_LINK_PORT) and the board clocks it itself, so there is nothing to carry. This program then only starts the wrapper and
 * shows the board's log; the board also runs on its own with no PC, and this is the way to watch it.
 *
 *   GBA (GB/GBC mode) --link port, level shifter-- ESP32 (esp32-uds-bridge 1.8) ~~ 3DS        (USB to this PC for the log only)
 *
 * Usage: uds-gblink-relay [--esp32 COMx] [--gblink COMy | --port] [--key <aes_keys.txt>] [--gen 1|2] [--name NAME] [--trace <file>]
 *   --esp32, --gblink   the ports; found by their USB vendor ids when left out (Espressif 303A; GB-Link 2FE3)
 *   --port              no GB-Link: the console is on the board's own link port
 *   --key               stores the 3DS UDS key (slot 0x2D) on the board if it has none (as uds-esp32-probe --store-key)
 *   --name              the name the 3DS is shown (default GB-Link)
 *   --trace             the board's log, every byte the board clocks, and the relay's own notes
 * Ctrl+C stops the wrapper and takes the GB-Link out of Game Boy mode.
 *
 * Windows only for now (the serial ports are Esp32Serial's built-in implementation). Build target: uds-gblink-relay.
 */
#include <mgba/internal/gb/sio/uds-esp32.h>
#include <mgba/internal/gb/sio/uds-keyfile.h>

#include "../../gba/sio/esp32/esp32-serial.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#define GBLINK_VENDOR 0x2FE3
#define PICO_CMD_CANCEL 0x01
#define PICO_CMD_SET_VOLTAGE_5V 0x41 // an official GB-Link sets the link port's voltage in software; the DIY board's SW1 does it by hand

static volatile bool sStop;
static FILE* sTrace;
static struct Esp32Serial* sGbLink;
static bool sGbLinkFailed;
static unsigned sToGbLink, sFromGbLink; // bytes carried

static unsigned _nowMs(void) {
#ifdef _WIN32
	return (unsigned) GetTickCount64();
#else
	return 0;
#endif
}

static void _note(const char* format, ...) {
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	printf("%s\n", line);
	if (sTrace) {
		fprintf(sTrace, "%8u relay: %s\n", _nowMs(), line);
		fflush(sTrace);
	}
}

#ifdef _WIN32
static BOOL WINAPI _onCtrl(DWORD type) {
	(void) type;
	sStop = true;
	return TRUE;
}
#endif

// A frame to the GB-Link, built here (the relay's own: voltage, and the Cancel at the end).
static void _picoCommand(uint8_t command) {
	const uint8_t frame[6] = {0x47, 0x42, 0x00, 0x01, 0x00, command};
	if (sGbLink && !Esp32SerialWrite(sGbLink, frame, sizeof(frame))) {
		sGbLinkFailed = true;
	}
}

// The board's frames for the Pico go to the GB-Link as they are.
static void _onPico(void* context, const uint8_t* frame, size_t length) {
	(void) context;
	if (!sGbLink || sGbLinkFailed) {
		return;
	}
	if (!Esp32SerialWrite(sGbLink, frame, length)) {
		sGbLinkFailed = true;
		return;
	}
	sToGbLink += (unsigned) length;
}

static const char* const kRoom[] = {"looking for a 3DS", "authenticating", "joining", "joined"};
static const char* const kSession[] = {"idle", "setting up the session", "linked", "the 3DS left"};
static const char* const kWire[] = {"down", "choosing roles", "idle", "syncing", "link menu", "exchanging data"};

static void _onState(void* context, const uint8_t state[5]) {
	(void) context;
	const char* gen = state[3] == 1 ? "Gen 1" : state[3] == 2 ? "Gen 2" : "generation not known yet";
	if (state[1] == 0xFF || state[1] != 2) {
		_note("3DS: %s%s%s (%s, channel %u)", state[0] < 4 ? kRoom[state[0]] : "?", state[1] < 4 ? ", " : "",
		      state[1] < 4 ? kSession[state[1]] : "", gen, state[4]);
	} else {
		_note("cable: %s (%s)", state[2] <= 5 ? kWire[state[2]] : "?", gen);
	}
}

// The board's log: all of it to the trace, the lines worth seeing to the console.
static void _onLog(void* context, const char* text, size_t length) {
	(void) context;
	if (sTrace) {
		fprintf(sTrace, "%8u board: %.*s\n", _nowMs(), (int) length, text);
		fflush(sTrace);
	}
	static const char* const kShown[] = {"generation:", "pico:", "port:", "wrapper started", "joining", "joined", "the session closed",
	                                     "the host went silent", "mail done", "clock: the cartridge answered", "clock (Pico)",
	                                     "clock (link port)"};
	size_t i;
	for (i = 0; i < sizeof(kShown) / sizeof(kShown[0]); ++i) {
		size_t n = strlen(kShown[i]);
		if (length >= n && !memcmp(text, kShown[i], n)) {
			printf("board: %.*s\n", (int) length, text);
			return;
		}
	}
}

static void _onStats(void* context, const uint32_t stats[UDS_ESP32_GB_STATS]) {
	(void) context;
	if (sTrace) {
		fprintf(sTrace, "%8u relay: board stats: beacons %u, frames sent %u received %u, units sent %u received %u, exchanges %u; "
		                "bytes to the GB-Link %u, from it %u\n",
		        _nowMs(), (unsigned) stats[0], (unsigned) stats[1], (unsigned) stats[2], (unsigned) stats[7], (unsigned) stats[8],
		        (unsigned) stats[9], sToGbLink, sFromGbLink);
		fflush(sTrace);
	}
}

static void _closeGbLink(void) {
	if (sGbLink) {
		Esp32SerialClose(sGbLink);
		sGbLink = NULL;
	}
}

static int _usage(void) {
	printf("usage: uds-gblink-relay [--esp32 COMx] [--gblink COMy | --port] [--key <aes_keys.txt>] [--gen 1|2] [--name NAME] "
	       "[--trace <file>]\n");
	return 2;
}

int main(int argc, char** argv) {
	const char* espPort = NULL;
	const char* gbPort = NULL;
	const char* keyPath = NULL;
	const char* tracePath = NULL;
	const char* name = "GB-Link";
	bool linkPort = false;
	int generation = 0;
	int i;
	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--esp32") && i + 1 < argc) {
			espPort = argv[++i];
		} else if (!strcmp(argv[i], "--gblink") && i + 1 < argc) {
			gbPort = argv[++i];
		} else if (!strcmp(argv[i], "--port")) {
			linkPort = true;
		} else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
			keyPath = argv[++i];
		} else if (!strcmp(argv[i], "--gen") && i + 1 < argc) {
			generation = atoi(argv[++i]);
			if (generation != 1 && generation != 2) {
				return _usage();
			}
		} else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
			name = argv[++i];
		} else if (!strcmp(argv[i], "--trace") && i + 1 < argc) {
			tracePath = argv[++i];
		} else {
			return _usage();
		}
	}
	if (linkPort && gbPort) {
		return _usage();
	}
	if (tracePath) {
		sTrace = fopen(tracePath, "w");
		if (!sTrace) {
			printf("cannot write %s\n", tracePath);
			return 1;
		}
	}
#ifdef _WIN32
	SetConsoleCtrlHandler(_onCtrl, TRUE);
	timeBeginPeriod(1);
#endif

	// The GB-Link first: if it is missing there is no point resetting the board.
	char found[64];
	if (linkPort) {
		_note("no GB-Link: the console is on the board's own link port");
	} else if (!gbPort) {
		if (!Esp32SerialFindVendor(GBLINK_VENDOR, found, sizeof(found))) {
			_note("no GB-Link found (USB vendor id 2FE3): is it plugged in, with GBLink-Firmware 2.2.7 or later? (--gblink COMy names it)");
			return 1;
		}
		gbPort = found;
	}
	if (!linkPort) {
		sGbLink = Esp32SerialOpen(gbPort, 115200);
		if (!sGbLink) {
			_note("cannot open the GB-Link on %s (is GB-Link's web page or another program using it?)", gbPort);
			return 1;
		}
		_note("GB-Link on %s", gbPort);
	}

	struct UDSEsp32 esp;
	struct UDSEsp32Handlers handlers = {
		.log = _onLog, .gbState = _onState, .gbStats = _onStats, .gbPico = _onPico,
	};
	if (!udsEsp32Open(&esp, espPort, &handlers)) {
		_note("no ESP32 board found%s%s (plugged in through its native USB port? another program using it?)", espPort ? " on " : "",
		      espPort ? espPort : "");
		_closeGbLink();
		return 1;
	}
	_note("ESP32 board found; waiting for it to boot (up to 15 s)...");
	if (!udsEsp32WaitReady(&esp, 15000)) {
		_note("the board did not answer: it must run Azahar's esp32-uds-bridge firmware (1.7 or later)");
		udsEsp32Close(&esp);
		_closeGbLink();
		return 1;
	}
	const bool detects = esp.info.major > 1 || (esp.info.major == 1 && esp.info.minor >= 7);
	_note("board firmware %u.%u", esp.info.major, esp.info.minor);
	if (linkPort && !udsEsp32HasGbPort(&esp)) {
		_note("this firmware has no link port: flash esp32-uds-bridge 1.8 or later, built with CONFIG_UDS_LINK_PORT");
		udsEsp32Close(&esp);
		return 1;
	}
	if (!udsEsp32HasGbPico(&esp) || (!generation && !detects)) {
		_note(!udsEsp32HasGbPico(&esp) ? "this firmware cannot drive a GB-Link: flash esp32-uds-bridge 1.7 or later"
		                               : "firmware 1.6 cannot detect the generation: give --gen 1 or --gen 2, or flash 1.7");
		udsEsp32Close(&esp);
		_closeGbLink();
		return 1;
	}

	int present = udsEsp32KeyStatus(&esp, 1000);
	if (present == 0 && keyPath) {
		uint8_t key[16];
		enum UDSKeyStatus status = udsKeyFileLoad(keyPath, key);
		if (!udsKeyStatusOk(status) || !udsEsp32SetKey(&esp, UDS_ESP32_KEY_SLOT_DATA, key, 3000)) {
			_note("could not store the key: %s", udsKeyStatusOk(status) ? "the board did not store it" : udsKeyStatusText(status));
			memset(key, 0, sizeof(key));
			udsEsp32Close(&esp);
			_closeGbLink();
			return 1;
		}
		memset(key, 0, sizeof(key));
		present = 1;
		_note("stored the 3DS UDS key on the board");
	}
	if (present != 1) {
		_note(present < 0 ? "the board did not answer the key query"
		                  : "the board holds no 3DS UDS key: give --key <aes_keys.txt> once (or use uds-esp32-probe --store-key)");
		udsEsp32Close(&esp);
		_closeGbLink();
		return 1;
	}

	// Whatever the GB-Link had queued is from before; then the link port's voltage for a Game Boy.
	uint8_t buffer[256];
	if (sGbLink) {
		unsigned drainStart = _nowMs();
		while (_nowMs() - drainStart < 100 && Esp32SerialRead(sGbLink, buffer, sizeof(buffer)) >= 0) {
		}
		_picoCommand(PICO_CMD_SET_VOLTAGE_5V);
	}

	char title[UDS_ESP32_GB_TITLE] = {0};
	if (generation) {
		strcpy(title, generation == 2 ? "POKEMON_GLD" : "POKEMON RED"); // stands for the generation only
	}
	uint16_t name16[10] = {0};
	for (i = 0; i < 10 && name[i]; ++i) {
		name16[i] = (uint8_t) name[i];
	}
	int32_t result = linkPort ? udsEsp32GbStartPort(&esp, true, generation == 0, title, name16, 2000)
	                          : udsEsp32GbStart(&esp, true, true, generation == 0, title, name16, 2000);
	if (result != 0) {
		_note("the board's wrapper did not start: %s", linkPort && result == 0x103 // ESP_ERR_INVALID_STATE
		                                                   ? "the board was built without a link port (menuconfig > UDS bridge)"
		                                                   : udsEsp32GbStartText(result));
		udsEsp32Close(&esp);
		_closeGbLink();
		return 1;
	}
	_note("running: talk to the Cable Club receptionist on the Game Boy, and host a trade on the 3DS. Ctrl+C stops.");

	while (!sStop) {
		const unsigned sentBefore = sToGbLink;
		bool moved = false;
		if (!udsEsp32Poll(&esp)) {
			_note("the ESP32's port failed (unplugged?)");
			break;
		}
		int count = sGbLink ? Esp32SerialRead(sGbLink, buffer, 64) : 0;
		if (count < 0 || sGbLinkFailed) {
			_note("the GB-Link's port failed (unplugged?)");
			break;
		}
		moved = sToGbLink != sentBefore; // a byte just went to the GB-Link: its reply is a round trip away
		if (count > 0) {
			// The board parses the Pico's frames byte by byte, so they may go in any pieces (at most 64 bytes each).
			udsEsp32GbPico(&esp, buffer, (size_t) count);
			sFromGbLink += (unsigned) count;
			moved = true;
		}
#ifdef _WIN32
		Sleep(moved ? 0 : 1); // every millisecond counts: each clocked byte is a round trip through here
#endif
	}

	_note("stopping");
	udsEsp32GbStop(&esp, 500);
	_picoCommand(PICO_CMD_CANCEL);
	udsEsp32Close(&esp);
	_closeGbLink();
	if (sTrace) {
		fclose(sTrace);
	}
#ifdef _WIN32
	timeEndPeriod(1);
#endif
	return 0;
}
