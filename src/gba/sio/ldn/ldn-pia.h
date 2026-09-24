/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LDN_PIA_H
#define GBA_SIO_LDN_PIA_H

#include "ldnd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Pia is Nintendo's peer-to-peer session middleware: plain UDP, port 12345, every datagram opening with the
 * magic 32 AB 98 64. FireRed/LeafGreen's Virtual Console app uses Pia version byte 15/16 (6.32+), a 21-byte
 * fixed header followed by an 8-byte truncated AES-GCM tag then the ciphertext (829 header bytes total = 0x1D,
 * matching the NintendoClients wiki's own table).
 *
 * Layout and key/nonce derivation read from the `pokeldn` reference project's own `pokeldn/ldn/crypto.py`
 * (Decryptu/pokeldn, referred by the project's maintainer as the primary reference for this layer) - a
 * live-tested, from-hardware-measurement implementation, not a specification the project invented. Used only to
 * learn the wire format; nothing here is copied source, and every byte offset was independently cross-checked
 * against a from-scratch Python re-derivation plus pycryptodome round-trip vectors before being relied on (see
 * the project notes).
 *
 *   header (21 bytes, all multi-byte fields big-endian):
 *     0x00  4  magic 32 AB 98 64
 *     0x04  1  enc      (0x90 observed - top bit "encrypted" plus a format nibble, not otherwise interpreted)
 *     0x05  1  flags    (pad_size << 4) | (1 if the plaintext body is zstd-compressed) | (2 if this datagram is
 *                        part of the establishing connection exchange, i.e. Net 0x12 / Session join) - where
 *                        pad_size is the number of trailing 0xFF padding bytes the plaintext body was grown by to
 *                        reach a 16-byte boundary. Confirmed against pokeldn/frlgsim's `sim.py`
 *                        `_send_messages()` AND independently against a live captured host datagram (pad=8,
 *                        compressed+establishing -> byte 0x83, the exact value on the wire) - not a fixed
 *                        constant, despite an earlier revision of this comment claiming "0x50 observed, not
 *                        otherwise interpreted" (that was only the coincidental value for one particular
 *                        uncompressed/non-establishing/5-byte-pad message).
 *     0x06  2  destination Pia variable id
 *     0x08  2  source Pia variable id
 *     0x0A  2  packet id (a per-session counter; NOT the AES-GCM nonce)
 *     0x0C  1  footer size (bytes of plaintext tail after the messages - almost always 2, a big-endian
 *                           recipient variable id; not covered by the GCM tag)
 *     0x0D  8  AES-GCM header nonce (an 8-byte monotonic counter, NOT the full 12-byte AES-GCM nonce itself)
 *   then an 8-byte AES-GCM tag (truncated from the real 16), then the ciphertext.
 *
 * Session key: AES-128-ECB(FRLG's fixed 16-byte game key).encrypt(ssid) where `ssid` is the LDN advertisement's
 * raw 16-byte SSID field (LdnAdvertisement.ssid - NOT the hex-encoded 32-character Wi-Fi SSID string
 * LdnAdvertisementWlanSsid produces for nl80211; this is the same 16 raw bytes, used directly).
 *
 * AES-GCM nonce (12 bytes) = (CRC32(ssid[1..15]) XOR the SENDER's own LDN IPv4 address, as a big-endian u32)
 * (4 bytes) || the header's own 8-byte nonce field. AAD is empty. The plaintext, once decrypted, may be a zstd
 * frame (not yet implemented - ldn-pia.c leaves compression for the layer above, mirroring the wire: whether a
 * given payload is compressed is a Pia MESSAGE flag, not a property of the datagram itself).
 */

enum {
	LDN_PIA_PORT = 12345,
	LDN_PIA_HEADER_SIZE = 13,  // through and including the footer-size byte, NOT the 8-byte nonce
	LDN_PIA_NONCE_SIZE = 8,
	LDN_PIA_TAG_SIZE = 8,
	// header + nonce + tag: the offset of the ciphertext within a Pia datagram.
	LDN_PIA_CIPHERTEXT_OFFSET = LDN_PIA_HEADER_SIZE + LDN_PIA_NONCE_SIZE + LDN_PIA_TAG_SIZE,
};

extern const uint8_t kLdnPiaMagic[4]; // 32 AB 98 64

struct LdnPiaHeader {
	uint16_t dst;
	uint16_t src;
	uint16_t pktid;
	uint8_t nonce8[LDN_PIA_NONCE_SIZE];
	uint8_t enc;
	uint8_t flags;
	uint8_t footer;
};

// Writes the 21-byte fixed header (LDN_PIA_HEADER_SIZE + LDN_PIA_NONCE_SIZE) to `out`.
void LdnPiaHeaderPack(const struct LdnPiaHeader* header, uint8_t out[21]);
// `length` must be at least LDN_PIA_CIPHERTEXT_OFFSET. Does not check the magic - see LdnPiaIsPia.
void LdnPiaHeaderUnpack(const uint8_t* datagram, struct LdnPiaHeader* out);

// True if `datagram` is at least long enough to hold a header+nonce+tag and starts with the Pia magic.
bool LdnPiaIsPia(const uint8_t* datagram, size_t length);

struct LdnPiaCrypto {
	uint8_t sessionKey[16];
	uint32_t netId; // CRC32(ssid[1..15])
};

// `ssid` is the LDN advertisement's raw 16-byte network SSID (LdnAdvertisement.ssid).
void LdnPiaCryptoInit(struct LdnPiaCrypto* crypto, const uint8_t ssid[16]);

// Decrypts a received Pia datagram. `srcIp` is the SENDER's own LDN IPv4 address (big-endian byte order, i.e.
// srcIp[0] is the most significant octet) - not derivable from the datagram itself, the caller must already know
// it (from the advertisement's participant list or the UDP source address). `outPlain` must be at least
// `length - LDN_PIA_CIPHERTEXT_OFFSET` bytes; `*outLength` receives the plaintext length. Returns false if
// `datagram` is not a well-formed, authenticating Pia datagram (wrong tag, too short, wrong magic).
bool LdnPiaDecrypt(const struct LdnPiaCrypto* crypto, const uint8_t* datagram, size_t length, const uint8_t srcIp[4], uint8_t* outPlain,
                   size_t* outLength);

// Encrypts `plaintext` into a complete Pia datagram (header + nonce + tag + ciphertext) at `outDatagram`, which
// must be at least `length + LDN_PIA_CIPHERTEXT_OFFSET` bytes; `*outLength` receives the datagram's total
// length. `header->nonce8` is the GCM header nonce - the caller is responsible for making it a fresh, strictly
// increasing 8-byte counter per datagram (the real console enforces "strictly above the last accepted value" and
// drops anything else - see the project notes); every other header field is used as given. `srcIp` is OUR OWN
// LDN IPv4 address (this datagram's sender).
bool LdnPiaEncrypt(const struct LdnPiaCrypto* crypto, const uint8_t* plaintext, size_t length, const uint8_t srcIp[4],
                   const struct LdnPiaHeader* header, uint8_t* outDatagram, size_t* outLength);

// A decrypted Pia payload may itself be a zstd frame (checked via the zstd magic number) - live-confirmed: the
// real Switch's own connection-layer messages (Net/Session) are sent this way, not just game data. Decompresses
// `data` into `out` if it looks like a zstd frame (via the vendored `src/third-party/zstd/zstdlib.c` build);
// otherwise copies it through unchanged, so a caller can always run every decrypted payload through this before
// message-tiling. `*outLength` is the buffer size on entry and the real output length on return. Returns false
// only for a payload that IS a zstd frame but fails to decode (never for plaintext, which always passes through
// untouched). `data` may be immediately followed by other bytes (a plaintext footer/0xFF padding) that are not
// part of the zstd frame itself - only the frame's own compressed length is ever read, via
// ZSTD_findFrameCompressedSize, so trailing bytes are safely ignored rather than tripping decode.
bool LdnPiaDecompress(const uint8_t* data, size_t length, uint8_t* out, size_t* outLength);

// The reference client zstd-compresses exactly one outgoing message on this whole layer: the Session join (see
// pokeldn/frlgsim's `ConnectionManager.on_message`, `_q(PROTO_SESSION, self._join(), ..., compress=True, ...)` -
// every other message it ever sends is uncompressed). Compresses `data` (the already-tiled message bytes, i.e.
// LdnPiaBuildMessage's own output, BEFORE any footer/padding is appended - mirrors how the host's own compressed
// messages decode to a raw tiled-message blob with no footer/padding inside the frame) into a zstd frame at
// `out`. `*outLength` is the buffer size on entry and the real compressed length on return. Uses a low
// compression level (correctness, not ratio, matters for messages this small). Returns false only on an actual
// encoder error (e.g. output buffer too small).
bool LdnPiaCompress(const uint8_t* data, size_t length, uint8_t* out, size_t* outLength);

/*
 * The actual UDP :12345 datagrams Pia rides on, sent/received as raw Ethernet frames over the STATION interface
 * (not through ldnd's kernel IP stack): ldnd's own SENDTO operation has no destination-address parameter (it is a
 * plain `send()`, confirmed by reading the daemon's own source - see the project notes), so an arbitrary-destination
 * UDP datagram can only go out as a hand-built Ethernet+IPv4+UDP frame injected on a raw AF_PACKET socket, the same
 * way ldn-monitor.c already injects/captures raw 802.11 frames. This also means no IP address ever needs to be
 * configured on the kernel interface - sending and receiving both bypass the kernel's IP stack entirely, exactly
 * like this project's LDN association/auth layers already do.
 */

enum {
	LDN_PIA_MAX_DATAGRAM = 2048, // payload only (the largest Pia message this project ever sends/expects)
};

struct LdnPiaSocket;

// `ifIndex`/`ourMac` are the STATION interface's (see LdnStationFindInterface) - NOT the monitor interface.
struct LdnPiaSocket* LdnPiaSocketOpen(struct LdndConnection* conn, uint32_t ifIndex, const uint8_t ourMac[6]);
void LdnPiaSocketClose(struct LdnPiaSocket*);

// Builds and sends one UDP :12345 datagram (Ethernet dst `destMac`, IPv4 src `srcIp` -> dst `destIp`) carrying
// `payload` (a complete encrypted Pia datagram from LdnPiaEncrypt, or anything else the caller wants to send raw
// on this port). Returns 0, or a negative LDND_ERR_*-style error.
int LdnPiaSocketSend(struct LdnPiaSocket*, const uint8_t destMac[6], const uint8_t srcIp[4], const uint8_t destIp[4], const uint8_t* payload,
                     size_t length);

// Dequeues one received UDP :12345 datagram's payload (non-blocking; returns false if none is pending). `outIp`
// receives the sender's IPv4 address; `*inOutLength` is the buffer size on entry (payloads over that size are
// truncated) and the datagram's real length on return.
// Sends one ARP frame (Ethernet ethertype 0x0806) out the station interface. The retail Switch did not send
// anything back to a Ryubing-emulated host until it received an ARP telling it that host's IP-to-MAC mapping - and
// group-broadcast ARP was NOT enough; a pairwise (unicast, kernel-encrypted) ARP straight to the Switch's MAC was
// the decisive fix (see the project notes). The kernel encrypts whatever frame goes out the associated station
// interface, so a unicast `destMac` here is a pairwise-protected ARP. `destMac` NULL = Ethernet broadcast.
// `opcode` is 1 (request) or 2 (reply); `targetMac` may be NULL (zeros) for a request. Returns 0 or a negative error.
int LdnPiaSocketSendArp(struct LdnPiaSocket*, const uint8_t* destMac, unsigned opcode, const uint8_t srcIp[4], const uint8_t targetIp[4],
                        const uint8_t* targetMac);

bool LdnPiaSocketPoll(struct LdnPiaSocket*, uint8_t outIp[4], uint8_t* outPayload, size_t* inOutLength);

const char* LdnPiaSocketLastError(void);

#endif
