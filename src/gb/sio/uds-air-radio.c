/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/uds-air-radio.h>

#include <mgba/internal/gb/sio/uds-keyfile.h>

#include <stdio.h>
#include <string.h>

#define HOP_MS 400
#define HOST_SILENT_MS 6000
#define CLIENT_DATA_RATE 22 // 11 Mbit/s, in 500 kbit/s units: the rate that worked for Azahar as a client
#define BOARD_BOOT_MS 15000

static const uint8_t sChannels[3] = {1, 6, 11};
static const uint8_t sPassphrase[12] = {'T', 'R', 'L', '_', 'N', 'E', 'T', 'W', 'O', 'R', 'K', 0};

static bool _groupAddress(const uint8_t* mac) {
	return mac[0] & 1;
}

static void _deliver(struct UDSAirRadio* radio, uint8_t type, uint8_t channel, const uint8_t* transmitter, const uint8_t* destination,
                     const uint8_t* data, size_t size) {
	struct UDSRoomPacket packet;
	memset(&packet, 0, sizeof(packet));
	packet.type = type;
	packet.channel = channel;
	memcpy(packet.transmitter, transmitter, 6);
	memcpy(packet.destination, destination, 6);
	packet.data = data;
	packet.size = size;
	uint8_t datagram[UDS_BRIDGE_MAX_DATAGRAM];
	size_t length = udsRoomEncode(datagram, sizeof(datagram), &packet);
	if (length && radio->deliver) {
		++radio->framesReceived;
		radio->deliver(radio->context, datagram, length);
	}
}

static void _transmit(struct UDSAirRadio* radio, const uint8_t* frame, size_t size, uint8_t rate) {
	uint8_t flags = _groupAddress(&frame[4]) ? UDS_ESP32_TX_NO_ACK : 0;
	if (udsEsp32TxFrame(&radio->esp, flags, rate, frame, size)) {
		++radio->framesSent;
	} else {
		++radio->txFailed;
	}
}

static struct UDSAirHost* _findHost(struct UDSAirRadio* radio, const uint8_t* mac) {
	unsigned i;
	for (i = 0; i < UDS_AIR_MAX_HOSTS; ++i) {
		if (radio->hosts[i].valid && !memcmp(radio->hosts[i].mac, mac, 6)) {
			return &radio->hosts[i];
		}
	}
	return NULL;
}

static void _rememberHost(struct UDSAirRadio* radio, const uint8_t* mac, uint8_t channel, const struct UDSRoomHost* info) {
	struct UDSAirHost* host = _findHost(radio, mac);
	if (!host) {
		unsigned i;
		for (i = 0; i < UDS_AIR_MAX_HOSTS && radio->hosts[i].valid; ++i) {
		}
		if (i == UDS_AIR_MAX_HOSTS) {
			i = 0; // the table is full of other hosts: replace the oldest slot
		}
		host = &radio->hosts[i];
		memset(host, 0, sizeof(*host));
		host->valid = true;
		memcpy(host->mac, mac, 6);
	}
	host->channel = channel;
	host->commId = info->commId;
	host->networkId = info->networkId;
	host->id = info->id;
}

// Choosing the host: the room has sent its first frame to it. Tune to its channel, watch its address and make the data key.
static bool _chooseHost(struct UDSAirRadio* radio, const uint8_t* mac, uint32_t nowMs) {
	struct UDSAirHost* known = _findHost(radio, mac);
	if (!known) {
		return false;
	}
	radio->host = *known;
	radio->haveHost = true;
	radio->assocSent = false;
	radio->txPacketNumber = 1;
	radio->txSequence = 0;
	radio->haveRxPacketNumber = false;
	radio->lastHostFrameMs = nowMs;
	udsCcmpDeriveKey(radio->slotKey, sPassphrase, sizeof(sPassphrase), known->commId, known->networkId, known->mac, known->id, radio->dataKey);
	if (radio->channel != known->channel) {
		udsEsp32SetChannel(&radio->esp, known->channel);
		radio->channel = known->channel;
	}
	udsEsp32SetWatch(&radio->esp, known->mac);
	return true;
}

static void _sendAssocRequest(struct UDSAirRadio* radio) {
	uint8_t body[30], frame[UDS_80211_HEADER + 30];
	udsBuildAssocRequestBody(body, radio->host.networkId);
	size_t size = udsBuildMgmtFrame(frame, sizeof(frame), UDS_FC_ASSOC_REQUEST, radio->mac, radio->host.mac, radio->host.mac,
	                                radio->txSequence++, body, sizeof(body));
	if (size) {
		_transmit(radio, frame, size, 0);
	}
}

static void _onRx(void* context, const struct UDSEsp32Rx* rx) {
	struct UDSAirRadio* radio = context;
	const uint8_t* m = rx->mpdu;
	if (rx->length < UDS_80211_HEADER) {
		return;
	}
	uint16_t frameControl = (uint16_t) (m[0] | (m[1] << 8));
	unsigned type = (frameControl >> 2) & 3, subtype = (frameControl >> 4) & 0xF;
	const uint8_t *a1 = &m[4], *a2 = &m[10];
	uint32_t now = radio->nowMs;

	if (type == 0 && subtype == 8) { // beacon
		struct UDSRoomHost info;
		memset(&info, 0, sizeof(info));
		if (rx->length >= UDS_80211_HEADER + 12 && udsRoomParseBeacon(m + UDS_80211_HEADER, rx->length - UDS_80211_HEADER, &info)) {
			++radio->beaconsSeen;
			_rememberHost(radio, a2, rx->channel, &info);
			if (radio->haveHost && !memcmp(a2, radio->host.mac, 6)) {
				radio->lastHostFrameMs = now;
			}
			_deliver(radio, UDS_PACKET_BEACON, rx->channel, a2, a1, m + UDS_80211_HEADER, rx->length - UDS_80211_HEADER);
		}
		return;
	}
	if (!radio->haveHost || memcmp(a2, radio->host.mac, 6)) {
		return; // not from the host we are joining
	}
	if (memcmp(a1, radio->mac, 6) && !_groupAddress(a1)) {
		return; // for someone else
	}
	radio->lastHostFrameMs = now;
	const uint8_t* body = m + UDS_80211_HEADER;
	size_t bodyLength = rx->length - UDS_80211_HEADER;

	if (type == 0) {
		if (subtype == 0xB && bodyLength >= 6) { // authentication
			_deliver(radio, UDS_PACKET_AUTH, rx->channel, a2, a1, body, bodyLength);
			if (body[2] == 2 && !radio->assocSent) { // SEQ2: now the association request, which the room does not know about
				radio->assocSent = true;
				_sendAssocRequest(radio);
			}
		} else if (subtype == 1 && bodyLength >= 6) { // association response
			_deliver(radio, UDS_PACKET_ASSOC_RESPONSE, rx->channel, a2, a1, body, bodyLength);
		} else if (subtype == 0xC) { // deauthentication
			radio->assocSent = false;
			_deliver(radio, UDS_PACKET_DEAUTH, rx->channel, a2, a1, body, bodyLength);
		}
		return;
	}
	if (type == 2 && (frameControl & 0x4000)) { // protected data
		uint8_t plain[UDS_BRIDGE_MAX_DATAGRAM];
		size_t plainLength = 0;
		struct UDSDataFrameInfo info;
		if (rx->length > sizeof(plain) + UDS_DATA_OVERHEAD) {
			++radio->droppedOther;
			return;
		}
		if (!udsOpenDataFrame(radio->dataKey, m, rx->length, plain, &plainLength, &info)) {
			++radio->droppedDecrypt;
			return;
		}
		// The host retransmits what the board did not acknowledge: the retries repeat the packet number.
		if (radio->haveRxPacketNumber && info.packetNumber <= radio->lastRxPacketNumber) {
			++radio->droppedReplay;
			return;
		}
		radio->haveRxPacketNumber = true;
		radio->lastRxPacketNumber = info.packetNumber;
		_deliver(radio, UDS_PACKET_DATA, rx->channel, a2, a1, plain, plainLength);
		return;
	}
	++radio->droppedOther;
}

static void _onStatus(void* context, uint8_t requestType, int32_t result) {
	(void) context;
	(void) requestType;
	(void) result;
}

bool udsAirRadioOpen(struct UDSAirRadio* radio, const char* portName, const char* keyPath, const uint8_t mac[6], UDSAirDeliver deliver,
                     void* context, char* error, size_t errorSize) {
	memset(radio, 0, sizeof(*radio));
	memcpy(radio->mac, mac, 6);
	radio->deliver = deliver;
	radio->context = context;
	enum UDSKeyStatus keyStatus = udsKeyFileLoad(keyPath, radio->slotKey);
	if (!udsKeyStatusOk(keyStatus)) {
		snprintf(error, errorSize, "UDS key file: %s", udsKeyStatusText(keyStatus));
		return false;
	}
	struct UDSEsp32Handlers handlers = {radio, _onRx, _onStatus, NULL, NULL};
	if (!udsEsp32Open(&radio->esp, portName, &handlers)) {
		snprintf(error, errorSize, "no ESP32 board found%s%s", portName && *portName ? " on " : "", portName && *portName ? portName : "");
		return false;
	}
	radio->open = true;
	radio->state = UDS_AIR_BOOTING;
	radio->channel = sChannels[0];
	return true;
}

bool udsAirRadioReady(const struct UDSAirRadio* radio) {
	return radio->open && radio->state == UDS_AIR_READY;
}

void udsAirRadioClose(struct UDSAirRadio* radio) {
	if (radio->open) {
		if (radio->state == UDS_AIR_READY) {
			udsEsp32Stop(&radio->esp);
		}
		udsEsp32Poll(&radio->esp);
		udsEsp32Close(&radio->esp);
		radio->open = false;
	}
	memset(radio->slotKey, 0, sizeof(radio->slotKey));
	memset(radio->dataKey, 0, sizeof(radio->dataKey));
}

void udsAirRadioPoll(struct UDSAirRadio* radio, uint32_t nowMs) {
	if (!radio->open || radio->state == UDS_AIR_FAILED) {
		return;
	}
	radio->nowMs = nowMs;
	if (!udsEsp32Poll(&radio->esp) && radio->state != UDS_AIR_FAILED) {
		radio->state = UDS_AIR_FAILED;
		snprintf(radio->error, sizeof(radio->error), "the serial port failed (is the board still plugged in?)");
		return;
	}
	if (radio->state == UDS_AIR_BOOTING) {
		if (!radio->bootMs) {
			radio->bootMs = nowMs ? nowMs : 1;
		}
		if (radio->esp.info.valid) {
			if (udsEsp32Start(&radio->esp, radio->channel, radio->mac, true)) {
				radio->state = UDS_AIR_READY;
				radio->lastHopMs = nowMs;
			} else {
				radio->state = UDS_AIR_FAILED;
				snprintf(radio->error, sizeof(radio->error), "could not start the radio");
			}
		} else if (nowMs - radio->bootMs > BOARD_BOOT_MS) {
			radio->state = UDS_AIR_FAILED;
			snprintf(radio->error, sizeof(radio->error),
			         "the board did not answer Hello: it must run Azahar's esp32-uds-bridge firmware (not the GB-Link LDN one)");
		} else if (!radio->lastHelloMs || nowMs - radio->lastHelloMs >= 500) {
			udsEsp32SendHello(&radio->esp);
			radio->lastHelloMs = nowMs ? nowMs : 1;
		}
		return;
	}
	if (radio->state != UDS_AIR_READY) {
		return;
	}
	if (radio->haveHost) {
		if (nowMs - radio->lastHostFrameMs > HOST_SILENT_MS) {
			radio->haveHost = false; // the host is gone: scan again
			udsEsp32SetWatch(&radio->esp, (const uint8_t[6]) {0, 0, 0, 0, 0, 0});
		}
	} else if (nowMs - radio->lastHopMs >= HOP_MS) {
		radio->hopIndex = (radio->hopIndex + 1) % 3;
		radio->channel = sChannels[radio->hopIndex];
		udsEsp32SetChannel(&radio->esp, radio->channel);
		radio->lastHopMs = nowMs;
	}
}

void udsAirRadioSend(struct UDSAirRadio* radio, const uint8_t* datagram, size_t size) {
	if (!radio->open || radio->state != UDS_AIR_READY) {
		return;
	}
	struct UDSRoomPacket packet;
	if (!udsRoomDecode(datagram, size, &packet)) {
		return;
	}
	uint32_t now = radio->nowMs;
	if (packet.type == UDS_PACKET_AUTH) {
		if (!radio->haveHost || memcmp(radio->host.mac, packet.destination, 6)) {
			if (!_chooseHost(radio, packet.destination, now)) {
				++radio->droppedOther;
				return;
			}
		}
		uint8_t frame[UDS_80211_HEADER + 16];
		size_t length = udsBuildMgmtFrame(frame, sizeof(frame), UDS_FC_AUTH, radio->mac, radio->host.mac, radio->host.mac,
		                                  radio->txSequence++, packet.data, packet.size > 16 ? 16 : packet.size);
		if (length) {
			_transmit(radio, frame, length, 0);
		}
		return;
	}
	if (packet.type == UDS_PACKET_DATA) {
		if (!radio->haveHost) {
			++radio->droppedNoKey;
			return;
		}
		uint8_t frame[UDS_BRIDGE_MAX_DATAGRAM + UDS_DATA_OVERHEAD];
		size_t length = udsBuildDataFrame(frame, sizeof(frame), radio->dataKey, packet.data, packet.size, radio->mac, radio->host.mac,
		                                  radio->host.mac, UDS_DS_TO, radio->txPacketNumber++, radio->txSequence++);
		if (length) {
			_transmit(radio, frame, length, CLIENT_DATA_RATE);
		}
	}
}
