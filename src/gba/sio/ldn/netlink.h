/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_LDN_NETLINK_H
#define GBA_SIO_LDN_NETLINK_H

#include "ldnd.h"

#include <stdbool.h>

/*
 * Linux netlink messages and a netlink client that runs over an ldnd virtual socket. Enough to resolve a generic
 * netlink family (e.g. "nl80211") and send it commands, or to send rtnetlink requests, collecting the replies of
 * single requests and dumps.
 *
 * ldnd delivers the data of *all* of a connection's sockets to one callback, so whoever owns the connection routes
 * each DATA frame to NlInput() of the client whose socket it belongs to.
 */

enum {
	NL_MSG_ERROR = 2,
	NL_MSG_DONE = 3,

	NL_F_REQUEST = 0x01,
	NL_F_MULTI = 0x02,
	NL_F_ACK = 0x04,
	NL_F_DUMP = 0x300,

	NL_PROTOCOL_ROUTE = 0,
	NL_PROTOCOL_GENERIC = 16,

	NL_HEADER_LENGTH = 16,
	GENL_HEADER_LENGTH = 4,
	GENL_ID_CTRL = 0x10,
	NL_ATTR_NESTED = 0x8000,
	NL_ATTR_TYPE_MASK = 0x3FFF,
};

// Building a message
struct NlMessage {
	uint8_t data[8192];
	size_t length;
};

void NlBegin(struct NlMessage*, uint16_t type, uint16_t flags, uint32_t sequence, uint32_t portId);
void NlAddGenl(struct NlMessage*, uint8_t command, uint8_t version);
void NlAddAttr(struct NlMessage*, uint16_t type, const void* data, size_t length);
void NlAddAttrU32(struct NlMessage*, uint16_t type, uint32_t value);
void NlAddAttrString(struct NlMessage*, uint16_t type, const char* value);
void NlAddRaw(struct NlMessage*, const void* data, size_t length);
void NlFinish(struct NlMessage*);

// Parsing. A datagram holds several messages; a message payload holds attributes (after the generic header).
struct NlHeader {
	uint32_t length;
	uint16_t type;
	uint16_t flags;
	uint32_t sequence;
	uint32_t portId;
};

bool NlNextMessage(const uint8_t* buffer, size_t length, size_t* offset, struct NlHeader* header, const uint8_t** payload, size_t* payloadLength);
bool NlNextAttr(const uint8_t** cursor, size_t* remaining, uint16_t* type, const uint8_t** payload, size_t* payloadLength);
uint32_t NlReadU32(const uint8_t* data);

struct NlClient;

typedef void (*NlReplyCallback)(void* context, const struct NlHeader* header, const uint8_t* payload, size_t payloadLength);

// Creates a netlink socket of the given protocol (NL_PROTOCOL_*), binds it and starts it.
struct NlClient* NlOpen(struct LdndConnection*, int protocol);
void NlClose(struct NlClient*);
uint32_t NlSocketId(const struct NlClient*);
// Feed a DATA frame from the connection's callback. Returns true when the frame belonged to this client's socket.
bool NlInput(struct NlClient*, uint32_t socketId, const uint8_t* data, size_t length);

// Sends one request and calls `callback` for each reply message until the request is complete: NLMSG_DONE for a dump,
// the first reply or the ACK otherwise. `body` is everything after the netlink header. Returns 0, a positive kernel
// errno from an NLMSG_ERROR, or a negative LDND_ERR_*.
int NlRequest(struct NlClient*, uint16_t type, uint16_t flags, const void* body, size_t bodyLength, NlReplyCallback callback, void* context, int timeoutMs);

// Generic netlink: NlRequest with a generic header (command, version) in front of the attributes.
int GenlRequest(struct NlClient*, uint16_t family, uint16_t flags, uint8_t command, uint8_t version, const void* attrs, size_t attrsLength,
                NlReplyCallback callback, void* context, int timeoutMs);

// Asks the controller for a family's id (for example "nl80211"). Returns 0 and sets *id, or an error as above.
int GenlResolveFamily(struct NlClient*, const char* name, uint16_t* id);

// Resolves a generic-netlink family's named multicast group (e.g. family "nl80211", group "mlme") to its id.
int GenlResolveMulticastGroup(struct NlClient*, const char* familyName, const char* groupName, uint32_t* groupId);
// Joins a multicast group on this socket, so the kernel's unsolicited notifications for it (e.g. nl80211's async
// CMD_CONNECT result) start arriving here at all - without this they are never delivered.
int NlJoinMulticastGroup(struct LdndConnection*, struct NlClient*, uint32_t groupId);

// Waits for the next message on this socket whose generic-netlink command byte equals `command`, regardless of
// sequence number - for a kernel notification that is not a reply to any request (e.g. an nl80211 CMD_CONNECT
// event arriving over the "mlme" multicast group after NlRequest's own ACK for the CMD_CONNECT request already
// came back). Non-matching messages already queued are discarded along the way. Returns 0 (payload passed to
// `callback`) or a negative LDND_ERR_* on timeout.
int NlWaitForEvent(struct NlClient*, uint8_t command, NlReplyCallback callback, void* context, int timeoutMs);

#endif
