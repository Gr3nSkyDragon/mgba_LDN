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
 * Nintendo LDN (local wireless) pieces: the keys needed to read a Switch's advertisements and the advertisement
 * decoder. Windows only (crypto through CNG). Layout and key derivation follow the format the Switch's action frames
 * are known to have (as implemented by other LDN clients such as ldn_mitm / Ryujinx's LdnReal).
 */

// Set to 1 to have the decoder describe on stderr why an advertisement could not be decoded.
extern int gLdnDebug;

struct LdnKeys {
	uint8_t masterKey00[16];
	uint8_t masterKey12[16];
	uint8_t aesKekGenerationSource[16];
	uint8_t aesKeyGenerationSource[16];
};

// Reads the four keys LDN needs out of a prod.keys file. The key values are never printed or logged.
bool LdnKeysLoad(const char* path, struct LdnKeys* keys);

enum {
	LDN_MAX_PARTICIPANTS = 8,
	LDN_MAX_APP_DATA = 384,
};

struct LdnParticipant {
	bool present;
	uint8_t ip[4];
	uint8_t mac[6];
	uint8_t index;
	uint8_t platform;
	char name[33];
};

struct LdnAdvertisement {
	int protocol; // 1 or 3
	uint8_t format;  // 1 plain, 2 AES-CTR, 3 AES-GCM
	uint8_t version;
	uint64_t localCommunicationId;
	uint16_t sceneId;
	uint8_t ssid[16];
	uint8_t serverRandom[16];
	uint64_t challenge;
	uint8_t securityMode;
	uint8_t stationAcceptPolicy;
	uint16_t appVersion;
	unsigned advertisedChannel;
	uint8_t maxParticipants;
	uint8_t numParticipants;
	struct LdnParticipant participants[LDN_MAX_PARTICIPANTS];
	uint16_t appDataSize;
	uint8_t appData[LDN_MAX_APP_DATA];
	bool infoDecoded; // false when only the envelope (ids, ssid) could be read
};

// `body` is the payload of a vendor-specific action frame, starting at the category byte (0x7F).
// Returns false when it is not an LDN advertisement (or cannot be decrypted with the keys given).
bool LdnDecodeAdvertisement(const uint8_t* body, size_t length, const struct LdnKeys* keys, struct LdnAdvertisement* out);

// The 16-byte WPA2 PSK to join a network, derived from its advertisement's `serverRandom` (16 bytes), `protocol`
// and the keys - see ldn.c for the recipe. Needed to actually associate (nl80211 CMD_CONNECT with an RSN/CCMP/PSK
// information element built around this key) with the network a decoded LdnAdvertisement described.
bool LdnDeriveWlanKey(const struct LdnKeys* keys, int protocol, const uint8_t serverRandom[16], uint8_t out[16]);

// The actual over-the-air Wi-Fi SSID for a decoded advertisement's network: its 16-byte `ssid` field, lowercase
// hex-encoded to 32 ASCII characters (that is literally how Nintendo derives it - not a coincidence of naming).
// `out` must hold at least 33 bytes (32 characters + a NUL).
void LdnAdvertisementWlanSsid(const struct LdnAdvertisement* advertisement, char out[33]);

// The key used to encrypt/decrypt an authentication frame's payload (protocol 3 / AES-GCM only - see ldn-auth.h),
// derived from the CLIENT's own random nonce. Same KDF chain/source as LdnDeriveWlanKey (DataKeySource), different
// data, so both sides end up with the same key once the host echoes clientRandom back in its response.
bool LdnDeriveAuthenticationKey(const struct LdnKeys* keys, int protocol, const uint8_t clientRandom[16], uint8_t out[16]);

// AES-GCM with an explicit, caller-supplied 12-byte nonce and a separate tag (unlike LdnDecodeAdvertisement's own
// AES-GCM format, which pads a 4-byte nonce and concatenates the tag with the ciphertext) - what LDN's
// authentication frame and challenge use instead.
bool LdnAesGcmEncrypt(const uint8_t key[16], const uint8_t nonce[12], const uint8_t* aad, size_t aadLength, const uint8_t* in, size_t length, uint8_t* out,
                      uint8_t tag[16]);
bool LdnAesGcmDecrypt(const uint8_t key[16], const uint8_t nonce[12], const uint8_t* aad, size_t aadLength, const uint8_t tag[16], const uint8_t* in,
                      size_t length, uint8_t* out);

// AES-GCM with a caller-chosen tag length (1-16 bytes) - Windows CNG's own GCM refuses anything under 12, but the
// Pia transport (ldn-pia.c) needs an 8-byte tag, so this is implemented from the raw algorithm instead of CNG.
bool LdnAesGcmEncryptTag(const uint8_t key[16], const uint8_t nonce12[12], const uint8_t* aad, size_t aadLength, const uint8_t* in, size_t length,
                         uint8_t* out, uint8_t* tag, size_t tagLength);
bool LdnAesGcmDecryptTag(const uint8_t key[16], const uint8_t nonce12[12], const uint8_t* aad, size_t aadLength, const uint8_t* tag, size_t tagLength,
                         const uint8_t* in, size_t length, uint8_t* out);

// HMAC-SHA256, for the authentication challenge's integrity check (see ldn-auth.c's ChallengeRequest).
bool LdnHmacSha256(const uint8_t* key, size_t keyLength, const uint8_t* data, size_t dataLength, uint8_t out[32]);

// `length` cryptographically random bytes (CNG's RNG). Used for the authentication handshake's client-side nonces.
bool LdnRandomBytes(uint8_t* out, size_t length);

// One AES-128-ECB block, no padding - the Pia session key derivation (ldn-pia.c) needs this directly (AES of the
// LDN SSID under a fixed game key), unlike every other AES use in this project which goes through _deriveKey.
bool LdnAesEcbEncryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/*
 * The FRLG Switch port's own RFU search beacon: a small record it embeds in its LDN advertisement's
 * application data (LdnAdvertisement.appData), describing the host for the game's own Wireless Club purposes.
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

// `appData`/`appDataSize` are LdnAdvertisement's fields. False when appData is too short to hold the header
// and the record.
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
