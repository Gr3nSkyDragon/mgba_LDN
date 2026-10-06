/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_ESP32_H
#define GB_SIO_UDS_ESP32_H

#include <mgba-util/common.h>

CXX_GUARD_START

/*
 * Stage 2, milestone R1 (doc/uds-wrapper-plan.md): the serial protocol of Azahar's ESP32-S3 UDS radio firmware
 * (firmware/esp32-uds-bridge in the Azahar repository). The board is a dumb 802.11 radio: it captures and injects raw MPDUs and
 * hardware-ACKs frames for the MAC it was started with. Everything else (join, CCMP, Pia) happens on the PC.
 *
 * Only the documented serial protocol is used here, no firmware code (this is not the GB-Link LDN firmware: that one speaks a
 * different framing, see src/gba/sio/esp32/esp32-wire.h). A frame is
 *
 *   | version=1 | type | seq | flags | length:u16 LE | payload | crc32:u32 LE |
 *
 * with the ordinary zlib CRC-32 over everything before it, then COBS-encoded and ended by one 0x00 byte.
 *
 * Host to board: Hello, Start {channel, mac[6]}, Stop, SetChannel {channel}, TxFrame {flags, rate in 500 kbit/s, MPDU without FCS},
 * SetBeacon {MPDU}, SetWatch {mac[6]}, Ping {token}. Board to host: HelloAck {proto, major, minor, factory mac[6]}, Status
 * {request type, result s32}, Rx {channel, rssi s8, flags, MPDU without FCS}, Log {text}, Stats, Pong, TxDone {acked, length, head}.
 *
 * The board resets when its native USB port is opened and needs a few seconds to boot; udsEsp32WaitReady sends Hello until it
 * answers. The serial port is Esp32Serial (src/gba/sio/esp32/esp32-serial.h): Windows built in, others by Esp32SerialSetOps.
 *
 * Single threaded: call udsEsp32Poll often; it reads what the port has and calls the handlers.
 */

#define UDS_ESP32_VERSION 1
#define UDS_ESP32_HEADER 6
#define UDS_ESP32_MAX_PAYLOAD 2432
#define UDS_ESP32_MAX_RAW (UDS_ESP32_HEADER + UDS_ESP32_MAX_PAYLOAD + 4)
#define UDS_ESP32_MAX_ENCODED (UDS_ESP32_MAX_RAW + UDS_ESP32_MAX_RAW / 254 + 2)

enum UDSEsp32Command {
	UDS_ESP32_CMD_HELLO = 0x01,
	UDS_ESP32_CMD_START = 0x02,
	UDS_ESP32_CMD_STOP = 0x03,
	UDS_ESP32_CMD_SET_CHANNEL = 0x04,
	UDS_ESP32_CMD_TX_FRAME = 0x06,
	UDS_ESP32_CMD_SET_BEACON = 0x07,
	UDS_ESP32_CMD_SET_WATCH = 0x08,
	UDS_ESP32_CMD_PING = 0x09,
	// Firmware 1.4 on: the 3DS key kept on the board, and the Game Boy wrapper running on it (see udsEsp32GbStart).
	UDS_ESP32_CMD_SET_KEY = 0x0A,
	UDS_ESP32_CMD_KEY_STATUS = 0x0B,
	UDS_ESP32_CMD_ERASE_KEYS = 0x0C,
	UDS_ESP32_CMD_GB_START = 0x10,
	UDS_ESP32_CMD_GB_XFER = 0x11,
	UDS_ESP32_CMD_GB_STOP = 0x12,
};

enum UDSEsp32Event {
	UDS_ESP32_EVT_HELLO_ACK = 0x81,
	UDS_ESP32_EVT_STATUS = 0x82,
	UDS_ESP32_EVT_RX = 0x83,
	UDS_ESP32_EVT_LOG = 0x84,
	UDS_ESP32_EVT_STATS = 0x85,
	UDS_ESP32_EVT_PONG = 0x86,
	UDS_ESP32_EVT_TX_DONE = 0x87,
	UDS_ESP32_EVT_KEY_INFO = 0x88,
	UDS_ESP32_EVT_GB_STATE = 0x90,
	UDS_ESP32_EVT_GB_REPLY = 0x91,
	UDS_ESP32_EVT_GB_STATS = 0x92,
};

#define UDS_ESP32_KEY_SLOT_DATA 0x2D // the 3DS UDS data key the board's wrapper needs
#define UDS_ESP32_GB_TITLE 16
#define UDS_ESP32_GB_STATS 10

enum {
	UDS_ESP32_TX_NO_ACK = 0x01, // group addressed: do not wait for an ACK
	UDS_ESP32_RX_TRUNCATED = 0x01,
};

// Framing, exposed for the test --------------------------------------------------------------------------------------------

uint32_t udsEsp32Crc32(const uint8_t* data, size_t length);
// One frame with its trailing 0x00. Returns the size, or 0 if the payload or `capacity` is too small.
size_t udsEsp32Encode(uint8_t type, uint8_t seq, uint8_t flags, const uint8_t* payload, size_t length, uint8_t* out, size_t capacity);

struct UDSEsp32Frame {
	uint8_t type;
	uint8_t seq;
	uint8_t flags;
	size_t length;
	uint8_t payload[UDS_ESP32_MAX_PAYLOAD];
};

struct UDSEsp32Decoder {
	uint8_t encoded[UDS_ESP32_MAX_ENCODED];
	size_t length;
	bool overflowed;
	unsigned framesOk;
	unsigned framesBad; // corrupt frames dropped; decoding resynchronises at the next 0x00
};

void udsEsp32DecoderInit(struct UDSEsp32Decoder* decoder);
// One received byte. True when it completed a valid frame, which is then in *frame.
bool udsEsp32DecoderFeed(struct UDSEsp32Decoder* decoder, uint8_t byte, struct UDSEsp32Frame* frame);

// The board ----------------------------------------------------------------------------------------------------------------------

struct UDSEsp32Info {
	bool valid; // a HelloAck has arrived
	uint8_t proto;
	uint8_t major;
	uint8_t minor;
	uint8_t factoryMac[6];
};

struct UDSEsp32Rx {
	uint8_t channel;
	int8_t rssi;
	uint8_t flags;
	const uint8_t* mpdu; // without FCS; valid during the call
	size_t length;
};

struct UDSEsp32;
struct UDSEsp32Handlers {
	void* context;
	void (*rx)(void* context, const struct UDSEsp32Rx* rx);
	void (*status)(void* context, uint8_t requestType, int32_t result);
	void (*log)(void* context, const char* text, size_t length);
	void (*txDone)(void* context, bool acked, size_t length);
	// The wrapper on the board: its state changed (room, session, wire phase, generation, channel), or its 5-second counters.
	void (*gbState)(void* context, const uint8_t state[5]);
	void (*gbStats)(void* context, const uint32_t stats[UDS_ESP32_GB_STATS]);
};

struct Esp32Serial;
struct UDSEsp32 {
	struct Esp32Serial* port;
	struct UDSEsp32Decoder decoder;
	struct UDSEsp32Handlers handlers;
	struct UDSEsp32Info info;
	uint8_t seq;
	unsigned rxFrames;
	unsigned statusFrames;
	unsigned logFrames;
	unsigned txDoneFrames;
	unsigned txAcked;
	uint32_t lastPong;
	bool portFailed;
	// Answers that are waited for (udsEsp32GbTransfer, udsEsp32KeyStatus, udsEsp32Request).
	bool keyInfoValid;
	bool keyPresent;
	bool replyValid;
	uint8_t replySeq;
	uint8_t replyByte;
	uint8_t replyPhase;
	bool statusValid;
	uint8_t statusType;
	int32_t statusResult;
};

// `portName` may be NULL or empty to find the board (Esp32SerialFindEspressif). Returns false when no port opens.
bool udsEsp32Open(struct UDSEsp32* esp, const char* portName, const struct UDSEsp32Handlers* handlers);
void udsEsp32Close(struct UDSEsp32* esp);

// Reads what the port has and dispatches the frames. Returns false once the port has failed.
bool udsEsp32Poll(struct UDSEsp32* esp);
// Sends Hello now and then and polls until the board answers or `timeoutMs` has passed. Returns info.valid.
bool udsEsp32WaitReady(struct UDSEsp32* esp, unsigned timeoutMs);

// `decoy`: the board's hardware gets the MAC with its first octet xor 0x02 (the optional flags byte of Start, bit 0), so that frames for
// `mac` are ordinary captured traffic instead of being swallowed by the chip; the hardware then does not acknowledge them.
bool udsEsp32Start(struct UDSEsp32* esp, uint8_t channel, const uint8_t mac[6], bool decoy);
bool udsEsp32SendHello(struct UDSEsp32* esp); // one Hello; the answer arrives through udsEsp32Poll (info.valid)
bool udsEsp32Stop(struct UDSEsp32* esp);
bool udsEsp32SetChannel(struct UDSEsp32* esp, uint8_t channel);
bool udsEsp32SetWatch(struct UDSEsp32* esp, const uint8_t mac[6]); // all zero clears
bool udsEsp32TxFrame(struct UDSEsp32* esp, uint8_t flags, uint8_t rate500kbps, const uint8_t* mpdu, size_t length);
bool udsEsp32SetBeacon(struct UDSEsp32* esp, const uint8_t* mpdu, size_t length); // empty clears
bool udsEsp32Ping(struct UDSEsp32* esp, uint32_t token);

// Firmware 1.4 on. These wait for the board's answer (up to `timeoutMs`), polling the port meanwhile.
bool udsEsp32HasGbWrapper(const struct UDSEsp32* esp); // the HelloAck says 1.4 or later
// Stores a key in the board's flash (the board never sends it back). True when the board says it is stored.
bool udsEsp32SetKey(struct UDSEsp32* esp, uint8_t slot, const uint8_t key[16], unsigned timeoutMs);
// Whether the board holds the UDS data key. -1: no answer.
int udsEsp32KeyStatus(struct UDSEsp32* esp, unsigned timeoutMs);
bool udsEsp32EraseKeys(struct UDSEsp32* esp, unsigned timeoutMs);
// Starts the board's Game Boy wrapper for the cartridge with this header title (0x134, up to 16 characters) and player name. The result
// is the board's status (0 ok; see udsEsp32GbStartText), or -1 for no answer.
int32_t udsEsp32GbStart(struct UDSEsp32* esp, bool log, const char title[UDS_ESP32_GB_TITLE], const uint16_t name[10], unsigned timeoutMs);
const char* udsEsp32GbStartText(int32_t result);
bool udsEsp32GbStop(struct UDSEsp32* esp, unsigned timeoutMs);
// One transfer the cartridge clocked out as master: `master` goes to the board, the byte the slave shifts back comes into *reply. False if
// the board did not answer in time.
bool udsEsp32GbTransfer(struct UDSEsp32* esp, uint8_t master, uint8_t* reply, uint8_t* phase, unsigned timeoutMs);

const char* udsEsp32StatusText(int32_t result);

CXX_GUARD_END

#endif
