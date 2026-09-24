/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * ldn-pia-join: scans for a Switch's FRLG LDN advertisement, associates (WPA2), authenticates (LDN's own
 * handshake), resolves both sides' LDN IP addresses from a post-join advertisement, then brings up and drives the
 * Pia CONNECTION layer (Net/Session/RTT - see ldn-pia-connect.c) to see whether the host accepts this joiner into
 * a Pia session. Does NOT yet bridge any RFU/game data (see ldn-pia-reliable.c's LdnPiaReliableSend/Receive,
 * unused here) - this tool's only job is to find out whether ST_CONNECTED is reachable at all, and if not, at
 * which stage it stalls, exactly the way ldn-join.c tested Wi-Fi association and authentication in isolation
 * before either was wired into the real backend.
 *
 *   ldn-pia-join --keys <prod.keys> [--seconds N]
 */
#include "ldn-auth.h"
#include "ldn-monitor.h"
#include "ldn-pia-connect.h"
#include "ldn-pia-reliable.h"
#include "ldn-pia.h"
#include "ldn-station.h"
#include "ldn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

struct Found {
	bool haveIt;
	uint8_t mac[6];
	unsigned channel;
	struct LdnAdvertisement ad;
};

struct ScanState {
	struct Found found;
	struct LdnKeys keys;
};

static void _onAdvertisement(void* context, const uint8_t* mac, const uint8_t* body, size_t bodyLength, unsigned channel) {
	struct ScanState* state = context;
	if (state->found.haveIt) {
		return;
	}
	struct LdnAdvertisement ad;
	if (!LdnDecodeAdvertisement(body, bodyLength, &state->keys, &ad) || !ad.infoDecoded) {
		return;
	}
	printf("Found it: \"%s\" on channel %u\n", ad.participants[0].present ? ad.participants[0].name : "?", channel);
	memcpy(state->found.mac, mac, 6);
	state->found.channel = channel;
	state->found.ad = ad;
	state->found.haveIt = true;
}

#ifdef _WIN32
// Sends one Pia message as its own datagram (no batching - simpler, still spec-valid since a datagram may carry
// any number of tiled messages including one). Maintains a strictly-increasing 8-byte nonce counter, required by
// the real console (see the project notes: it drops any datagram whose header nonce is not strictly above the
// last one it accepted).
struct PiaSender {
	struct LdnPiaSocket* socket;
	struct LdnPiaCrypto* crypto;
	uint8_t ourIp[4];
	uint8_t hostIp[4];
	uint8_t hostMac[6];
	uint16_t hostVar; // the footer value - learned dynamically from the host's own Net 0x11 (see LdnPiaConnect),
	                  // NOT the crypto.py default constant (0x7620) that only happens to match it in practice
	uint64_t nonceCounter;
	uint16_t pktidCounter;
};

// The largest single tiled message this tool ever sends: a Reliable(10) frame wrapping up to
// LDN_PIA_RELIABLE_MAX_PAYLOAD bytes of inner payload (8-byte sub-header + payload), plus this layer's own
// 5-byte tiling header, a 2-byte footer and up to 15 bytes of 0xFF padding.
enum { PIA_JOIN_MAX_TILED = 5 + 8 + LDN_PIA_RELIABLE_MAX_PAYLOAD + 2 + 16 };

static bool _sendRaw(struct PiaSender* sender, uint8_t proto, uint16_t dst, uint16_t src, bool establishing, bool footer, bool compress,
                     const uint8_t* payload, size_t length) {
	uint8_t tiled[PIA_JOIN_MAX_TILED];
	size_t tiledLength = LdnPiaBuildMessage(proto, payload, length, false, 0, tiled);
	bool compressed = false;
	if (compress) {
		// Compress the tiled message blob itself (proto+payload, before any footer/padding) - mirrors how the
		// host's own compressed messages decode: LdnPiaDecompress runs on the raw decrypted plaintext, BEFORE
		// LdnPiaParseMessages, so the frame's decompressed content IS the tiled blob with no footer/padding
		// inside it. A compressed message is always small enough that the compressed form fits comfortably in
		// the same `tiled` buffer (zstd's per-frame overhead is a few bytes, nowhere near this buffer's slack).
		uint8_t compbuf[sizeof(tiled)];
		size_t compLength = sizeof(compbuf);
		if (LdnPiaCompress(tiled, tiledLength, compbuf, &compLength)) {
			memcpy(tiled, compbuf, compLength);
			tiledLength = compLength;
			compressed = true;
		}
	}
	if (footer) {
		tiled[tiledLength++] = (uint8_t) (sender->hostVar >> 8);
		tiled[tiledLength++] = (uint8_t) sender->hostVar;
	}
	size_t beforePad = tiledLength;
	while (tiledLength % 16 != 0) {
		tiled[tiledLength++] = 0xFF;
	}
	uint8_t pad = (uint8_t) (tiledLength - beforePad);

	struct LdnPiaHeader header;
	header.dst = dst;
	header.src = src;
	header.pktid = establishing ? 0 : sender->pktidCounter++;
	header.enc = 0x90;
	// NOT a fixed constant: byte5 = (padding_size << 4) | (1 if zstd-compressed) | (2 if establishing) - matches
	// pokeldn/frlgsim's `sim.py` `_send_messages()` exactly, and independently confirmed against the live host's
	// own captured Net 0x11 (pad=8, compressed+establishing -> 0x83, the exact byte observed on the wire). The
	// project's earlier "flags 0x50 observed, not otherwise interpreted" comment described only the coincidental
	// value for a specific uncompressed/non-establishing/5-byte-pad message, not a real constant.
	header.flags = (uint8_t) ((pad << 4) | (compressed ? 1 : 0) | (establishing ? 2 : 0));
	header.footer = footer ? 2 : 0;
	++sender->nonceCounter;
	for (int i = 0; i < 8; ++i) {
		header.nonce8[i] = (uint8_t) (sender->nonceCounter >> (8 * (7 - i)));
	}

	uint8_t datagram[LDN_PIA_CIPHERTEXT_OFFSET + sizeof(tiled)];
	size_t datagramLength;
	if (!LdnPiaEncrypt(sender->crypto, tiled, tiledLength, sender->ourIp, &header, datagram, &datagramLength)) {
		printf("  [send] proto=%u dst=%04x src=%04x compress=%d(actual=%d) ENCRYPT FAILED\n", proto, dst, src, compress, compressed);
		return false;
	}
	int rc = LdnPiaSocketSend(sender->socket, sender->hostMac, sender->ourIp, sender->hostIp, datagram, datagramLength);
	printf("  [send] proto=%u dst=%04x src=%04x compress=%d(actual=%d) tiled=%zu datagram=%zu rc=%d: ", proto, dst, src, compress, compressed,
	       tiledLength, datagramLength, rc);
	for (size_t h = 0; h < datagramLength; ++h) {
		printf("%02x", datagram[h]);
	}
	printf("\n");
	return rc == 0;
}

static bool _sendMessage(struct PiaSender* sender, const struct LdnPiaOutMessage* msg) {
	return _sendRaw(sender, msg->proto, msg->dst, msg->src, msg->establishing, msg->footer, msg->compress, msg->payload, msg->length);
}

static void _sendAll(struct PiaSender* sender, struct LdnPiaOutMessage* messages, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		if (!_sendMessage(sender, &messages[i])) {
			printf("  (send failed: %s)\n", LdnPiaSocketLastError());
		}
	}
}
#endif

int main(int argc, char** argv) {
	const char* keysPath = NULL;
	unsigned seconds = 30;
	unsigned channels[8] = {1, 6, 11};
	unsigned channelCount = 3;

	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
			keysPath = argv[++i];
		} else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			seconds = (unsigned) atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--debug")) {
			gLdnDebug = 1;
		}
	}
	if (!keysPath) {
		fprintf(stderr, "--keys <prod.keys> is required.\n");
		return 1;
	}

	struct ScanState state = {0};
	if (!LdnKeysLoad(keysPath, &state.keys)) {
		fprintf(stderr, "Could not read the four LDN keys from \"%s\".\n", keysPath);
		return 1;
	}

	struct LdnMonitor* monitor = LdnMonitorOpen(NULL, channels[0], NULL, _onAdvertisement, &state);
	if (!monitor) {
		fprintf(stderr, "%s\n", LdnMonitorLastError());
		return 1;
	}
	printf("Scanning for up to %u seconds...\n", seconds);
	fflush(stdout);

#ifdef _WIN32
	DWORD end = GetTickCount() + seconds * 1000;
	unsigned hop = 1;
	while (!state.found.haveIt && (int32_t) (end - GetTickCount()) > 0) {
		unsigned channel = channels[hop++ % channelCount];
		LdnMonitorSetChannel(monitor, channel);
		Sleep(400);
	}
#endif

	if (!state.found.haveIt) {
		LdnMonitorClose(monitor);
		printf("Nothing found.\n");
		return 1;
	}

	uint8_t wlanKey[16];
	if (!LdnDeriveWlanKey(&state.keys, state.found.ad.protocol, state.found.ad.serverRandom, wlanKey)) {
		fprintf(stderr, "Could not derive the WLAN key.\n");
		return 1;
	}
	char ssid[33];
	LdnAdvertisementWlanSsid(&state.found.ad, ssid);
	printf("SSID: %s\n", ssid);

	struct LdnStation* station = LdnStationOpen(LdnMonitorConnection(monitor));
	if (!station) {
		fprintf(stderr, "%s\n", LdnStationLastError());
		LdnMonitorClose(monitor);
		return 1;
	}
	uint32_t ifIndex;
	uint8_t ourMac[6];
	if (LdnStationFindInterface(station, &ifIndex, ourMac)) {
		fprintf(stderr, "%s\n", LdnStationLastError());
		LdnStationClose(station);
		LdnMonitorClose(monitor);
		return 1;
	}
	printf("Station interface index %u (our MAC %02X:%02X:%02X:%02X:%02X:%02X). Associating on channel %u...\n", ifIndex, ourMac[0], ourMac[1],
	       ourMac[2], ourMac[3], ourMac[4], ourMac[5], state.found.channel);
	fflush(stdout);

	uint8_t hostMac[6];
	int error = LdnStationConnect(station, ifIndex, ssid, state.found.channel, wlanKey, state.found.mac, hostMac);
	if (error) {
		fprintf(stderr, "ASSOCIATION FAILED: %s\n", LdnStationLastError());
		LdnStationClose(station);
		LdnMonitorClose(monitor);
		return 1;
	}
	printf("ASSOCIATED with %02X:%02X:%02X:%02X:%02X:%02X. Authenticating...\n", hostMac[0], hostMac[1], hostMac[2], hostMac[3], hostMac[4],
	       hostMac[5]);
	fflush(stdout);

	int authStatus = LdnStationAuthenticate(station, ifIndex, hostMac, &state.found.ad, &state.keys, "mGBA", state.found.ad.appVersion);
	if (authStatus != LDN_AUTH_SUCCESS) {
		fprintf(stderr, "AUTHENTICATION FAILED (status %d): %s\n", authStatus, LdnStationLastError());
		LdnStationDisconnect(station, ifIndex);
		LdnStationClose(station);
		LdnMonitorClose(monitor);
		return 1;
	}
	printf("Deriving our own and the host's LDN IP addresses...\n");
	fflush(stdout);

#ifdef _WIN32
	// NOT re-scanning for a post-join advertisement to learn these, on purpose: an earlier version of this tool
	// did (closing and reopening the monitor - a fresh vif plus an explicit channel-set - while the station stayed
	// associated), and that reliably wedged the radio (nl80211 EBUSY on the channel switch, and on live hardware
	// this actually left the WHOLE RADIO stuck association-side too, breaking even a completely fresh run
	// afterwards, until ldnd itself was restarted). ldn-join.c and every prior live test never hit this because
	// they never close and recreate a SECOND monitor after the station is already associated - a monitor vif
	// coexisting quietly with an associated station vif is fine; re-creating one and issuing a fresh SET_CHANNEL
	// once the station already owns the radio's channel is not, on this hardware.
	//
	// Instead: `host_ip` comes straight from the PRE-JOIN advertisement already decoded during the scan above
	// (`participants[0].ip` - the host is always participant 0, LdnAdvertisement's format has carried this from
	// the very first scan this project ever ran). `our_ip` is assumed to be that same /24 subnet with the last
	// octet 2 - the same fallback the LDN-0.0.3 reference client's own LiveTransport uses when it cannot read the
	// address off its own interface (`host_ip.rsplit(".", 1)[0] + ".2"`), and the only sane value in this
	// single-joiner scenario in any case (the host is always assigned .1, the first joiner always .2).
	uint8_t ourIp[4], hostIp[4];
	memcpy(hostIp, state.found.ad.participants[0].ip, 4);
	memcpy(ourIp, hostIp, 3);
	ourIp[3] = 2;
	printf("host IP (from the advertisement): %u.%u.%u.%u  our IP (assumed): %u.%u.%u.%u\n", hostIp[0], hostIp[1], hostIp[2], hostIp[3], ourIp[0],
	       ourIp[1], ourIp[2], ourIp[3]);
	fflush(stdout);

	// Shares the monitor's still-open ldnd connection rather than opening a second one (ldnd only accepts one
	// pipe connection at a time; `station` already shares it too via the same multi-callback mechanism - see
	// ldnd.h). No connection churn at all now, which is exactly what avoided the radio-wedging problem above.
	struct LdnPiaSocket* piaSocket = LdnPiaSocketOpen(LdnMonitorConnection(monitor), ifIndex, ourMac);
	if (!piaSocket) {
		fprintf(stderr, "could not open the Pia raw socket: %s\n", LdnPiaSocketLastError());
		LdnStationDisconnect(station, ifIndex);
		LdnStationClose(station);
		LdnMonitorClose(monitor);
		return 1;
	}

	struct LdnPiaCrypto crypto;
	LdnPiaCryptoInit(&crypto, state.found.ad.ssid);
	struct LdnPiaConnect conn;
	LdnPiaConnectInit(&conn, ourMac, hostMac, ourIp, "mGBA");

	struct PiaSender sender;
	sender.socket = piaSocket;
	sender.crypto = &crypto;
	memcpy(sender.ourIp, ourIp, 4);
	memcpy(sender.hostIp, hostIp, 4);
	memcpy(sender.hostMac, hostMac, 6);
	sender.hostVar = 0x7620; // a starting guess (crypto.py's STATION_HOST default) until the host's own Net 0x11 arrives
	sender.nonceCounter = 0;
	sender.pktidCounter = 1;

	struct LdnPiaReliable* reliable = malloc(sizeof(*reliable)); // too large for the stack - see the project notes
	LdnPiaReliableInit(reliable, LDN_PIA_RELIABLE_RTO_BASE_MS, 200);
	bool openedStream = false;

	printf("Waiting for the host's Pia connection handshake (Net 0x11)...\n");
	fflush(stdout);

	int lastState = -1;
	DWORD tickDeadline = GetTickCount() + seconds * 1000;
	DWORD connectedSince = 0;
	unsigned tick = 0;
	unsigned rawReceived = 0;
	while ((int32_t) (tickDeadline - GetTickCount()) > 0) {
		uint8_t datagram[LDN_PIA_MAX_DATAGRAM]; // matches LdnPiaSocketPoll's own buffer sizing (ldn-pia.c)
		uint8_t srcIp[4];
		size_t datagramLength = sizeof(datagram);
		while (LdnPiaSocketPoll(piaSocket, srcIp, datagram, &datagramLength)) {
			uint8_t plain[LDN_PIA_MAX_DATAGRAM];
			size_t plainLength;
			++rawReceived;
			printf("  [raw] #%u from %u.%u.%u.%u, %zu bytes: ", rawReceived, srcIp[0], srcIp[1], srcIp[2], srcIp[3], datagramLength);
			for (size_t h = 0; h < datagramLength && h < 32; ++h) {
				printf("%02x", datagram[h]);
			}
			printf("%s\n", datagramLength > 32 ? "..." : "");
			if (LdnPiaDecrypt(&crypto, datagram, datagramLength, srcIp, plain, &plainLength)) {
				printf("    decrypted %zu bytes: ", plainLength);
				for (size_t h = 0; h < plainLength; ++h) {
					printf("%02x", plain[h]);
				}
				printf("\n");

				// The decrypted plaintext may itself be a zstd frame - live-confirmed: the real Switch host sends
				// its Net/Session connection-layer messages this way, not just game data (the plaintext dump above
				// began with 28 b5 2f fd, zstd's magic number, the first time this was observed). Always run it
				// through LdnPiaDecompress before tiling: non-compressed payloads pass through unchanged.
				uint8_t decompressed[8192];
				size_t decompressedLength = sizeof(decompressed);
				if (!LdnPiaDecompress(plain, plainLength, decompressed, &decompressedLength)) {
					printf("    FAILED TO DECOMPRESS (zstd frame, decode error)\n");
					datagramLength = sizeof(datagram);
					continue;
				}
				if (decompressedLength != plainLength) {
					printf("    decompressed to %zu bytes: ", decompressedLength);
					for (size_t h = 0; h < decompressedLength; ++h) {
						printf("%02x", decompressed[h]);
					}
					printf("\n");
				}

				struct LdnPiaMessage messages[8];
				size_t consumed;
				size_t n = LdnPiaParseMessages(decompressed, decompressedLength, messages, 8, &consumed);
				printf("    %zu tiled message(s), consumed=%zu\n", n, consumed);
				for (size_t i = 0; i < n; ++i) {
					printf("    msg[%zu]: proto=%u %zu byte(s): ", i, messages[i].proto, messages[i].payloadLength);
					for (size_t h = 0; h < messages[i].payloadLength && h < 24; ++h) {
						printf("%02x", messages[i].payload[h]);
					}
					printf("%s\n", messages[i].payloadLength > 24 ? "..." : "");
					if (messages[i].proto == LDN_PIA_PROTO_RELIABLE) {
						struct LdnPiaReliableFrame frame;
						if (LdnPiaParseReliableFrame(messages[i].payload, messages[i].payloadLength, &frame)) {
							struct LdnPiaReliableEntry delivered[8];
							size_t nd = LdnPiaReliableReceive(reliable, &frame, GetTickCount(), delivered, 8);
							for (size_t d = 0; d < nd; ++d) {
								printf("  [reliable] delivered seq=%u %zu byte(s)\n", delivered[d].seq, delivered[d].length);
							}
						}
					} else {
						LdnPiaConnectOnMessage(&conn, messages[i].proto, messages[i].payload, messages[i].payloadLength);
					}
				}
			} else {
				printf("    FAILED TO DECRYPT\n");
			}
			datagramLength = sizeof(datagram);
		}

		if (conn.haveHostVar) {
			sender.hostVar = conn.hostVar; // may have just been learned from this tick's incoming Net 0x11
		}

		if (conn.state != lastState) {
			static const char* names[] = {"NET", "FINALIZE", "CONNECTED"};
			printf("[state] -> %s\n", names[conn.state]);
			fflush(stdout);
			lastState = conn.state;
			if (conn.state == LDN_PIA_CONNECT_ST_CONNECTED) {
				connectedSince = GetTickCount();
			}
		}

		if (LdnPiaConnectIsConnected(&conn) && !openedStream) {
			uint16_t seq;
			LdnPiaReliableOpen(reliable, kLdnPiaMetadataFrame, sizeof(kLdnPiaMetadataFrame), GetTickCount(), &seq);
			openedStream = true;
			printf("[reliable] opened the stream with the metadata frame (seq %u)\n", seq);
		}

		LdnPiaConnectTick(&conn, tick++);

		struct LdnPiaOutMessage outMsgs[4];
		size_t nOut = LdnPiaConnectDrain(&conn, outMsgs, 4);
		_sendAll(&sender, outMsgs, nOut);

		if (openedStream) {
			struct LdnPiaReliableEntry due[8];
			size_t nDue = LdnPiaReliablePoll(reliable, GetTickCount(), due, 8);
			for (size_t i = 0; i < nDue; ++i) {
				uint8_t inner[8 + LDN_PIA_RELIABLE_MAX_PAYLOAD];
				size_t innerLength = LdnPiaBuildReliableFrame(due[i].seq, LdnPiaReliableSendLow(reliable), due[i].flagsA, due[i].payload,
				                                              due[i].length, inner);
				_sendRaw(&sender, LDN_PIA_PROTO_RELIABLE, conn.hostVar, conn.ourVar, false, true, false, inner, innerLength);
			}
		}

		if (connectedSince && GetTickCount() - connectedSince > 5000) {
			printf("Connected and stable for 5s - stopping.\n");
			break;
		}
		Sleep(16);
	}

	printf("Raw datagrams received: %u\n", rawReceived);
	printf("Final state: %s\n", conn.state == LDN_PIA_CONNECT_ST_NET      ? "NET (never got a Net 0x11 / never progressed)"
	                            : conn.state == LDN_PIA_CONNECT_ST_FINALIZE ? "FINALIZE (joined but did not finish)"
	                                                                        : "CONNECTED");

	free(reliable);
	LdnPiaSocketClose(piaSocket);
	// A courtesy WPA2 deauth - IMPORTANT to always do this before exiting: an association left up when this
	// process just exits/crashes persists at the kernel/driver level (it is not tied to our pipe connection's
	// lifetime) and reliably wedges the radio for every later run, association and even a fresh scan's monitor
	// mode included, until ldnd itself is restarted. Measured live this session.
	LdnStationDisconnect(station, ifIndex);
	LdnStationClose(station);
	LdnMonitorClose(monitor);
	return conn.state == LDN_PIA_CONNECT_ST_CONNECTED ? 0 : 1;
#else
	LdnStationDisconnect(station, ifIndex);
	LdnStationClose(station);
	LdnMonitorClose(monitor);
	return 1;
#endif
}
