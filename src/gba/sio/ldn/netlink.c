/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "netlink.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

enum {
	AF_NETLINK_LINUX = 16,
	SOCK_DGRAM_LINUX = 2,

	CTRL_CMD_GETFAMILY = 3,
	CTRL_ATTR_FAMILY_ID = 1,
	CTRL_ATTR_FAMILY_NAME = 2,
	CTRL_ATTR_MCAST_GROUPS = 7,
	CTRL_ATTR_MCAST_GRP_NAME = 1,
	CTRL_ATTR_MCAST_GRP_ID = 2,

	SOL_NETLINK_LINUX = 270,
	NETLINK_ADD_MEMBERSHIP_LINUX = 1,
};

// ---------------------------------------------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------------------------------------------

static void _le16(uint8_t* out, uint16_t value) {
	out[0] = value;
	out[1] = value >> 8;
}

static void _le32(uint8_t* out, uint32_t value) {
	out[0] = value;
	out[1] = value >> 8;
	out[2] = value >> 16;
	out[3] = value >> 24;
}

static uint16_t _rd16(const uint8_t* in) {
	return in[0] | (in[1] << 8);
}

uint32_t NlReadU32(const uint8_t* in) {
	return in[0] | (in[1] << 8) | (in[2] << 16) | ((uint32_t) in[3] << 24);
}

void NlBegin(struct NlMessage* message, uint16_t type, uint16_t flags, uint32_t sequence, uint32_t portId) {
	memset(message->data, 0, NL_HEADER_LENGTH);
	_le16(&message->data[4], type);
	_le16(&message->data[6], flags);
	_le32(&message->data[8], sequence);
	_le32(&message->data[12], portId);
	message->length = NL_HEADER_LENGTH;
}

void NlAddGenl(struct NlMessage* message, uint8_t command, uint8_t version) {
	message->data[message->length] = command;
	message->data[message->length + 1] = version;
	message->data[message->length + 2] = 0;
	message->data[message->length + 3] = 0;
	message->length += GENL_HEADER_LENGTH;
}

void NlAddRaw(struct NlMessage* message, const void* data, size_t length) {
	if (message->length + length > sizeof(message->data)) {
		return;
	}
	memcpy(&message->data[message->length], data, length);
	message->length += length;
}

void NlAddAttr(struct NlMessage* message, uint16_t type, const void* data, size_t length) {
	size_t padded = (length + 3) & ~(size_t) 3;
	if (message->length + 4 + padded > sizeof(message->data)) {
		return;
	}
	_le16(&message->data[message->length], (uint16_t) (4 + length));
	_le16(&message->data[message->length + 2], type);
	if (length) {
		memcpy(&message->data[message->length + 4], data, length);
	}
	memset(&message->data[message->length + 4 + length], 0, padded - length);
	message->length += 4 + padded;
}

void NlAddAttrU32(struct NlMessage* message, uint16_t type, uint32_t value) {
	uint8_t bytes[4];
	_le32(bytes, value);
	NlAddAttr(message, type, bytes, 4);
}

void NlAddAttrString(struct NlMessage* message, uint16_t type, const char* value) {
	NlAddAttr(message, type, value, strlen(value) + 1);
}

void NlFinish(struct NlMessage* message) {
	_le32(&message->data[0], (uint32_t) message->length);
}

// ---------------------------------------------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------------------------------------------

bool NlNextMessage(const uint8_t* buffer, size_t length, size_t* offset, struct NlHeader* header, const uint8_t** payload, size_t* payloadLength) {
	size_t at = *offset;
	if (at + NL_HEADER_LENGTH > length) {
		return false;
	}
	header->length = NlReadU32(&buffer[at]);
	header->type = _rd16(&buffer[at + 4]);
	header->flags = _rd16(&buffer[at + 6]);
	header->sequence = NlReadU32(&buffer[at + 8]);
	header->portId = NlReadU32(&buffer[at + 12]);
	if (header->length < NL_HEADER_LENGTH || at + header->length > length) {
		return false;
	}
	*payload = &buffer[at + NL_HEADER_LENGTH];
	*payloadLength = header->length - NL_HEADER_LENGTH;
	*offset = at + ((header->length + 3) & ~(size_t) 3);
	return true;
}

bool NlNextAttr(const uint8_t** cursor, size_t* remaining, uint16_t* type, const uint8_t** payload, size_t* payloadLength) {
	if (*remaining < 4) {
		return false;
	}
	uint16_t length = _rd16(*cursor);
	if (length < 4 || length > *remaining) {
		return false;
	}
	*type = _rd16(*cursor + 2);
	*payload = *cursor + 4;
	*payloadLength = length - 4;
	size_t step = (length + 3) & ~(size_t) 3;
	if (step > *remaining) {
		step = *remaining;
	}
	*cursor += step;
	*remaining -= step;
	return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Client over an ldnd socket
// ---------------------------------------------------------------------------------------------------------------

#ifdef _WIN32

struct Datagram {
	struct Datagram* next;
	size_t length;
	uint8_t data[];
};

struct NlClient {
	struct LdndConnection* conn;
	uint32_t socketId;
	uint32_t portId;
	uint32_t sequence;

	CRITICAL_SECTION lock;
	CONDITION_VARIABLE condition;
	struct Datagram* head;
	struct Datagram* tail;
};

uint32_t NlSocketId(const struct NlClient* client) {
	return client->socketId;
}

bool NlInput(struct NlClient* client, uint32_t socketId, const uint8_t* data, size_t length) {
	if (socketId != client->socketId) {
		return false;
	}
	struct Datagram* datagram = malloc(sizeof(*datagram) + length);
	if (!datagram) {
		return true;
	}
	datagram->next = NULL;
	datagram->length = length;
	memcpy(datagram->data, data, length);
	EnterCriticalSection(&client->lock);
	if (client->tail) {
		client->tail->next = datagram;
	} else {
		client->head = datagram;
	}
	client->tail = datagram;
	WakeAllConditionVariable(&client->condition);
	LeaveCriticalSection(&client->lock);
	return true;
}

struct NlClient* NlOpen(struct LdndConnection* conn, int protocol) {
	struct NlClient* client = calloc(1, sizeof(*client));
	if (!client) {
		return NULL;
	}
	client->conn = conn;
	InitializeCriticalSection(&client->lock);
	InitializeConditionVariable(&client->condition);

	if (LdndSocket(conn, AF_NETLINK_LINUX, SOCK_DGRAM_LINUX, protocol, &client->socketId) != LDND_OK) {
		client->socketId = 0;
		goto fail;
	}
	uint8_t address[12] = {0}; // struct sockaddr_nl: family, pad, pid, groups
	_le16(address, AF_NETLINK_LINUX);
	if (LdndBind(conn, client->socketId, address, sizeof(address)) != LDND_OK) {
		goto fail;
	}
	uint8_t bound[16] = {0};
	size_t boundLength = sizeof(bound);
	if (LdndGetSockName(conn, client->socketId, bound, &boundLength) != LDND_OK || boundLength < 8) {
		goto fail;
	}
	client->portId = NlReadU32(&bound[4]);
	if (LdndStart(conn, client->socketId) != LDND_OK) {
		goto fail;
	}
	return client;

fail:
	NlClose(client);
	return NULL;
}

void NlClose(struct NlClient* client) {
	if (!client) {
		return;
	}
	if (client->socketId) {
		LdndCloseSocket(client->conn, client->socketId);
	}
	while (client->head) {
		struct Datagram* next = client->head->next;
		free(client->head);
		client->head = next;
	}
	DeleteCriticalSection(&client->lock);
	free(client);
}

int NlRequest(struct NlClient* client, uint16_t type, uint16_t flags, const void* body, size_t bodyLength, NlReplyCallback callback, void* context, int timeoutMs) {
	struct NlMessage message;
	uint32_t sequence = ++client->sequence;
	NlBegin(&message, type, flags | NL_F_REQUEST, sequence, client->portId);
	if (body && bodyLength) {
		NlAddRaw(&message, body, bodyLength);
	}
	NlFinish(&message);

	// Discard anything left from an earlier request
	EnterCriticalSection(&client->lock);
	while (client->head) {
		struct Datagram* next = client->head->next;
		free(client->head);
		client->head = next;
	}
	client->tail = NULL;
	LeaveCriticalSection(&client->lock);

	int result = LdndSendTo(client->conn, client->socketId, 0, message.data, message.length);
	if (result != LDND_OK) {
		return result;
	}

	bool dump = (flags & NL_F_DUMP) == NL_F_DUMP;
	DWORD deadline = GetTickCount() + (DWORD) timeoutMs;
	while (true) {
		struct Datagram* datagram = NULL;
		EnterCriticalSection(&client->lock);
		while (!client->head) {
			DWORD now = GetTickCount();
			if ((int32_t) (deadline - now) <= 0) {
				break;
			}
			SleepConditionVariableCS(&client->condition, &client->lock, deadline - now);
		}
		datagram = client->head;
		if (datagram) {
			client->head = datagram->next;
			if (!client->head) {
				client->tail = NULL;
			}
		}
		LeaveCriticalSection(&client->lock);
		if (!datagram) {
			return LDND_ERR_TIMEOUT;
		}

		size_t offset = 0;
		struct NlHeader header;
		const uint8_t* payload;
		size_t payloadLength;
		bool finished = false;
		while (NlNextMessage(datagram->data, datagram->length, &offset, &header, &payload, &payloadLength)) {
			if (header.sequence != sequence) {
				continue;
			}
			if (header.type == NL_MSG_ERROR) {
				int32_t error = payloadLength >= 4 ? (int32_t) NlReadU32(payload) : -1;
				result = error < 0 ? -error : 0;
				finished = true;
			} else if (header.type == NL_MSG_DONE) {
				result = 0;
				finished = true;
			} else {
				if (callback) {
					callback(context, &header, payload, payloadLength);
				}
				if (!dump) {
					result = 0;
					finished = true;
				}
			}
		}
		free(datagram);
		if (finished) {
			return result;
		}
	}
}

int NlWaitForEvent(struct NlClient* client, uint8_t command, NlReplyCallback callback, void* context, int timeoutMs) {
	DWORD deadline = GetTickCount() + (DWORD) timeoutMs;
	while (true) {
		struct Datagram* datagram = NULL;
		EnterCriticalSection(&client->lock);
		while (!client->head) {
			DWORD now = GetTickCount();
			if ((int32_t) (deadline - now) <= 0) {
				break;
			}
			SleepConditionVariableCS(&client->condition, &client->lock, deadline - now);
		}
		datagram = client->head;
		if (datagram) {
			client->head = datagram->next;
			if (!client->head) {
				client->tail = NULL;
			}
		}
		LeaveCriticalSection(&client->lock);
		if (!datagram) {
			return LDND_ERR_TIMEOUT;
		}

		size_t offset = 0;
		struct NlHeader header;
		const uint8_t* payload;
		size_t payloadLength;
		bool found = false;
		while (NlNextMessage(datagram->data, datagram->length, &offset, &header, &payload, &payloadLength)) {
			if (header.type == NL_MSG_ERROR || header.type == NL_MSG_DONE || payloadLength < GENL_HEADER_LENGTH || payload[0] != command) {
				continue;
			}
			if (callback) {
				callback(context, &header, payload, payloadLength);
			}
			found = true;
		}
		free(datagram);
		if (found) {
			return LDND_OK;
		}
	}
}

int NlJoinMulticastGroup(struct LdndConnection* conn, struct NlClient* client, uint32_t groupId) {
	return LdndSetSockOpt(conn, client->socketId, SOL_NETLINK_LINUX, NETLINK_ADD_MEMBERSHIP_LINUX, &groupId, sizeof(groupId));
}

struct MulticastGroupResult {
	const char* groupName;
	bool found;
	uint32_t id;
};

static void _mcastGroupReply(void* context, const struct NlHeader* header, const uint8_t* payload, size_t payloadLength) {
	struct MulticastGroupResult* result = context;
	(void) header;
	if (payloadLength < GENL_HEADER_LENGTH) {
		return;
	}
	const uint8_t* cursor = payload + GENL_HEADER_LENGTH;
	size_t remaining = payloadLength - GENL_HEADER_LENGTH;
	uint16_t type;
	const uint8_t* value;
	size_t valueLength;
	while (NlNextAttr(&cursor, &remaining, &type, &value, &valueLength)) {
		if ((type & NL_ATTR_TYPE_MASK) != CTRL_ATTR_MCAST_GROUPS) {
			continue;
		}
		const uint8_t* groupsCursor = value;
		size_t groupsRemaining = valueLength;
		uint16_t groupType;
		const uint8_t* groupValue;
		size_t groupValueLength;
		while (NlNextAttr(&groupsCursor, &groupsRemaining, &groupType, &groupValue, &groupValueLength)) {
			const uint8_t* attrCursor = groupValue;
			size_t attrRemaining = groupValueLength;
			uint16_t attrType;
			const uint8_t* attrValue;
			size_t attrValueLength;
			const char* name = NULL;
			size_t nameLength = 0;
			uint32_t id = 0;
			bool haveId = false;
			while (NlNextAttr(&attrCursor, &attrRemaining, &attrType, &attrValue, &attrValueLength)) {
				if ((attrType & NL_ATTR_TYPE_MASK) == CTRL_ATTR_MCAST_GRP_NAME) {
					name = (const char*) attrValue;
					nameLength = attrValueLength;
				} else if ((attrType & NL_ATTR_TYPE_MASK) == CTRL_ATTR_MCAST_GRP_ID && attrValueLength >= 4) {
					id = NlReadU32(attrValue);
					haveId = true;
				}
			}
			if (name && haveId && strlen(result->groupName) <= nameLength && !strncmp(name, result->groupName, strlen(result->groupName)) &&
			    (nameLength == strlen(result->groupName) || name[strlen(result->groupName)] == 0)) {
				result->found = true;
				result->id = id;
			}
		}
	}
}

int GenlResolveMulticastGroup(struct NlClient* client, const char* familyName, const char* groupName, uint32_t* groupId) {
	struct NlMessage attrs;
	attrs.length = 0;
	NlAddAttrString(&attrs, CTRL_ATTR_FAMILY_NAME, familyName);
	struct MulticastGroupResult result = {0};
	result.groupName = groupName;
	int error = GenlRequest(client, GENL_ID_CTRL, 0, CTRL_CMD_GETFAMILY, 1, attrs.data, attrs.length, _mcastGroupReply, &result, 5000);
	if (error) {
		return error;
	}
	if (!result.found) {
		return LDND_ERR_ARGS;
	}
	*groupId = result.id;
	return 0;
}

int GenlRequest(struct NlClient* client, uint16_t family, uint16_t flags, uint8_t command, uint8_t version, const void* attrs, size_t attrsLength,
                NlReplyCallback callback, void* context, int timeoutMs) {
	struct NlMessage body;
	body.length = 0;
	NlAddGenl(&body, command, version);
	if (attrs && attrsLength) {
		NlAddRaw(&body, attrs, attrsLength);
	}
	return NlRequest(client, family, flags, body.data, body.length, callback, context, timeoutMs);
}

struct FamilyResult {
	bool found;
	uint16_t id;
};

static void _familyReply(void* context, const struct NlHeader* header, const uint8_t* payload, size_t payloadLength) {
	struct FamilyResult* result = context;
	(void) header;
	if (payloadLength < GENL_HEADER_LENGTH) {
		return;
	}
	const uint8_t* cursor = payload + GENL_HEADER_LENGTH;
	size_t remaining = payloadLength - GENL_HEADER_LENGTH;
	uint16_t type;
	const uint8_t* value;
	size_t valueLength;
	while (NlNextAttr(&cursor, &remaining, &type, &value, &valueLength)) {
		if ((type & NL_ATTR_TYPE_MASK) == CTRL_ATTR_FAMILY_ID && valueLength >= 2) {
			result->id = _rd16(value);
			result->found = true;
		}
	}
}

int GenlResolveFamily(struct NlClient* client, const char* name, uint16_t* id) {
	struct NlMessage attrs;
	attrs.length = 0;
	NlAddAttrString(&attrs, CTRL_ATTR_FAMILY_NAME, name);
	struct FamilyResult result = {0};
	int error = GenlRequest(client, GENL_ID_CTRL, 0, CTRL_CMD_GETFAMILY, 1, attrs.data, attrs.length, _familyReply, &result, 5000);
	if (error) {
		return error;
	}
	if (!result.found) {
		return LDND_ERR_ARGS;
	}
	*id = result.id;
	return 0;
}

#else // !_WIN32

struct NlClient* NlOpen(struct LdndConnection* conn, int protocol) {
	(void) conn;
	(void) protocol;
	return NULL;
}
void NlClose(struct NlClient* client) {
	(void) client;
}
uint32_t NlSocketId(const struct NlClient* client) {
	(void) client;
	return 0;
}
bool NlInput(struct NlClient* client, uint32_t socketId, const uint8_t* data, size_t length) {
	(void) client; (void) socketId; (void) data; (void) length;
	return false;
}
int NlRequest(struct NlClient* client, uint16_t type, uint16_t flags, const void* body, size_t bodyLength, NlReplyCallback callback, void* context, int timeoutMs) {
	(void) client; (void) type; (void) flags; (void) body; (void) bodyLength; (void) callback; (void) context; (void) timeoutMs;
	return LDND_ERR_PIPE;
}
int GenlRequest(struct NlClient* client, uint16_t family, uint16_t flags, uint8_t command, uint8_t version, const void* attrs, size_t attrsLength,
                NlReplyCallback callback, void* context, int timeoutMs) {
	(void) client; (void) family; (void) flags; (void) command; (void) version; (void) attrs; (void) attrsLength;
	(void) callback; (void) context; (void) timeoutMs;
	return LDND_ERR_PIPE;
}
int NlWaitForEvent(struct NlClient* client, uint8_t command, NlReplyCallback callback, void* context, int timeoutMs) {
	(void) client; (void) command; (void) callback; (void) context; (void) timeoutMs;
	return LDND_ERR_PIPE;
}
int NlJoinMulticastGroup(struct LdndConnection* conn, struct NlClient* client, uint32_t groupId) {
	(void) conn; (void) client; (void) groupId;
	return LDND_ERR_PIPE;
}
int GenlResolveMulticastGroup(struct NlClient* client, const char* familyName, const char* groupName, uint32_t* groupId) {
	(void) client; (void) familyName; (void) groupName; (void) groupId;
	return LDND_ERR_PIPE;
}
int GenlResolveFamily(struct NlClient* client, const char* name, uint16_t* id) {
	(void) client; (void) name; (void) id;
	return LDND_ERR_PIPE;
}

#endif
