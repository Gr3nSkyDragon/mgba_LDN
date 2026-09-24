/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ldn-auth.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

enum {
	// Authentication frame format (LDN-0.0.3's ldn/__init__.py AUTH_FORMAT_*).
	AUTH_FORMAT_PLAIN = 0,
	AUTH_FORMAT_AES_GCM = 1,

	LDN_AUTH_HEADER_SIZE = 0x48, // version+len+status+isResponse+len+format+pad(2) [8] + networkId(32) + serverRandom(16) + clientRandom(16)

	LDN_CHALLENGE_BODY_SIZE = 0x2D0, // 720
	LDN_CHALLENGE_REQUEST_SIZE = 0x300, // 768: u32(0) + hmac(32) + pad(12) + body(720)

	LDN_AUTH_REQUEST_BASE_SIZE = 0x64, // 100: name(32) + appVersion(2) + platform(1) + pad(29) + pad(0x24)
	LDN_AUTH_REQUEST_SIZE = LDN_AUTH_REQUEST_BASE_SIZE + LDN_CHALLENGE_REQUEST_SIZE, // 868

	// 6-byte prefix (OUI 0x0022AA + frame type 0x0102 + 1 pad) + header + tag (AES-GCM only) + payload.
	LDN_AUTH_FRAME_MAX = 6 + LDN_AUTH_HEADER_SIZE + 16 + LDN_AUTH_REQUEST_SIZE,

	PLATFORM_NX = 0,
};

// This key is used for the HMAC algorithm in the authentication challenge - a fixed constant (not derived from
// prod.keys), the same for every LDN client. Source: the LDN-0.0.3 reference client's own ldn/__init__.py
// (CHALLENGE_KEY); there is also a CHALLENGE_KEY_DEV for devkits, not needed here.
static const uint8_t kChallengeKey[32] = {
	0xf8, 0x4b, 0x48, 0x7f, 0xb3, 0x72, 0x51, 0xc2, 0x63, 0xbf, 0x11, 0x60, 0x90, 0x36, 0x58, 0x92,
	0x66, 0xaf, 0x70, 0xca, 0x79, 0xb4, 0x4c, 0x93, 0xc7, 0x37, 0x0c, 0x57, 0x69, 0xc0, 0xf6, 0x02,
};

static void _writeU16LE(uint8_t* p, uint16_t v) {
	p[0] = (uint8_t) v;
	p[1] = (uint8_t) (v >> 8);
}

static void _writeU16BE(uint8_t* p, uint16_t v) {
	p[0] = (uint8_t) (v >> 8);
	p[1] = (uint8_t) v;
}

static void _writeU64LE(uint8_t* p, uint64_t v) {
	for (int i = 0; i < 8; ++i) {
		p[i] = (uint8_t) (v >> (8 * i));
	}
}

// Builds the little-endian 32-byte NetworkId used inside an authentication frame's header - NOT the same
// endianness as the advertisement frame's own NetworkId (see ldn.c's LdnDecodeAdvertisement, which reads it
// big-endian). Layout: u64 local_communication_id, pad(2), u16 scene_id, pad(4), ssid[16].
static void _buildNetworkIdLE(uint64_t localCommunicationId, uint16_t sceneId, const uint8_t ssid[16], uint8_t out[32]) {
	memset(out, 0, 32);
	_writeU64LE(&out[0], localCommunicationId);
	_writeU16LE(&out[10], sceneId);
	memcpy(&out[16], ssid, 16);
}

// ChallengeRequest.encode(key) from ldn/__init__.py: a fixed-size (0x300 byte), HMAC-protected blob the joiner
// sends embedded in its AuthenticationRequest so the host can prove it actually generated the advertisement's
// challenge token (anti-replay). `params1`/`params2` (extra u64 lists a real Switch may attach) are left empty -
// nothing here needs them, and the reference client's own decode tolerates that.
static bool _buildChallengeRequest(uint64_t token, uint64_t nonce, uint64_t deviceId, uint8_t out[LDN_CHALLENGE_REQUEST_SIZE]) {
	uint8_t body[LDN_CHALLENGE_BODY_SIZE];
	memset(body, 0, sizeof(body));
	// body[0..1] = 0, body[2] = len(params1) = 0, body[3] = len(params2) = 0, body[4] = flags = 0, body[5..7] pad
	_writeU64LE(&body[8], token);
	_writeU64LE(&body[16], nonce);
	_writeU64LE(&body[24], deviceId);
	// body[32..47] = unk (Switch 2 only) = 0
	// body[48..143] = pad(0x60), body[144..207] = params1 zone (empty), body[208..719] = params2 zone (empty)
	uint8_t mac[32];
	if (!LdnHmacSha256(kChallengeKey, sizeof(kChallengeKey), body, sizeof(body), mac)) {
		return false;
	}
	memset(out, 0, LDN_CHALLENGE_REQUEST_SIZE);
	memcpy(&out[4], mac, 32);
	memcpy(&out[48], body, sizeof(body));
	return true;
}

// AuthenticationRequest.encode(version) for version >= 3 (all advertisements this project decodes are version 2,
// 3 or 4 - see LdnDecodeAdvertisement - so the version < 3, challenge-less form is not implemented).
static size_t _buildAuthenticationRequest(const char* username, uint16_t appVersion, const uint8_t challenge[LDN_CHALLENGE_REQUEST_SIZE],
                                          uint8_t out[LDN_AUTH_REQUEST_SIZE]) {
	memset(out, 0, LDN_AUTH_REQUEST_BASE_SIZE);
	size_t nameLength = strlen(username);
	if (nameLength > 32) {
		nameLength = 32;
	}
	memcpy(out, username, nameLength);
	_writeU16BE(&out[32], appVersion);
	out[34] = PLATFORM_NX;
	// out[35..63] pad(29), out[64..99] pad(0x24)
	memcpy(&out[LDN_AUTH_REQUEST_BASE_SIZE], challenge, LDN_CHALLENGE_REQUEST_SIZE);
	return LDN_AUTH_REQUEST_SIZE;
}

static void _buildAuthHeader(uint8_t version, uint16_t payloadLength, uint8_t statusCode, bool isResponse, uint8_t format,
                             const struct LdnAdvertisement* ad, const uint8_t clientRandom[16], uint8_t out[LDN_AUTH_HEADER_SIZE]) {
	out[0] = version;
	out[1] = (uint8_t) payloadLength;
	out[2] = statusCode;
	out[3] = isResponse ? 1 : 0;
	out[4] = (uint8_t) (payloadLength >> 8);
	out[5] = format;
	out[6] = 0;
	out[7] = 0;
	_buildNetworkIdLE(ad->localCommunicationId, ad->sceneId, ad->ssid, &out[8]);
	memcpy(&out[40], ad->serverRandom, 16);
	memcpy(&out[56], clientRandom, 16);
}

// Parses and validates a reply frame against what we sent: OUI/frame-type/format match, the network id/random
// values echo what we sent (the reference client's own _check_authentication_response does the same sanity
// checks before trusting a frame), and - for AES-GCM - the payload's authentication tag verifies. Only after all
// of that does `*outStatus` (the frame's cleartext status_code byte) become trustworthy. Returns false for a
// frame that is not a valid, matching response at all (caller should keep waiting/retry), not for one that
// carries a rejection status code (that is a successful decode with *outStatus != 0).
static bool _checkAuthResponse(const uint8_t* data, size_t length, const struct LdnAdvertisement* ad, const uint8_t clientRandom[16],
                               const uint8_t authKey[16], uint8_t* outStatus) {
	if (length < 6 + LDN_AUTH_HEADER_SIZE) {
		return false;
	}
	if (data[0] != 0x00 || data[1] != 0x22 || data[2] != 0xAA || data[3] != 0x01 || data[4] != 0x02) {
		return false;
	}
	const uint8_t* header = &data[6];
	uint8_t version = header[0];
	uint16_t lenLo = header[1];
	uint8_t statusCode = header[2];
	uint8_t isResponse = header[3];
	uint16_t lenHi = header[4];
	uint8_t format = header[5];
	if (!isResponse || version != ad->version) {
		return false;
	}
	uint8_t expectedFormat = ad->protocol == 3 ? AUTH_FORMAT_AES_GCM : AUTH_FORMAT_PLAIN;
	if (format != expectedFormat) {
		return false;
	}
	uint8_t netId[32];
	_buildNetworkIdLE(ad->localCommunicationId, ad->sceneId, ad->ssid, netId);
	if (memcmp(&header[8], netId, 32) || memcmp(&header[40], ad->serverRandom, 16) || memcmp(&header[56], clientRandom, 16)) {
		return false;
	}

	size_t offset = 6 + LDN_AUTH_HEADER_SIZE;
	const uint8_t* tag = NULL;
	if (format == AUTH_FORMAT_AES_GCM) {
		if (length < offset + 16) {
			return false;
		}
		tag = &data[offset];
		offset += 16;
	}
	uint16_t size = (uint16_t) ((lenHi << 8) | lenLo);
	if (length - offset != size) {
		return false;
	}

	if (format == AUTH_FORMAT_AES_GCM) {
		uint8_t* plain = malloc(size ? size : 1);
		if (!plain) {
			return false;
		}
		bool ok = LdnAesGcmDecrypt(authKey, header, header, LDN_AUTH_HEADER_SIZE, tag, &data[offset], size, plain);
		free(plain);
		if (!ok) {
			return false;
		}
	}
	*outStatus = statusCode;
	return true;
}

int LdnStationAuthenticate(struct LdnStation* station, uint32_t ifIndex, const uint8_t hostMac[6], const struct LdnAdvertisement* ad,
                            const struct LdnKeys* keys, const char* username, uint16_t appVersion) {
	uint8_t clientRandom[16];
	if (!LdnRandomBytes(clientRandom, sizeof(clientRandom))) {
		return LDND_ERR_ARGS;
	}
	uint64_t nonce = 0, deviceId = 0;
	LdnRandomBytes((uint8_t*) &nonce, sizeof(nonce));
	LdnRandomBytes((uint8_t*) &deviceId, sizeof(deviceId));

	uint8_t challenge[LDN_CHALLENGE_REQUEST_SIZE];
	if (!_buildChallengeRequest(ad->challenge, nonce, deviceId, challenge)) {
		return LDND_ERR_ARGS;
	}

	uint8_t payload[LDN_AUTH_REQUEST_SIZE];
	size_t payloadLength = _buildAuthenticationRequest(username, appVersion, challenge, payload);

	uint8_t format = ad->protocol == 3 ? AUTH_FORMAT_AES_GCM : AUTH_FORMAT_PLAIN;
	uint8_t header[LDN_AUTH_HEADER_SIZE];
	_buildAuthHeader(ad->version, (uint16_t) payloadLength, 0, false, format, ad, clientRandom, header);

	uint8_t authKey[16];
	if (format == AUTH_FORMAT_AES_GCM && !LdnDeriveAuthenticationKey(keys, ad->protocol, clientRandom, authKey)) {
		return LDND_ERR_ARGS;
	}

	uint8_t frame[LDN_AUTH_FRAME_MAX];
	frame[0] = 0x00;
	frame[1] = 0x22;
	frame[2] = 0xAA;
	frame[3] = 0x01;
	frame[4] = 0x02;
	frame[5] = 0;
	memcpy(&frame[6], header, LDN_AUTH_HEADER_SIZE);
	size_t frameLength = 6 + LDN_AUTH_HEADER_SIZE;
	if (format == AUTH_FORMAT_AES_GCM) {
		uint8_t tag[16];
		uint8_t ciphertext[LDN_AUTH_REQUEST_SIZE];
		if (!LdnAesGcmEncrypt(authKey, header, header, LDN_AUTH_HEADER_SIZE, payload, payloadLength, ciphertext, tag)) {
			return LDND_ERR_ARGS;
		}
		memcpy(&frame[frameLength], tag, 16);
		frameLength += 16;
		memcpy(&frame[frameLength], ciphertext, payloadLength);
		frameLength += payloadLength;
	} else {
		memcpy(&frame[frameLength], payload, payloadLength);
		frameLength += payloadLength;
	}

	// Attempt authentication up to three times, waiting 700ms each - matches the reference client
	// (ldn/__init__.py's STANetwork._authenticate).
	for (int attempt = 0; attempt < 3; ++attempt) {
		int error = LdnStationSendControlPortFrame(station, ifIndex, hostMac, frame, frameLength);
		if (error) {
			return error;
		}
		uint8_t replyMac[6];
		uint8_t replyFrame[LDN_STATION_MAX_FRAME];
		size_t replyLength = sizeof(replyFrame);
		if (LdnStationWaitControlPortFrame(station, replyMac, replyFrame, &replyLength, 700)) {
			continue;
		}
		if (memcmp(replyMac, hostMac, 6)) {
			continue;
		}
		uint8_t statusCode;
		if (_checkAuthResponse(replyFrame, replyLength, ad, clientRandom, authKey, &statusCode)) {
			return statusCode;
		}
	}
	return LDN_AUTH_NO_RESPONSE;
}

#else // !_WIN32

int LdnStationAuthenticate(struct LdnStation* station, uint32_t ifIndex, const uint8_t hostMac[6], const struct LdnAdvertisement* ad,
                            const struct LdnKeys* keys, const char* username, uint16_t appVersion) {
	(void) station;
	(void) ifIndex;
	(void) hostMac;
	(void) ad;
	(void) keys;
	(void) username;
	(void) appVersion;
	return LDND_ERR_ARGS;
}

#endif
