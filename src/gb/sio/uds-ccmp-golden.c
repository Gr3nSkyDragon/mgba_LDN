/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Golden-frame check for uds-ccmp.c against an Azahar log (R0 of the real-air stage). Azahar's physical-UDS trace lines carry both
 * the plaintext it handed to its CCMP code and the 802.11 frame that came out ("UDS DATA TRACE TX MPDU: ... plaintext=...,
 * mpdu=..."), and the association request line carries its frame. For every such line this tool
 *   - decrypts the logged frame with the key derived here and compares it with the logged plaintext,
 *   - rebuilds the frame from the plaintext with the same addresses, packet number and sequence and compares all bytes.
 * The association request frame is rebuilt from its network id and compared too. Prints counts only, never key material.
 *
 * Usage: uds-ccmp-golden <key file> <azahar log> <network id, hex> [id (default 1)] [comm id, hex (default 171010)]
 * The key file is the one mGBA's setting points at (see uds-keyfile.h). Build target: uds-ccmp-golden.
 */
#include <mgba/internal/gb/sio/uds-ccmp.h>
#include <mgba/internal/gb/sio/uds-keyfile.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t parseHex(const char* text, uint8_t* out, size_t capacity) {
	size_t count = 0;
	while (count < capacity) {
		unsigned value;
		if (sscanf(text, "%2x", &value) != 1) {
			break;
		}
		out[count++] = (uint8_t) value;
		text += 2;
		if (*text != ':') {
			break;
		}
		++text;
	}
	return count;
}

static const char* field(const char* line, const char* name) {
	const char* at = strstr(line, name);
	return at ? at + strlen(name) : NULL;
}

int main(int argc, char** argv) {
	if (argc < 4) {
		printf("usage: %s <key file> <azahar log> <network id hex> [id] [comm id hex]\n", argv[0]);
		return 2;
	}
	uint8_t slotKey[16];
	enum UDSKeyStatus status = udsKeyFileLoad(argv[1], slotKey);
	if (!udsKeyStatusOk(status)) {
		printf("key file: %s\n", udsKeyStatusText(status));
		return 2;
	}
	uint32_t networkId = (uint32_t) strtoul(argv[3], NULL, 16);
	uint8_t id = argc > 4 ? (uint8_t) strtoul(argv[4], NULL, 0) : 1;
	uint32_t commId = argc > 5 ? (uint32_t) strtoul(argv[5], NULL, 16) : 0x00171010;
	FILE* log = fopen(argv[2], "rb");
	if (!log) {
		printf("cannot open %s\n", argv[2]);
		return 2;
	}

	static const uint8_t phrase[12] = {'T', 'R', 'L', '_', 'N', 'E', 'T', 'W', 'O', 'R', 'K', 0};
	uint8_t dataKey[16];
	bool haveKey = false;
	unsigned lines = 0, decryptOk = 0, decryptBad = 0, rebuildOk = 0, rebuildBad = 0, assocOk = 0, assocBad = 0;
	static char line[16384];
	while (fgets(line, sizeof(line), log)) {
		bool data = strstr(line, "UDS DATA TRACE TX MPDU:") != NULL;
		bool assoc = strstr(line, "UDS JOIN TRACE TX ASSOCIATION REQUEST:") != NULL;
		if (!data && !assoc) {
			continue;
		}
		const char* mpduText = field(line, "mpdu=");
		if (!mpduText) {
			continue;
		}
		static uint8_t mpdu[2600], plain[2600], expect[2600], rebuilt[2600];
		size_t mpduLength = parseHex(mpduText, mpdu, sizeof(mpdu));
		if (mpduLength < UDS_80211_HEADER) {
			continue;
		}
		if (assoc) {
			uint8_t body[30];
			size_t bodyLength = udsBuildAssocRequestBody(body, networkId);
			uint8_t transmitter[6], host[6];
			memcpy(transmitter, &mpdu[10], 6);
			memcpy(host, &mpdu[4], 6);
			uint16_t sequence = (uint16_t) ((mpdu[22] | (mpdu[23] << 8)) >> 4);
			size_t size = udsBuildMgmtFrame(rebuilt, sizeof(rebuilt), UDS_FC_ASSOC_REQUEST, transmitter, host, host, sequence, body,
			                                bodyLength);
			if (size == mpduLength && !memcmp(rebuilt, mpdu, size)) {
				++assocOk;
			} else {
				++assocBad;
			}
			continue;
		}
		const char* plainText = field(line, "plaintext=");
		if (!plainText) {
			continue;
		}
		size_t plainLength = parseHex(plainText, expect, sizeof(expect));
		++lines;
		if (!haveKey) {
			udsCcmpDeriveKey(slotKey, phrase, sizeof(phrase), commId, networkId, &mpdu[16], id, dataKey);
			haveKey = true;
		}
		struct UDSDataFrameInfo info;
		size_t openedLength = 0;
		if (udsOpenDataFrame(dataKey, mpdu, mpduLength, plain, &openedLength, &info) && openedLength == plainLength &&
		    !memcmp(plain, expect, plainLength)) {
			++decryptOk;
		} else {
			++decryptBad;
			if (decryptBad <= 3) {
				printf("line %u: the logged frame does not decrypt to the logged plaintext\n", lines);
			}
			continue;
		}
		enum UDSDsMode mode = (info.frameControl & 0x0100) ? UDS_DS_TO : (info.frameControl & 0x0200) ? UDS_DS_FROM : UDS_DS_NONE;
		const uint8_t *transmitter, *destination, *bssid;
		if (mode == UDS_DS_NONE) {
			destination = info.a1;
			transmitter = info.a2;
			bssid = info.a3;
		} else if (mode == UDS_DS_TO) {
			bssid = info.a1;
			transmitter = info.a2;
			destination = info.a3;
		} else {
			destination = info.a1;
			bssid = info.a2;
			transmitter = info.a3;
		}
		size_t size = udsBuildDataFrame(rebuilt, sizeof(rebuilt), dataKey, expect, plainLength, transmitter, destination, bssid, mode,
		                                info.packetNumber, (uint16_t) (info.sequenceControl >> 4));
		if (size == mpduLength && !memcmp(rebuilt, mpdu, size)) {
			++rebuildOk;
		} else {
			++rebuildBad;
			if (rebuildBad <= 3) {
				printf("line %u: the rebuilt frame differs from the logged one (%zu vs %zu bytes)\n", lines, size, mpduLength);
			}
		}
	}
	fclose(log);
	memset(slotKey, 0, sizeof(slotKey));
	memset(dataKey, 0, sizeof(dataKey));
	printf("data frames: %u; decrypt to the logged plaintext: %u ok, %u wrong; rebuilt identical: %u ok, %u differ\n", lines, decryptOk,
	       decryptBad, rebuildOk, rebuildBad);
	printf("association request frames: %u identical, %u differ\n", assocOk, assocBad);
	return (decryptBad || rebuildBad || assocBad || !lines) ? 1 : 0;
}
