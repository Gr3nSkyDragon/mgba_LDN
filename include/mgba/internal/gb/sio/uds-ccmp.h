/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_SIO_UDS_CCMP_H
#define GB_SIO_UDS_CCMP_H

#include <mgba-util/common.h>

CXX_GUARD_START

/*
 * Stage 2, milestone R0 (doc/uds-wrapper-plan.md): what the real air needs below uds-room.c. Portable C with no allocation, so
 * the same files can later go into the ESP32 firmware.
 *
 *  - AES-128 (encryption only) and AES-CCM with a 16-byte block, 13-byte nonce and 8-byte tag, as 802.11 CCMP uses it;
 *  - the per-network data key a retail 3DS uses (Azahar's GenerateDataCCMPKey): AES-CTR of MD5(passphrase) under the UDS data
 *    key (slot 0x2D, from the user's key file, see uds-keyfile.h) with the counter MD5(comm id, network id, host MAC, id);
 *  - 802.11 data frames as Azahar builds and reads them: the 24-byte header, the 8-byte CCMP header, the encrypted payload and
 *    its tag; and the management frames and the association request body.
 */

#define UDS_AES_BLOCK 16
#define UDS_CCM_NONCE 13
#define UDS_CCM_TAG 8
#define UDS_80211_HEADER 24
#define UDS_CCMP_HEADER 8
#define UDS_DATA_OVERHEAD (UDS_80211_HEADER + UDS_CCMP_HEADER + UDS_CCM_TAG)

struct UDSAes {
	uint8_t roundKeys[11][16];
};

void udsAesInit(struct UDSAes* aes, const uint8_t key[16]);
void udsAesEncrypt(const struct UDSAes* aes, const uint8_t in[16], uint8_t out[16]);

// out receives length + 8 bytes: the ciphertext then the tag.
void udsCcmEncrypt(const uint8_t key[16], const uint8_t nonce[UDS_CCM_NONCE], const uint8_t* aad, size_t aadLength,
                   const uint8_t* plain, size_t length, uint8_t* out);
// in holds length + 8 bytes (ciphertext, tag). False when the tag does not match; out is then not to be used.
bool udsCcmDecrypt(const uint8_t key[16], const uint8_t nonce[UDS_CCM_NONCE], const uint8_t* aad, size_t aadLength,
                   const uint8_t* in, size_t length, uint8_t* out);

// The data key of one network. `slotKey` is the finished slot 0x2D key.
void udsCcmpDeriveKey(const uint8_t slotKey[16], const uint8_t* passphrase, size_t passphraseLength, uint32_t commId,
                      uint32_t networkId, const uint8_t hostMac[6], uint8_t id, uint8_t out[16]);

enum UDSDsMode {
	UDS_DS_NONE, // group-addressed frames: A1 destination, A2 transmitter, A3 BSSID
	UDS_DS_TO, // station to host: A1 BSSID, A2 transmitter, A3 destination
	UDS_DS_FROM, // host to station: A1 destination, A2 BSSID, A3 transmitter
};

// A protected data frame (frame control 0x4008 plus the DS bits). Returns its size, 0 when `capacity` is too small.
size_t udsBuildDataFrame(uint8_t* out, size_t capacity, const uint8_t key[16], const uint8_t* payload, size_t length,
                         const uint8_t transmitter[6], const uint8_t destination[6], const uint8_t bssid[6],
                         enum UDSDsMode mode, uint64_t packetNumber, uint16_t sequence);

struct UDSDataFrameInfo {
	uint16_t frameControl;
	uint8_t a1[6], a2[6], a3[6];
	uint16_t sequenceControl;
	uint64_t packetNumber;
};

// Parses and decrypts a protected data frame (no FCS). False when it is too short, not protected data, or the tag is wrong.
// `plain` needs length - UDS_DATA_OVERHEAD bytes; *plainLength gets that.
bool udsOpenDataFrame(const uint8_t key[16], const uint8_t* frame, size_t length, uint8_t* plain, size_t* plainLength,
                      struct UDSDataFrameInfo* info);

// A management frame: 24-byte header (frame control, duration 0, destination, transmitter, BSSID, sequence) and the body.
size_t udsBuildMgmtFrame(uint8_t* out, size_t capacity, uint16_t frameControl, const uint8_t transmitter[6],
                         const uint8_t destination[6], const uint8_t bssid[6], uint16_t sequence, const uint8_t* body,
                         size_t bodyLength);

// The association request body of a retail joiner: capabilities 0x0431, listen interval 1, the network id as an eight-digit
// upper-case hex SSID, and the supported and extended rates. Returns its size (30).
size_t udsBuildAssocRequestBody(uint8_t out[30], uint32_t networkId);

#define UDS_FC_AUTH 0x00B0
#define UDS_FC_ASSOC_REQUEST 0x0000
#define UDS_FC_ASSOC_RESPONSE 0x0010
#define UDS_FC_DEAUTH 0x00C0

CXX_GUARD_END

#endif
