/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-esp32-flash.h>

#include <mgba-util/md5.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define SLIP_END 0xC0
#define SLIP_ESC 0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

#define REQUEST 0x00
#define RESPONSE 0x01
#define HEADER 8 // direction, command, size:u16, checksum or value:u32

#define SYNC_MS 100 // per SYNC attempt, as esptool
#define COMMAND_MS 3000
#define ERASE_MS 40000 // FLASH_BEGIN erases the whole region before it answers (esptool allows 30 s per MB)
#define DATA_MS 5000
#define MD5_MS 15000
#define BLOCK_ATTEMPTS 3

uint8_t udsFlashChecksum(const uint8_t* data, size_t length) {
	uint8_t sum = 0xEF;
	size_t i;
	for (i = 0; i < length; ++i) {
		sum ^= data[i];
	}
	return sum;
}

size_t udsFlashSlipEncode(const uint8_t* data, size_t length, uint8_t* out, size_t capacity) {
	size_t n = 0;
	size_t i;
	if (capacity < 2) {
		return 0;
	}
	out[n++] = SLIP_END;
	for (i = 0; i < length; ++i) {
		if (n + 3 > capacity) {
			return 0;
		}
		if (data[i] == SLIP_END) {
			out[n++] = SLIP_ESC;
			out[n++] = SLIP_ESC_END;
		} else if (data[i] == SLIP_ESC) {
			out[n++] = SLIP_ESC;
			out[n++] = SLIP_ESC_ESC;
		} else {
			out[n++] = data[i];
		}
	}
	out[n++] = SLIP_END;
	return n;
}

static void _put32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
	p[2] = (uint8_t) (v >> 16);
	p[3] = (uint8_t) (v >> 24);
}

static uint32_t _get32(const uint8_t* p) {
	return p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void _copyText(char* out, size_t capacity, const uint8_t* field, size_t length) {
	size_t i;
	for (i = 0; i < length && i + 1 < capacity && field[i]; ++i) {
		out[i] = field[i] >= 0x20 && field[i] < 0x7F ? (char) field[i] : '?';
	}
	out[i] = 0;
}

// The image ---------------------------------------------------------------------------------------------------------------------

#define IMAGE_MAGIC 0xE9
#define PARTITION_TABLE 0x8000
#define APP_DESC 0x20 // esp_app_desc_t, after the image header (24 bytes) and the first segment's header (8)
#define APP_DESC_MAGIC 0xABCD5432u

static bool _why(char* why, size_t capacity, const char* text) {
	if (why && capacity) {
		snprintf(why, capacity, "%s", text);
	}
	return false;
}

bool udsFlashCheckImage(const uint8_t* image, size_t size, struct UDSFlashImageInfo* info, char* why, size_t whyCapacity) {
	memset(info, 0, sizeof(*info));
	if (size < 24 || image[0] != IMAGE_MAGIC) {
		return _why(why, whyCapacity, "this is not an ESP32 firmware image");
	}
	unsigned chip = image[12] | (image[13] << 8);
	if (chip != UDS_FLASH_CHIP_ESP32S3) {
		char text[160];
		snprintf(text, sizeof(text), "this image is built for another chip (id %u); the board is an ESP32-S3 (id %u)", chip, UDS_FLASH_CHIP_ESP32S3);
		return _why(why, whyCapacity, text);
	}
	if (size < PARTITION_TABLE + 32 || image[PARTITION_TABLE] != 0xAA || image[PARTITION_TABLE + 1] != 0x50) {
		return _why(why, whyCapacity, "this is the app on its own (as in a build folder), not a release image: use the merged "
		                              "esp32-uds-bridge-fw<version>-esp32s3.bin, which also holds the bootloader and the partition table");
	}
	static const size_t kSizes[] = {1, 2, 4, 8, 16, 32, 64, 128};
	unsigned sizeCode = image[3] >> 4;
	if (sizeCode >= sizeof(kSizes) / sizeof(kSizes[0])) {
		return _why(why, whyCapacity, "the bootloader header names an unknown flash size");
	}
	info->flashSize = kSizes[sizeCode] << 20;
	if (size > info->flashSize) {
		return _why(why, whyCapacity, "the image is larger than the flash it is built for");
	}
	// The app: the factory partition, else the first app partition.
	size_t offset;
	bool found = false;
	for (offset = PARTITION_TABLE; offset + 32 <= size && offset < PARTITION_TABLE + 0xC00; offset += 32) {
		const uint8_t* entry = &image[offset];
		if (entry[0] != 0xAA || entry[1] != 0x50) {
			break;
		}
		if (entry[2] == 0x00 && (!found || entry[3] == 0x00)) {
			info->appOffset = _get32(&entry[4]);
			found = true;
			if (entry[3] == 0x00) {
				break;
			}
		}
	}
	if (!found || info->appOffset + APP_DESC + 0x80 > size || image[info->appOffset] != IMAGE_MAGIC) {
		return _why(why, whyCapacity, "the image has no app where its partition table says");
	}
	const uint8_t* desc = &image[info->appOffset + APP_DESC];
	if (_get32(desc) == APP_DESC_MAGIC) {
		_copyText(info->version, sizeof(info->version), desc + 0x10, 32);
		_copyText(info->project, sizeof(info->project), desc + 0x30, 32);
		_copyText(info->time, sizeof(info->time), desc + 0x50, 16);
		_copyText(info->date, sizeof(info->date), desc + 0x60, 16);
	}
	return true;
}

// The ROM loader ----------------------------------------------------------------------------------------------------------------

struct Loader {
	const struct UDSFlashPort* port;
	const struct UDSFlashHandlers* handlers;
	char* error;
	size_t errorCapacity;
	bool portFailed;
	bool dtr; // the DTR level last set (see _setRts)
	// What has been read but not decoded yet, and the frame being decoded.
	uint8_t in[512];
	size_t inPos;
	size_t inLength;
	uint8_t frame[1100];
	size_t frameLength;
	bool inFrame;
	bool escape;
	bool overlong;
};

static void _log(struct Loader* loader, const char* format, ...) {
	if (!loader->handlers || !loader->handlers->log) {
		return;
	}
	char text[300];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	loader->handlers->log(loader->handlers->context, text);
}

static bool _fail(struct Loader* loader, const char* format, ...) {
	if (loader->error && loader->errorCapacity) {
		va_list args;
		va_start(args, format);
		vsnprintf(loader->error, loader->errorCapacity, format, args);
		va_end(args);
	}
	return false;
}

static void _resetDecoder(struct Loader* loader) {
	loader->inPos = loader->inLength = 0;
	loader->frameLength = 0;
	loader->inFrame = loader->escape = loader->overlong = false;
}

// The next whole SLIP frame within timeoutMs. Text the ROM prints between frames (its boot message) is skipped.
static bool _readFrame(struct Loader* loader, unsigned timeoutMs) {
	const struct UDSFlashPort* port = loader->port;
	uint32_t start = port->nowMs(port->context);
	for (;;) {
		while (loader->inPos < loader->inLength) {
			uint8_t byte = loader->in[loader->inPos++];
			if (byte == SLIP_END) {
				if (loader->inFrame && loader->frameLength && !loader->overlong) {
					loader->inFrame = false;
					return true;
				}
				// An empty frame, or the start of one.
				loader->inFrame = true;
				loader->frameLength = 0;
				loader->escape = loader->overlong = false;
				continue;
			}
			if (!loader->inFrame) {
				continue;
			}
			if (loader->escape) {
				byte = byte == SLIP_ESC_END ? SLIP_END : byte == SLIP_ESC_ESC ? SLIP_ESC : byte;
				loader->escape = false;
			} else if (byte == SLIP_ESC) {
				loader->escape = true;
				continue;
			}
			if (loader->frameLength < sizeof(loader->frame)) {
				loader->frame[loader->frameLength++] = byte;
			} else {
				loader->overlong = true;
			}
		}
		uint32_t elapsed = port->nowMs(port->context) - start;
		if (elapsed >= timeoutMs) {
			return false;
		}
		int got = port->read(port->context, loader->in, sizeof(loader->in), timeoutMs - elapsed);
		if (got < 0) {
			loader->portFailed = true;
			return false;
		}
		loader->inPos = 0;
		loader->inLength = (size_t) got;
	}
}

static bool _send(struct Loader* loader, uint8_t command, const uint8_t* data, size_t length, uint32_t checksum) {
	uint8_t packet[HEADER + 16 + UDS_FLASH_BLOCK];
	uint8_t slip[2 * sizeof(packet) + 2];
	if (length > sizeof(packet) - HEADER) {
		return false;
	}
	packet[0] = REQUEST;
	packet[1] = command;
	packet[2] = (uint8_t) length;
	packet[3] = (uint8_t) (length >> 8);
	_put32(&packet[4], checksum);
	if (length) {
		memcpy(&packet[HEADER], data, length);
	}
	size_t size = udsFlashSlipEncode(packet, HEADER + length, slip, sizeof(slip));
	if (!size || !loader->port->write(loader->port->context, slip, size)) {
		loader->portFailed = true;
		return false;
	}
	return true;
}

static const char* _romError(uint8_t code) {
	switch (code) {
	case 0x05:
		return "the message was invalid";
	case 0x06:
		return "the loader could not act on the message";
	case 0x07:
		return "the message's checksum was wrong";
	case 0x08:
		return "flash write error";
	case 0x09:
		return "flash read error";
	case 0x0A:
		return "flash read length error";
	default:
		return "unknown error";
	}
}

// Sends a command and waits for its answer: the value field, and the body (the status bytes, after any data). The ROM loader's
// body ends with four status bytes (status, error, 0, 0); the status is the first byte after the command's own data.
static bool _command(struct Loader* loader, uint8_t command, const uint8_t* data, size_t length, uint32_t checksum, unsigned timeoutMs,
                     size_t dataBytes, uint32_t* value, uint8_t* body, size_t bodyCapacity) {
	if (!_send(loader, command, data, length, checksum)) {
		return _fail(loader, "the serial port failed while sending to the board");
	}
	const struct UDSFlashPort* port = loader->port;
	uint32_t start = port->nowMs(port->context);
	for (;;) {
		uint32_t elapsed = port->nowMs(port->context) - start;
		if (elapsed >= timeoutMs || !_readFrame(loader, timeoutMs - elapsed)) {
			return _fail(loader, loader->portFailed ? "the serial port failed while waiting for the board" : "the board did not answer command %02X", command);
		}
		const uint8_t* f = loader->frame;
		size_t size = loader->frameLength;
		if (size < HEADER || f[0] != RESPONSE || f[1] != command) {
			continue; // not this command's answer (a late SYNC answer, for one)
		}
		size_t bodyLength = f[2] | (f[3] << 8);
		if (HEADER + bodyLength > size || bodyLength < dataBytes + 2) {
			return _fail(loader, "the board's answer to command %02X is malformed", command);
		}
		const uint8_t* bodyBytes = &f[HEADER];
		if (bodyBytes[dataBytes] != 0) {
			return _fail(loader, "the board refused command %02X: %s (%02X)", command, _romError(bodyBytes[dataBytes + 1]), bodyBytes[dataBytes + 1]);
		}
		if (value) {
			*value = _get32(&f[4]);
		}
		if (body && bodyCapacity) {
			memcpy(body, bodyBytes, bodyLength < bodyCapacity ? bodyLength : bodyCapacity);
		}
		return true;
	}
}

static bool _sync(struct Loader* loader) {
	uint8_t data[36] = {0x07, 0x07, 0x12, 0x20};
	memset(&data[4], 0x55, 32);
	if (!_send(loader, UDS_FLASH_SYNC, data, sizeof(data), 0)) {
		return false;
	}
	const struct UDSFlashPort* port = loader->port;
	uint32_t start = port->nowMs(port->context);
	while (port->nowMs(port->context) - start < SYNC_MS) {
		uint32_t elapsed = port->nowMs(port->context) - start;
		if (!_readFrame(loader, SYNC_MS - elapsed)) {
			return false;
		}
		if (loader->frameLength >= HEADER && loader->frame[0] == RESPONSE && loader->frame[1] == UDS_FLASH_SYNC) {
			// The loader answers a SYNC several times over: let the rest arrive and drop them.
			while (_readFrame(loader, SYNC_MS)) {
			}
			return !loader->portFailed;
		}
	}
	return false;
}

// The line changes go through these. Windows' USB serial driver (usbser.sys) sends the board an RTS change only together with a DTR
// change, so every RTS change is followed by setting DTR again to its current level, as esptool does: without it, a reset that only
// moves RTS (the restart into the firmware) never reaches the chip.
static void _setDtr(struct Loader* loader, bool level) {
	loader->dtr = level;
	loader->port->setDtr(loader->port->context, level);
}

static void _setRts(struct Loader* loader, bool level) {
	loader->port->setRts(loader->port->context, level);
	loader->port->setDtr(loader->port->context, loader->dtr);
}

static void _pause(struct Loader* loader, unsigned ms) {
	loader->port->sleepMs(loader->port->context, ms);
}

// esptool's USBJTAGSerialReset: the S3's USB Serial/JTAG peripheral turns these DTR/RTS changes into "reset, with GPIO0 low".
static void _resetUsbJtag(struct Loader* loader) {
	_setRts(loader, false);
	_setDtr(loader, false); // idle
	_pause(loader, 100);
	_setDtr(loader, true); // GPIO0 low
	_setRts(loader, false);
	_pause(loader, 100);
	_setRts(loader, true); // reset, going through (1,1) rather than (0,0)
	_setDtr(loader, false);
	_setRts(loader, true);
	_pause(loader, 100);
	_setDtr(loader, false);
	_setRts(loader, false); // out of reset
}

// esptool's ClassicReset, for a board whose USB-to-UART bridge drives EN and GPIO0 through two transistors.
static void _resetClassic(struct Loader* loader) {
	_setDtr(loader, false);
	_setRts(loader, true); // EN low
	_pause(loader, 100);
	_setDtr(loader, true); // GPIO0 low
	_setRts(loader, false); // EN high
	_pause(loader, 50);
	_setDtr(loader, false); // GPIO0 high again
}

// esptool's HardReset: GPIO0 left high, so the chip starts the firmware.
static void _hardReset(struct Loader* loader, bool usbJtag) {
	_setDtr(loader, false);
	_setRts(loader, true); // reset
	_pause(loader, usbJtag ? 200 : 100);
	_setRts(loader, false);
	if (usbJtag) {
		_pause(loader, 200);
	}
}

static bool _reconnect(struct Loader* loader) {
	_resetDecoder(loader);
	loader->portFailed = false;
	loader->dtr = false; // a port opened again starts with both lines low
	if (!loader->port->reopen(loader->port->context)) {
		loader->portFailed = true;
		return false;
	}
	return true;
}

static bool _enterLoader(struct Loader* loader, const struct UDSFlashOptions* options) {
	const struct UDSFlashPort* port = loader->port;
	// Already there (BOOT held while the board was plugged in)?
	if (_sync(loader)) {
		return true;
	}
	int attempt;
	for (attempt = 0; attempt < 3; ++attempt) {
		if (loader->portFailed && !_reconnect(loader)) {
			continue;
		}
		_log(loader, "Restarting the board into its download mode%s", attempt ? " (again)" : "");
		if (options && options->usbJtag) {
			_resetUsbJtag(loader);
		} else {
			_resetClassic(loader);
		}
		_resetDecoder(loader);
		int tries;
		for (tries = 0; tries < 20; ++tries) {
			if (_sync(loader)) {
				return true;
			}
			if (loader->portFailed) {
				// The native USB port goes away while the chip restarts: open it again once it is back.
				port->sleepMs(port->context, 300);
				if (!_reconnect(loader)) {
					break;
				}
			}
		}
	}
	return _fail(loader, "the board's download mode did not answer. Hold its BOOT button while plugging it in (or press RESET while "
	                     "holding BOOT), then try again");
}

bool udsFlashWriteImage(const struct UDSFlashPort* port, const struct UDSFlashHandlers* handlers, const struct UDSFlashOptions* options,
                        const uint8_t* image, size_t size, char* error, size_t errorCapacity) {
	struct Loader* loader = calloc(1, sizeof(*loader));
	if (!loader) {
		snprintf(error, errorCapacity, "out of memory");
		return false;
	}
	loader->port = port;
	loader->handlers = handlers;
	loader->error = error;
	loader->errorCapacity = errorCapacity;
	bool ok = false;
	struct UDSFlashImageInfo info;
	uint8_t data[16 + UDS_FLASH_BLOCK];
	uint32_t value = 0;

	if (!udsFlashCheckImage(image, size, &info, error, errorCapacity)) {
		goto done;
	}
	_log(loader, "Connecting to the board's ROM loader");
	if (!_enterLoader(loader, options)) {
		goto done;
	}
	_put32(data, UDS_FLASH_MAGIC_REG);
	if (!_command(loader, UDS_FLASH_READ_REG, data, 4, 0, COMMAND_MS, 0, &value, NULL, 0)) {
		goto done;
	}
	if (value != UDS_FLASH_CHIP_ESP32S3) {
		_fail(loader, "the board is not an ESP32-S3 (its chip magic is %08X); this firmware is for the S3 only", (unsigned) value);
		goto done;
	}
	_log(loader, "Found an ESP32-S3");

	memset(data, 0, 8); // SPI_ATTACH: the default SPI pins
	if (!_command(loader, UDS_FLASH_SPI_ATTACH, data, 8, 0, COMMAND_MS, 0, NULL, NULL, 0)) {
		goto done;
	}
	_put32(&data[0], 0); // flash id
	_put32(&data[4], (uint32_t) info.flashSize);
	_put32(&data[8], 64 * 1024); // block
	_put32(&data[12], 4 * 1024); // sector
	_put32(&data[16], 256); // page
	_put32(&data[20], 0xFFFF); // status mask
	if (!_command(loader, UDS_FLASH_SPI_SET_PARAMS, data, 24, 0, COMMAND_MS, 0, NULL, NULL, 0)) {
		goto done;
	}

	uint32_t blocks = (uint32_t) ((size + UDS_FLASH_BLOCK - 1) / UDS_FLASH_BLOCK);
	_put32(&data[0], (uint32_t) size); // erase size
	_put32(&data[4], blocks);
	_put32(&data[8], UDS_FLASH_BLOCK);
	_put32(&data[12], 0); // offset
	_put32(&data[16], 0); // not encrypted (the S3's ROM loader takes this fifth word)
	_log(loader, "Erasing %u KB of flash (a few seconds)", (unsigned) ((size + 1023) / 1024));
	if (!_command(loader, UDS_FLASH_BEGIN, data, 20, 0, ERASE_MS, 0, NULL, NULL, 0)) {
		goto done;
	}

	_log(loader, "Writing %u blocks", (unsigned) blocks);
	uint32_t seq;
	for (seq = 0; seq < blocks; ++seq) {
		size_t offset = (size_t) seq * UDS_FLASH_BLOCK;
		size_t chunk = size - offset < UDS_FLASH_BLOCK ? size - offset : UDS_FLASH_BLOCK;
		uint8_t* block = &data[16];
		memcpy(block, &image[offset], chunk);
		memset(block + chunk, 0xFF, UDS_FLASH_BLOCK - chunk);
		_put32(&data[0], UDS_FLASH_BLOCK);
		_put32(&data[4], seq);
		_put32(&data[8], 0);
		_put32(&data[12], 0);
		int attempt;
		bool written = false;
		for (attempt = 0; attempt < BLOCK_ATTEMPTS && !written; ++attempt) {
			written = _command(loader, UDS_FLASH_DATA, data, 16 + UDS_FLASH_BLOCK, udsFlashChecksum(block, UDS_FLASH_BLOCK), DATA_MS, 0,
			                   NULL, NULL, 0);
			if (!written && loader->portFailed) {
				break;
			}
		}
		if (!written) {
			char reason[200];
			snprintf(reason, sizeof(reason), "%s", error);
			_fail(loader, "writing block %u of %u failed: %s. The board has no working firmware until it is flashed again", (unsigned) seq + 1,
			      (unsigned) blocks, reason);
			goto done;
		}
		if (handlers && handlers->progress) {
			handlers->progress(handlers->context, offset + chunk, size);
		}
	}

	_log(loader, "Verifying");
	_put32(&data[0], 0);
	_put32(&data[4], (uint32_t) size);
	_put32(&data[8], 0);
	_put32(&data[12], 0);
	uint8_t md5Body[40];
	if (!_command(loader, UDS_FLASH_SPI_MD5, data, 16, 0, MD5_MS, 32, NULL, md5Body, sizeof(md5Body))) {
		goto done;
	}
	uint8_t digest[16];
	md5Buffer(image, size, digest);
	char expected[33];
	int i;
	for (i = 0; i < 16; ++i) {
		snprintf(&expected[i * 2], 3, "%02x", digest[i]);
	}
	for (i = 0; i < 32; ++i) {
		char got = (char) md5Body[i];
		if (got >= 'A' && got <= 'F') {
			got = (char) (got - 'A' + 'a');
		}
		if (got != expected[i]) {
			_fail(loader, "the flash does not match the file after writing (MD5 differs). Try again; the board has no working firmware until it is flashed");
			goto done;
		}
	}

	_log(loader, "Written and verified. Restarting the board into the new firmware");
	_hardReset(loader, options && options->usbJtag);
	ok = true;

done:
	memset(data, 0, sizeof(data));
	free(loader);
	return ok;
}

void udsFlashRestart(const struct UDSFlashPort* port, const struct UDSFlashOptions* options) {
	struct Loader loader;
	memset(&loader, 0, sizeof(loader));
	loader.port = port;
	_hardReset(&loader, options && options->usbJtag);
}

// The Windows COM port ----------------------------------------------------------------------------------------------------------

#ifdef _WIN32
struct WinPort {
	struct UDSFlashPort d;
	HANDLE handle;
	char name[32];
};

static HANDLE _openHandle(const char* name) {
	char path[64];
	snprintf(path, sizeof(path), "\\\\.\\%s", name);
	HANDLE handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		return handle;
	}
	DCB dcb;
	memset(&dcb, 0, sizeof(dcb));
	dcb.DCBlength = sizeof(dcb);
	if (GetCommState(handle, &dcb)) {
		dcb.BaudRate = 115200; // the ROM loader's rate on a UART; the native USB port ignores it
		dcb.ByteSize = 8;
		dcb.Parity = NOPARITY;
		dcb.StopBits = ONESTOPBIT;
		dcb.fBinary = TRUE;
		dcb.fOutxCtsFlow = FALSE;
		dcb.fOutxDsrFlow = FALSE;
		dcb.fDtrControl = DTR_CONTROL_DISABLE; // both low: opening the port leaves the chip alone
		dcb.fRtsControl = RTS_CONTROL_DISABLE;
		dcb.fOutX = FALSE;
		dcb.fInX = FALSE;
		dcb.fAbortOnError = FALSE;
		SetCommState(handle, &dcb);
	}
	COMMTIMEOUTS timeouts;
	memset(&timeouts, 0, sizeof(timeouts));
	timeouts.WriteTotalTimeoutConstant = 3000;
	SetCommTimeouts(handle, &timeouts);
	SetupComm(handle, 1 << 16, 1 << 16);
	PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
	return handle;
}

static bool _winWrite(void* context, const uint8_t* data, size_t length) {
	struct WinPort* port = context;
	while (length) {
		DWORD put = 0;
		if (!WriteFile(port->handle, data, (DWORD) length, &put, NULL) || !put) {
			return false;
		}
		data += put;
		length -= put;
	}
	return true;
}

static int _winRead(void* context, uint8_t* buffer, size_t capacity, unsigned timeoutMs) {
	struct WinPort* port = context;
	// MAXDWORD/MAXDWORD/constant: return at once with what is there, else wait up to the constant for the first byte.
	COMMTIMEOUTS timeouts;
	memset(&timeouts, 0, sizeof(timeouts));
	timeouts.ReadIntervalTimeout = MAXDWORD;
	timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
	timeouts.ReadTotalTimeoutConstant = timeoutMs ? timeoutMs : 1;
	timeouts.WriteTotalTimeoutConstant = 3000;
	if (!SetCommTimeouts(port->handle, &timeouts)) {
		return -1;
	}
	DWORD got = 0;
	if (!ReadFile(port->handle, buffer, (DWORD) capacity, &got, NULL)) {
		return -1;
	}
	return (int) got;
}

static void _winDtr(void* context, bool level) {
	struct WinPort* port = context;
	EscapeCommFunction(port->handle, level ? SETDTR : CLRDTR);
}

static void _winRts(void* context, bool level) {
	struct WinPort* port = context;
	EscapeCommFunction(port->handle, level ? SETRTS : CLRRTS);
}

static void _winSleep(void* context, unsigned ms) {
	(void) context;
	Sleep(ms);
}

static uint32_t _winNow(void* context) {
	(void) context;
	return (uint32_t) GetTickCount64();
}

static bool _winReopen(void* context) {
	struct WinPort* port = context;
	if (port->handle != INVALID_HANDLE_VALUE) {
		CloseHandle(port->handle);
		port->handle = INVALID_HANDLE_VALUE;
	}
	uint64_t start = GetTickCount64();
	while (GetTickCount64() - start < 8000) {
		Sleep(250);
		port->handle = _openHandle(port->name);
		if (port->handle != INVALID_HANDLE_VALUE) {
			return true;
		}
	}
	return false;
}

struct UDSFlashPort* udsFlashOpenPort(const char* name) {
	struct WinPort* port = calloc(1, sizeof(*port));
	if (!port) {
		return NULL;
	}
	snprintf(port->name, sizeof(port->name), "%s", name);
	port->handle = _openHandle(name);
	if (port->handle == INVALID_HANDLE_VALUE) {
		free(port);
		return NULL;
	}
	port->d.context = port;
	port->d.write = _winWrite;
	port->d.read = _winRead;
	port->d.setDtr = _winDtr;
	port->d.setRts = _winRts;
	port->d.sleepMs = _winSleep;
	port->d.nowMs = _winNow;
	port->d.reopen = _winReopen;
	return &port->d;
}

void udsFlashClosePort(struct UDSFlashPort* d) {
	if (!d) {
		return;
	}
	struct WinPort* port = d->context;
	if (port->handle != INVALID_HANDLE_VALUE) {
		CloseHandle(port->handle);
	}
	free(port);
}
#endif
