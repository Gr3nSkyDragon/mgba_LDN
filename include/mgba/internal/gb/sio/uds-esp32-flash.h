/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_ESP32_FLASH_H
#define GB_SIO_UDS_ESP32_FLASH_H

#include <mgba-util/common.h>

CXX_GUARD_START

/*
 * Writes a firmware image to an ESP32-S3 through the chip's own ROM loader (the serial protocol Espressif documents for esptool), so
 * that uds-esp32-setup can install Azahar's esp32-uds-bridge firmware without Python or ESP-IDF.
 *
 * The image is a release file of that firmware: bootloader, partition table and app merged into one file written at offset 0
 * (esp32-uds-bridge-fw<version>-esp32s3.bin, made by its package-release-bin.ps1). It is written whole, gaps included, so the key
 * store in between is erased; the setup tool stores the key again afterwards.
 *
 * The steps are esptool's with --no-stub: reset the chip into its download mode (DTR/RTS, the sequence for the S3's native USB port or
 * for a USB-to-UART bridge), SYNC, read the chip's magic number, SPI_ATTACH, SPI_SET_PARAMS, FLASH_BEGIN (which erases), FLASH_DATA in
 * 1 KB blocks, SPI_FLASH_MD5 to compare with the file, and a hard reset into the new firmware.
 *
 * The serial port is a small table, so the unit test (uds-esp32-flash-test) can stand in for the chip; udsFlashOpenPort is the
 * Windows COM port.
 */

#define UDS_FLASH_CHIP_ESP32S3 9 // the image header's chip id, and the S3's CHIP_DETECT_MAGIC value
#define UDS_FLASH_BLOCK 0x400 // FLASH_DATA block size of the ROM loader
#define UDS_FLASH_MAGIC_REG 0x40001000u // CHIP_DETECT_MAGIC_REG

enum UDSFlashCommand {
	UDS_FLASH_BEGIN = 0x02,
	UDS_FLASH_DATA = 0x03,
	UDS_FLASH_END = 0x04,
	UDS_FLASH_SYNC = 0x08,
	UDS_FLASH_READ_REG = 0x0A,
	UDS_FLASH_SPI_SET_PARAMS = 0x0B,
	UDS_FLASH_SPI_ATTACH = 0x0D,
	UDS_FLASH_SPI_MD5 = 0x13,
};

struct UDSFlashPort {
	void* context;
	bool (*write)(void* context, const uint8_t* data, size_t length);
	// Waits up to timeoutMs for the first byte, then returns what is there: the count, 0 on a timeout, -1 if the port failed.
	int (*read)(void* context, uint8_t* buffer, size_t capacity, unsigned timeoutMs);
	void (*setDtr)(void* context, bool level);
	void (*setRts)(void* context, bool level);
	void (*sleepMs)(void* context, unsigned ms);
	uint32_t (*nowMs)(void* context);
	// The chip reset; its USB port may have gone away and come back. Closes the port and opens it again (retrying for a few seconds).
	bool (*reopen)(void* context);
};

struct UDSFlashHandlers {
	void* context;
	void (*log)(void* context, const char* text);
	void (*progress)(void* context, size_t done, size_t total);
};

// What an image file holds, from its headers. udsFlashCheckImage fills it and says what is wrong with an unusable file.
struct UDSFlashImageInfo {
	size_t flashSize; // from the bootloader header
	size_t appOffset; // the factory (or first) app partition
	char project[33]; // the app's esp_app_desc_t
	char version[33];
	char date[17];
	char time[17];
};
bool udsFlashCheckImage(const uint8_t* image, size_t size, struct UDSFlashImageInfo* info, char* why, size_t whyCapacity);

struct UDSFlashOptions {
	bool usbJtag; // the S3's native USB port (Espressif's VID 303A); otherwise a USB-to-UART bridge with the classic auto-reset wiring
};

// Writes the whole image at offset 0 and restarts the chip into it. False with the reason in `error` (the board then still runs its
// ROM loader or the old firmware; nothing else on it is touched before the erase).
bool udsFlashWriteImage(const struct UDSFlashPort* port, const struct UDSFlashHandlers* handlers, const struct UDSFlashOptions* options,
                        const uint8_t* image, size_t size, char* error, size_t errorCapacity);

// Restarts the chip into its firmware (the reset that ends udsFlashWriteImage). For a board left in download mode.
void udsFlashRestart(const struct UDSFlashPort* port, const struct UDSFlashOptions* options);

// Framing, exposed for the test --------------------------------------------------------------------------------------------------
// SLIP: C0 ... C0, with C0 sent as DB DC and DB as DB DD. Returns the encoded size, or 0 if `capacity` is too small.
size_t udsFlashSlipEncode(const uint8_t* data, size_t length, uint8_t* out, size_t capacity);
uint8_t udsFlashChecksum(const uint8_t* data, size_t length); // FLASH_DATA's: XOR of the bytes, seeded with EF

#ifdef _WIN32
// The Windows COM port (e.g. "COM4"), opened with DTR and RTS low so that opening it does not reset the chip. NULL if it does not open.
struct UDSFlashPort* udsFlashOpenPort(const char* name);
void udsFlashClosePort(struct UDSFlashPort* port);
#endif

CXX_GUARD_END

#endif
