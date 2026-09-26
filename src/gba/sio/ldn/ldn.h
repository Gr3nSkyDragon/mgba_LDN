/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LDN_LDN_H
#define GBA_SIO_LDN_LDN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Nintendo LDN (local wireless) pieces mGBA still needs itself now that ldnd (see ldnd.h) does the scanning, joining
 * and key handling: the GBA emulator's LDN passphrase, the crypto primitives the Pia transport (ldn-pia.c) is built
 * on, and the FireRed/LeafGreen room beacon found in a network's application data. The crypto is Windows only (CNG).
 */

// The GBA Virtual Console's LDN passphrase, shared by all of its titles: a join passes it to ldnd (it goes into the
// link key, so without it the host accepts the join and then drops everything we send).
extern const uint8_t kLdnGbaPassphrase[64];

// AES-GCM with a caller-chosen tag length (1-16 bytes) - Windows CNG's own GCM refuses anything under 12, but the
// Pia transport (ldn-pia.c) needs an 8-byte tag, so this is implemented from the raw algorithm instead of CNG.
bool LdnAesGcmEncryptTag(const uint8_t key[16], const uint8_t nonce12[12], const uint8_t* aad, size_t aadLength, const uint8_t* in, size_t length,
                         uint8_t* out, uint8_t* tag, size_t tagLength);
bool LdnAesGcmDecryptTag(const uint8_t key[16], const uint8_t nonce12[12], const uint8_t* aad, size_t aadLength, const uint8_t* tag, size_t tagLength,
                         const uint8_t* in, size_t length, uint8_t* out);

// `length` cryptographically random bytes (CNG's RNG): the Pia connection's nonce, a join's device id.
bool LdnRandomBytes(uint8_t* out, size_t length);

// One AES-128-ECB block, no padding - the Pia session key derivation (ldn-pia.c): AES of the LDN SSID under a fixed
// game key.
bool LdnAesEcbEncryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/*
 * The FRLG Switch port's own RFU search beacon: a small record it embeds in its LDN advertisement's
 * application data (LdndNetworkInfo.applicationData), describing the host for the game's own Wireless Club purposes.
 * It is NOT the same layout as a real GBA cartridge's 24-byte RFU broadcast record (see rfu.c's BCAST trace) -
 * this is the Switch port's own, simpler record. LdnBeaconToBroadcastWords() below turns it into the 6-word
 * record our emulated adapter's BroadcastRead command expects, filling in the pieces (compat/serial code,
 * activity) that this record does not carry itself.
 *
 * Layout (reverse engineered from the frlg-ldn-trade reference tool, cross-checked live: the name this decodes
 * matches the name already carried in the LDN advertisement's own participant field):
 *   appData = a 0x5C-byte Pia system header, then a custom base85 encoding of a 24-byte record:
 *     bytes  0- 1: trainer id (u16 LE)
 *     bytes  2- 9: host name, FRLG character set (see LdnFrlgCharToAscii), 0xFF-terminated
 *     bytes 10-11: RFU session id (u16 LE)
 *     bytes 12-19: partner info (meaning not yet known; all zero when nobody has joined)
 *     bytes 20-23: bits 16-31 = the species of the lead party member (0 outside a trade), LE u32
 *   Base85: alphabet is the 85 bytes 0x23..0x78 with 0x5C ('\\') skipped, groups of 5 characters decode to
 *   4 bytes little-endian, and (unusually) the FIRST character of a group is the LEAST-significant base85 digit.
 */
enum {
	LDN_RFU_BEACON_PIA_HEADER = 0x5C,
};

struct LdnRfuBeacon {
	uint16_t trainerId;
	char name[9]; // ASCII, NUL-terminated
	uint16_t rfuSessionId;
	uint8_t partnerInfo[8];
	uint16_t tradeSpecies;
};

// `appData`/`appDataSize` are a network's application data (LdndNetworkInfo). False when appData is too short to
// hold the header and the record.
bool LdnDecodeRfuBeacon(const uint8_t* appData, size_t appDataSize, struct LdnRfuBeacon* out);

// One FRLG character <-> ASCII. Letters and digits only (what a trainer name can hold); anything else maps to
// '?' (decoding) or ' ' (encoding, i.e. it is dropped).
char LdnFrlgCharToAscii(uint8_t frlgChar);
uint8_t LdnAsciiToFrlgChar(char ascii);

// Builds the 6-word broadcast record (RFU_BROADCAST_WORDS) our emulated adapter would show a joiner: `compat`
// is the compatibility+serial word (upper 16 = compatibility bits, lower 16 = serial number - see rfu.c's BCAST
// trace for known values), `activity` is what a real cartridge would put in the low byte of word 3 (4 = trade,
// the only one this project cares about). The beacon's name is re-encoded into the FRLG character set.
void LdnBeaconToBroadcastWords(const struct LdnRfuBeacon* beacon, uint32_t compat, uint8_t activity, uint32_t words[6]);

#endif
