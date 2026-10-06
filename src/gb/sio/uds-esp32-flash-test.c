/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Test of uds-esp32-flash.c, no hardware needed. A simulated ESP32-S3 stands in for the board: its USB Serial/JTAG reset logic (the
 * chip restarts when RTS is released after RTS high with DTR low, into download mode if DTR high with RTS low was seen first), and its
 * ROM loader (SLIP, SYNC answered several times, the commands the flasher uses with their sizes and checksums, an erase that only
 * covers the region, MD5 as 32 hex digits, four status bytes). By default the lines behave as through Windows' usbser.sys, which
 * sends an RTS change to the board only with the next DTR change (a restart that only moved RTS left a real board in download mode).
 * What it cannot show is the real hardware's timing and that its
 * reset logic really is this one: that takes a board. Build target: uds-esp32-flash-test.
 */
#include <mgba/internal/gb/sio/uds-esp32-flash.h>

#include <mgba-util/md5.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond, ...) \
	do { \
		++checks; \
		if (!(cond)) { \
			++failures; \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

#define FLASH_SIZE (4u << 20)

struct Sim {
	struct UDSFlashPort d;
	uint32_t now;
	bool dtr, rts;
	bool usbser; // RTS reaches the chip only when DTR is set (Windows' USB serial driver)
	bool pendingRts;
	bool bootRequest; // DTR high with RTS low since the last restart
	bool inReset;
	bool download; // the ROM loader is running (else the old firmware, which ignores all of this)
	bool disconnectOnReset; // the native USB port goes away when the chip restarts, until it is opened again
	bool disconnected;
	unsigned restarts;
	unsigned reopens;
	uint32_t magic;

	uint8_t out[1 << 16]; // to the host
	size_t outLength;
	size_t outPos;
	uint8_t frame[4096]; // from the host
	size_t frameLength;
	bool inFrame;
	bool escape;

	uint8_t* flash;
	bool begun;
	uint32_t beginSize, beginBlocks, beginBlockSize, beginOffset, nextSeq;
	bool attached, paramsSet;
	unsigned syncs;
	unsigned badChecksumOnce; // answer this block (1-based) with a checksum error once
	bool corruptAfterWrite; // a flash byte goes wrong after it is written (for the MD5 check)
	unsigned dataNacks;
};

static void _emit(struct Sim* sim, const uint8_t* bytes, size_t length) {
	if (sim->outLength + length <= sizeof(sim->out)) {
		memcpy(&sim->out[sim->outLength], bytes, length);
		sim->outLength += length;
	}
}

static void _respond(struct Sim* sim, uint8_t command, uint32_t value, const uint8_t* data, size_t dataLength, uint8_t status, uint8_t error) {
	uint8_t packet[64];
	size_t n = 0;
	packet[n++] = 0x01;
	packet[n++] = command;
	size_t bodyLength = dataLength + 4;
	packet[n++] = (uint8_t) bodyLength;
	packet[n++] = (uint8_t) (bodyLength >> 8);
	packet[n++] = (uint8_t) value;
	packet[n++] = (uint8_t) (value >> 8);
	packet[n++] = (uint8_t) (value >> 16);
	packet[n++] = (uint8_t) (value >> 24);
	memcpy(&packet[n], data, dataLength);
	n += dataLength;
	packet[n++] = status;
	packet[n++] = error;
	packet[n++] = 0;
	packet[n++] = 0;
	uint8_t slip[140];
	size_t size = udsFlashSlipEncode(packet, n, slip, sizeof(slip));
	_emit(sim, slip, size);
}

static uint32_t _u32(const uint8_t* p) {
	return p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void _handle(struct Sim* sim, const uint8_t* f, size_t length) {
	if (length < 8 || f[0] != 0x00) {
		return;
	}
	uint8_t command = f[1];
	size_t size = f[2] | (f[3] << 8);
	uint32_t checksum = _u32(&f[4]);
	const uint8_t* data = &f[8];
	if (8 + size != length) {
		_respond(sim, command, 0, NULL, 0, 1, 0x05);
		return;
	}
	switch (command) {
	case UDS_FLASH_SYNC: {
		++sim->syncs;
		int i;
		for (i = 0; i < 8; ++i) { // the ROM answers SYNC eight times
			_respond(sim, command, 0x20120707, NULL, 0, 0, 0);
		}
		return;
	}
	case UDS_FLASH_READ_REG:
		_respond(sim, command, size == 4 && _u32(data) == UDS_FLASH_MAGIC_REG ? sim->magic : 0, NULL, 0, size == 4 ? 0 : 1, 0x05);
		return;
	case UDS_FLASH_SPI_ATTACH:
		sim->attached = size == 8;
		_respond(sim, command, 0, NULL, 0, size == 8 ? 0 : 1, 0x05);
		return;
	case UDS_FLASH_SPI_SET_PARAMS:
		sim->paramsSet = size == 24 && _u32(&data[4]) == FLASH_SIZE && _u32(&data[12]) == 4096;
		_respond(sim, command, 0, NULL, 0, sim->paramsSet ? 0 : 1, 0x05);
		return;
	case UDS_FLASH_BEGIN: {
		if (size != 20 || !sim->attached) { // the S3's ROM loader takes five words
			_respond(sim, command, 0, NULL, 0, 1, 0x05);
			return;
		}
		sim->beginSize = _u32(&data[0]);
		sim->beginBlocks = _u32(&data[4]);
		sim->beginBlockSize = _u32(&data[8]);
		sim->beginOffset = _u32(&data[12]);
		uint32_t start = sim->beginOffset & ~4095u;
		uint32_t end = (sim->beginOffset + sim->beginSize + 4095u) & ~4095u;
		if (end > FLASH_SIZE) {
			_respond(sim, command, 0, NULL, 0, 1, 0x06);
			return;
		}
		memset(&sim->flash[start], 0xFF, end - start); // erases whole sectors
		sim->begun = true;
		sim->nextSeq = 0;
		_respond(sim, command, 0, NULL, 0, 0, 0);
		return;
	}
	case UDS_FLASH_DATA: {
		if (!sim->begun || size < 16) {
			_respond(sim, command, 0, NULL, 0, 1, 0x06);
			return;
		}
		uint32_t blockSize = _u32(&data[0]);
		uint32_t seq = _u32(&data[4]);
		const uint8_t* block = &data[16];
		if (blockSize != sim->beginBlockSize || size != 16 + blockSize || seq != sim->nextSeq) {
			_respond(sim, command, 0, NULL, 0, 1, 0x05);
			return;
		}
		if (udsFlashChecksum(block, blockSize) != (uint8_t) checksum || (sim->badChecksumOnce && seq + 1 == sim->badChecksumOnce)) {
			sim->badChecksumOnce = 0;
			++sim->dataNacks;
			_respond(sim, command, 0, NULL, 0, 1, 0x07);
			return;
		}
		uint32_t address = sim->beginOffset + seq * blockSize;
		if (address + blockSize <= FLASH_SIZE) {
			uint32_t i;
			for (i = 0; i < blockSize; ++i) {
				sim->flash[address + i] &= block[i]; // NOR flash: writing only clears bits, so an unerased sector shows
			}
		}
		if (sim->corruptAfterWrite && seq == 3) {
			sim->flash[address + 7] ^= 0x01;
		}
		++sim->nextSeq;
		_respond(sim, command, 0, NULL, 0, 0, 0);
		return;
	}
	case UDS_FLASH_SPI_MD5: {
		if (size != 16) {
			_respond(sim, command, 0, NULL, 0, 1, 0x05);
			return;
		}
		uint8_t digest[16];
		md5Buffer(&sim->flash[_u32(&data[0])], _u32(&data[4]), digest);
		char hex[33];
		int i;
		for (i = 0; i < 16; ++i) {
			snprintf(&hex[2 * i], 3, "%02x", digest[i]);
		}
		_respond(sim, command, 0, (const uint8_t*) hex, 32, 0, 0);
		return;
	}
	default:
		_respond(sim, command, 0, NULL, 0, 1, 0x05);
		return;
	}
}

static void _restart(struct Sim* sim) {
	++sim->restarts;
	sim->download = sim->bootRequest;
	sim->bootRequest = false;
	sim->begun = sim->attached = sim->paramsSet = false;
	sim->outLength = sim->outPos = 0;
	sim->frameLength = 0;
	sim->inFrame = sim->escape = false;
	if (sim->disconnectOnReset) {
		sim->disconnected = true;
	}
	if (sim->download) {
		static const char boot[] = "ESP-ROM:esp32s3-20210327\r\nrst:0x15 (USB_UART_CHIP_RESET),boot:0x0 (DOWNLOAD(USB/UART0))\r\nwaiting for download\r\n";
		_emit(sim, (const uint8_t*) boot, strlen(boot));
	}
}

static void _lines(struct Sim* sim) {
	if (sim->dtr && !sim->rts) {
		sim->bootRequest = true;
	}
	bool reset = sim->rts && !sim->dtr;
	if (sim->inReset && !reset) {
		sim->inReset = false;
		_restart(sim);
	} else if (reset) {
		sim->inReset = true;
	}
}

static bool _simWrite(void* context, const uint8_t* data, size_t length) {
	struct Sim* sim = context;
	if (sim->disconnected) {
		return false;
	}
	if (!sim->download) {
		return true; // the firmware swallows it
	}
	size_t i;
	for (i = 0; i < length; ++i) {
		uint8_t byte = data[i];
		if (byte == 0xC0) {
			if (sim->inFrame && sim->frameLength) {
				_handle(sim, sim->frame, sim->frameLength);
				sim->inFrame = false;
			} else {
				sim->inFrame = true;
			}
			sim->frameLength = 0;
			continue;
		}
		if (!sim->inFrame) {
			continue;
		}
		if (sim->escape) {
			byte = byte == 0xDC ? 0xC0 : 0xDB;
			sim->escape = false;
		} else if (byte == 0xDB) {
			sim->escape = true;
			continue;
		}
		if (sim->frameLength < sizeof(sim->frame)) {
			sim->frame[sim->frameLength++] = byte;
		}
	}
	return true;
}

static int _simRead(void* context, uint8_t* buffer, size_t capacity, unsigned timeoutMs) {
	struct Sim* sim = context;
	if (sim->disconnected) {
		return -1;
	}
	if (sim->outPos >= sim->outLength) {
		sim->now += timeoutMs; // nothing came
		return 0;
	}
	size_t n = sim->outLength - sim->outPos;
	if (n > capacity) {
		n = capacity;
	}
	if (n > 37) {
		n = 37; // arrive in pieces, splitting frames
	}
	memcpy(buffer, &sim->out[sim->outPos], n);
	sim->outPos += n;
	return (int) n;
}

static void _simDtr(void* context, bool level) {
	struct Sim* sim = context;
	sim->dtr = level;
	if (sim->usbser) {
		sim->rts = sim->pendingRts;
	}
	_lines(sim);
}

static void _simRts(void* context, bool level) {
	struct Sim* sim = context;
	sim->pendingRts = level;
	if (!sim->usbser) {
		sim->rts = level;
		_lines(sim);
	}
}

static void _simSleep(void* context, unsigned ms) {
	struct Sim* sim = context;
	sim->now += ms;
}

static uint32_t _simNow(void* context) {
	struct Sim* sim = context;
	return sim->now;
}

static bool _simReopen(void* context) {
	struct Sim* sim = context;
	++sim->reopens;
	sim->disconnected = false;
	sim->now += 500;
	sim->dtr = sim->rts = sim->pendingRts = false; // a fresh handle starts with both lines low
	return true;
}

static void _simInit(struct Sim* sim) {
	memset(sim, 0, sizeof(*sim));
	sim->flash = malloc(FLASH_SIZE);
	memset(sim->flash, 0xA5, FLASH_SIZE); // the old contents
	sim->magic = UDS_FLASH_CHIP_ESP32S3;
	sim->usbser = true;
	sim->d.context = sim;
	sim->d.write = _simWrite;
	sim->d.read = _simRead;
	sim->d.setDtr = _simDtr;
	sim->d.setRts = _simRts;
	sim->d.sleepMs = _simSleep;
	sim->d.nowMs = _simNow;
	sim->d.reopen = _simReopen;
}

// A release image in miniature: the bootloader header (chip 9, 4 MB), the partition table and an app with its description, and data
// full of the bytes SLIP has to escape.
#define IMAGE_SIZE (0x10000 + 0x2345)
static uint8_t* _makeImage(void) {
	uint8_t* image = malloc(IMAGE_SIZE);
	uint32_t x = 12345;
	size_t i;
	for (i = 0; i < IMAGE_SIZE; ++i) {
		x = x * 1103515245u + 12345u;
		uint8_t byte = (uint8_t) (x >> 16);
		image[i] = (i % 7 == 0) ? 0xC0 : (i % 11 == 0) ? 0xDB : byte;
	}
	memset(image, 0, 24);
	image[0] = 0xE9;
	image[3] = 0x2F; // 4 MB, 80 MHz
	image[12] = UDS_FLASH_CHIP_ESP32S3;
	memset(&image[0x8000], 0xFF, 0x1000);
	static const uint8_t table[3][32] = {
		{0xAA, 0x50, 0x01, 0x02, 0x00, 0x90, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 'n', 'v', 's'},
		{0xAA, 0x50, 0x01, 0x01, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 'p', 'h', 'y'},
		{0xAA, 0x50, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x10, 0x00, 'f', 'a', 'c', 't', 'o', 'r', 'y'},
	};
	memcpy(&image[0x8000], table, sizeof(table));
	memset(&image[0x9000], 0xFF, 0x7000); // the gaps merge_bin fills (the key store among them)
	memset(&image[0x10000], 0, 0x100); // the header and esp_app_desc_t, whose text fields are zero padded
	image[0x10000] = 0xE9;
	image[0x10000 + 12] = UDS_FLASH_CHIP_ESP32S3;
	uint8_t* desc = &image[0x10020];
	desc[0] = 0x32;
	desc[1] = 0x54;
	desc[2] = 0xCD;
	desc[3] = 0xAB;
	memcpy(desc + 0x10, "release_v2.2-11-g2f506fcff", 26);
	memcpy(desc + 0x30, "esp32_uds_bridge", 16);
	memcpy(desc + 0x50, "11:59:56", 8);
	memcpy(desc + 0x60, "Oct  6 2026", 11);
	return image;
}

static unsigned sProgressCalls;
static size_t sProgressLast;
static void _progress(void* context, size_t done, size_t total) {
	(void) context;
	(void) total;
	++sProgressCalls;
	sProgressLast = done;
}
static void _log(void* context, const char* text) {
	(void) context;
	(void) text;
}
static const struct UDSFlashHandlers kHandlers = {NULL, _log, _progress};

static void testImageCheck(void) {
	uint8_t* image = _makeImage();
	struct UDSFlashImageInfo info;
	char why[300];
	CHECK(udsFlashCheckImage(image, IMAGE_SIZE, &info, why, sizeof(why)), "a release image is accepted (%s)", why);
	CHECK(info.flashSize == FLASH_SIZE && info.appOffset == 0x10000, "flash size %u, app at %X", (unsigned) info.flashSize, (unsigned) info.appOffset);
	CHECK(!strcmp(info.project, "esp32_uds_bridge") && !strcmp(info.version, "release_v2.2-11-g2f506fcff") && !strcmp(info.date, "Oct  6 2026"),
	      "the app's description is read: %s %s %s", info.project, info.version, info.date);
	CHECK(!udsFlashCheckImage(&image[0x10000], IMAGE_SIZE - 0x10000, &info, why, sizeof(why)) && strstr(why, "app on its own"),
	      "the app alone (as in a build folder) is refused: %s", why);
	image[12] = 5;
	CHECK(!udsFlashCheckImage(image, IMAGE_SIZE, &info, why, sizeof(why)) && strstr(why, "another chip"), "another chip's image is refused: %s", why);
	image[12] = UDS_FLASH_CHIP_ESP32S3;
	image[0] = 0x00;
	CHECK(!udsFlashCheckImage(image, IMAGE_SIZE, &info, why, sizeof(why)), "a file that is not an image is refused");
	image[0] = 0xE9;
	image[3] = 0x0F; // 1 MB
	CHECK(udsFlashCheckImage(image, IMAGE_SIZE, &info, why, sizeof(why)) && info.flashSize == (1u << 20), "the flash size comes from the header");
	free(image);
}

static void testSlip(void) {
	uint8_t data[] = {1, 0xC0, 2, 0xDB, 3};
	uint8_t out[20];
	size_t n = udsFlashSlipEncode(data, sizeof(data), out, sizeof(out));
	static const uint8_t expected[] = {0xC0, 1, 0xDB, 0xDC, 2, 0xDB, 0xDD, 3, 0xC0};
	CHECK(n == sizeof(expected) && !memcmp(out, expected, n), "SLIP escapes C0 and DB");
	CHECK(udsFlashChecksum(data, sizeof(data)) == (uint8_t) (0xEF ^ 1 ^ 0xC0 ^ 2 ^ 0xDB ^ 3), "the checksum is seeded with EF");
}

// From the old firmware (running, ignoring the loader's bytes): reset into download mode, write, verify, restart into the firmware.
static void testFlash(bool disconnect, unsigned nackBlock, bool usbser) {
	struct Sim sim;
	_simInit(&sim);
	sim.usbser = usbser;
	sim.disconnectOnReset = disconnect;
	sim.badChecksumOnce = nackBlock;
	uint8_t* image = _makeImage();
	struct UDSFlashOptions options = {true};
	char error[300] = "";
	sProgressCalls = 0;
	sProgressLast = 0;
	bool ok = udsFlashWriteImage(&sim.d, &kHandlers, &options, image, IMAGE_SIZE, error, sizeof(error));
	CHECK(ok, "the image is written (%s, disconnect %d)", error, disconnect);
	CHECK(!memcmp(sim.flash, image, IMAGE_SIZE), "the flash holds the image");
	size_t tail = (IMAGE_SIZE + 4095u) & ~4095u;
	bool tailErased = true;
	size_t i;
	for (i = IMAGE_SIZE; i < tail; ++i) {
		tailErased = tailErased && sim.flash[i] == 0xFF;
	}
	CHECK(tailErased, "the rest of the last sector is erased (the last block is padded with FF)");
	CHECK(sim.flash[tail] == 0xA5 && sim.flash[FLASH_SIZE - 1] == 0xA5, "nothing past the image's last sector is touched");
	CHECK(sim.restarts == 2 && !sim.download, "two restarts: into download mode, then into the new firmware (%u, download %d, usbser %d)",
	      sim.restarts, sim.download, usbser);
	CHECK(!disconnect || sim.reopens >= 1, "the port is opened again after the chip restarts (%u)", sim.reopens);
	CHECK(sProgressCalls == (IMAGE_SIZE + UDS_FLASH_BLOCK - 1) / UDS_FLASH_BLOCK && sProgressLast == IMAGE_SIZE, "progress: %u calls, last %u",
	      sProgressCalls, (unsigned) sProgressLast);
	CHECK(!nackBlock || sim.dataNacks == 1, "a refused block is sent again (%u refusals)", sim.dataNacks);
	free(image);
	free(sim.flash);
}

static void testAlreadyInDownloadMode(void) {
	struct Sim sim;
	_simInit(&sim);
	sim.download = true; // BOOT held while plugging in
	uint8_t* image = _makeImage();
	struct UDSFlashOptions options = {true};
	char error[300] = "";
	CHECK(udsFlashWriteImage(&sim.d, &kHandlers, &options, image, IMAGE_SIZE, error, sizeof(error)), "written (%s)", error);
	CHECK(sim.restarts == 1 && !sim.download, "only the final restart, into the firmware: the loader was already there (%u)", sim.restarts);

	// A board left in download mode (as the first version of the tool left one) is restarted into its firmware.
	sim.download = true;
	udsFlashRestart(&sim.d, &options);
	CHECK(sim.restarts == 2 && !sim.download, "udsFlashRestart starts the firmware (%u, download %d)", sim.restarts, sim.download);
	free(image);
	free(sim.flash);
}

static void _ignoreLine(void* context, bool level) {
	(void) context;
	(void) level;
}

static void testFailures(void) {
	struct Sim sim;
	uint8_t* image = _makeImage();
	struct UDSFlashOptions options = {true};
	char error[300];

	_simInit(&sim);
	sim.magic = 0x00F01D83; // an original ESP32
	error[0] = 0;
	CHECK(!udsFlashWriteImage(&sim.d, &kHandlers, &options, image, IMAGE_SIZE, error, sizeof(error)) && strstr(error, "not an ESP32-S3"),
	      "another chip is refused before anything is erased: %s", error);
	CHECK(sim.flash[0] == 0xA5, "and its flash is untouched");
	free(sim.flash);

	_simInit(&sim);
	sim.corruptAfterWrite = true;
	error[0] = 0;
	CHECK(!udsFlashWriteImage(&sim.d, &kHandlers, &options, image, IMAGE_SIZE, error, sizeof(error)) && strstr(error, "MD5"),
	      "a flash that does not match is reported: %s", error);
	free(sim.flash);

	// A board that never enters download mode (the reset lines do nothing).
	_simInit(&sim);
	sim.d.setDtr = _ignoreLine;
	sim.d.setRts = _ignoreLine;
	error[0] = 0;
	CHECK(!udsFlashWriteImage(&sim.d, &kHandlers, &options, image, IMAGE_SIZE, error, sizeof(error)) && strstr(error, "BOOT"),
	      "no download mode: the user is told to hold BOOT: %s", error);
	free(sim.flash);
	free(image);
}

// UDS_FLASH_TEST_IMAGE=<file> also checks a real file and prints what it holds (a release image passes, an app-only file does not).
static void testRealImage(const char* path) {
	FILE* file = fopen(path, "rb");
	if (!file) {
		CHECK(false, "cannot read %s", path);
		return;
	}
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	uint8_t* image = malloc((size_t) size);
	bool read = fread(image, 1, (size_t) size, file) == (size_t) size;
	fclose(file);
	struct UDSFlashImageInfo info;
	char why[300] = "";
	bool ok = read && udsFlashCheckImage(image, (size_t) size, &info, why, sizeof(why));
	printf("%s: %s", path, ok ? "a release image" : why);
	if (ok) {
		printf(" (%s %s, %s %s; app at %X, %u MB flash)", info.project, info.version, info.date, info.time, (unsigned) info.appOffset,
		       (unsigned) (info.flashSize >> 20));
	}
	printf("\n");
	free(image);
}

int main(void) {
	const char* real = getenv("UDS_FLASH_TEST_IMAGE");
	if (real && *real) {
		testRealImage(real);
	}
	testSlip();
	testImageCheck();
	testFlash(false, 0, true);
	testFlash(true, 0, true);
	testFlash(false, 5, true);
	testFlash(false, 0, false); // lines that change on their own (another driver, or a USB-to-UART bridge's)
	testAlreadyInDownloadMode();
	testFailures();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
