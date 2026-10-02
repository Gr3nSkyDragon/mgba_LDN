/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ldnd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	DATAGRAM_PREFIX_LENGTH = 6, // peer[4], port u16
};

bool LdndParseDatagram(const uint8_t* payload, size_t length, uint8_t peer[4], uint16_t* port, const uint8_t** data, size_t* dataLength) {
	if (length < DATAGRAM_PREFIX_LENGTH) {
		return false;
	}
	memcpy(peer, payload, 4);
	*port = (uint16_t) (payload[4] | (payload[5] << 8));
	*data = &payload[DATAGRAM_PREFIX_LENGTH];
	*dataLength = length - DATAGRAM_PREFIX_LENGTH;
	return true;
}

const char* LdndStatusName(int code) {
	switch (code) {
	case LDND_OK:
		return "OK";
	case LDND_STATUS_BAD_REQUEST:
		return "BAD_REQUEST";
	case LDND_STATUS_UNSUPPORTED_VERSION:
		return "UNSUPPORTED_VERSION";
	case LDND_STATUS_INVALID_HANDLE:
		return "INVALID_HANDLE";
	case LDND_STATUS_INVALID_PARAM:
		return "INVALID_PARAM";
	case LDND_STATUS_BUSY:
		return "BUSY";
	case LDND_STATUS_NO_RADIO:
		return "NO_RADIO";
	case LDND_STATUS_NO_KEYS:
		return "NO_KEYS";
	case LDND_STATUS_NOT_FOUND:
		return "NOT_FOUND";
	case LDND_STATUS_TIMEOUT:
		return "TIMEOUT";
	case LDND_STATUS_AUTH_FAILED:
		return "AUTH_FAILED";
	case LDND_STATUS_IO:
		return "IO";
	case LDND_STATUS_UNSUPPORTED:
		return "UNSUPPORTED";
	case LDND_STATUS_INTERNAL:
		return "INTERNAL";
	case LDND_ERR_PIPE:
		return "pipe closed";
	case LDND_ERR_TIMEOUT:
		return "timed out";
	case LDND_ERR_ARGS:
		return "bad arguments";
	case LDND_ERR_PROTOCOL:
		return "bad reply";
	}
	return "UNKNOWN";
}

static int _setResult(struct LdndResult* result, int code, const char* format, ...) {
	if (result) {
		result->code = code;
		result->message[0] = 0;
		if (format) {
			va_list args;
			va_start(args, format);
			vsnprintf(result->message, sizeof(result->message), format, args);
			va_end(args);
		}
	}
	return code;
}

#ifdef _WIN32
#include <windows.h>

enum {
	OP_HELLO = 0x01,
	OP_LDN = 0x10,
	OP_REPLY = 0x80,
	OP_EVENT = 0x81,
	OP_DATA = 0x82,

	LDN_SCAN = 0x20,
	LDN_SCAN_CANCEL = 0x21,
	LDN_CONNECT = 0x30,
	LDN_CLOSE_NETWORK = 0x32,
	LDN_OPEN_DATAGRAM = 0x50,
	LDN_CLOSE_CHANNEL = 0x52,

	WIRELESS_PROTOCOL_LDN = 0,

	HEADER_LENGTH = 10,
	MAX_BODY = 256 * 1024,
	// Large enough for any request body this client builds (a Connect, the largest, is under 1.2 KiB).
	MAX_REQUEST = 2048,

	REQUEST_TIMEOUT_MS = 5000,
	// A scan answers once it has visited every channel, each costing its dwell plus ldnd's retune settle.
	SCAN_SETTLE_MS = 100,
	SCAN_DEFAULT_DWELL_MS = 1000, // an upper bound on ldnd's own default, for when the dwell is left to it
	SCAN_DEFAULT_CHANNELS = 3,
	// ldnd may overrun a join's budget by up to one association timeout; wait that much longer before giving up.
	CONNECT_GRACE_MS = 30000,
	CONNECT_UNBOUNDED_MS = 300000,
};

static const char* const kDefaultPipe = "\\\\.\\pipe\\ldnd";

// ---------------------------------------------------------------------------------------------------------------
// Body encoding: little-endian fields in order, `bytes`/`str`/`list` as a u32 count first, `opt` as a 0/1 flag first.
// ---------------------------------------------------------------------------------------------------------------

struct Writer {
	uint8_t* data;
	size_t capacity;
	size_t length;
	bool overflow;
};

static void _put(struct Writer* writer, const void* data, size_t length) {
	if (writer->overflow || length > writer->capacity - writer->length) {
		writer->overflow = true;
		return;
	}
	if (length) {
		memcpy(&writer->data[writer->length], data, length);
	}
	writer->length += length;
}

static void _putU8(struct Writer* writer, uint8_t value) {
	_put(writer, &value, 1);
}

static void _putU16(struct Writer* writer, uint16_t value) {
	uint8_t bytes[2] = {(uint8_t) value, (uint8_t) (value >> 8)};
	_put(writer, bytes, sizeof(bytes));
}

static void _putU32(struct Writer* writer, uint32_t value) {
	uint8_t bytes[4] = {(uint8_t) value, (uint8_t) (value >> 8), (uint8_t) (value >> 16), (uint8_t) (value >> 24)};
	_put(writer, bytes, sizeof(bytes));
}

static void _putU64(struct Writer* writer, uint64_t value) {
	_putU32(writer, (uint32_t) value);
	_putU32(writer, (uint32_t) (value >> 32));
}

static void _putBytes(struct Writer* writer, const void* data, size_t length) {
	if (length > UINT32_MAX) {
		writer->overflow = true;
		return;
	}
	_putU32(writer, (uint32_t) length);
	_put(writer, data, length);
}

static void _putParticipant(struct Writer* writer, const struct LdndParticipant* participant) {
	_put(writer, participant->ip, 4);
	_put(writer, participant->mac, 6);
	_putU8(writer, participant->connected);
	_putBytes(writer, participant->name, participant->nameLength);
	_putU16(writer, participant->appVersion);
	_putU8(writer, participant->platform);
}

static void _putNetworkInfo(struct Writer* writer, const struct LdndNetworkInfo* network) {
	_putU8(writer, network->protocol);
	_put(writer, network->address, 6);
	_putU8(writer, network->band);
	_putU8(writer, network->channel);
	_putU64(writer, network->localCommunicationId);
	_putU16(writer, network->sceneId);
	_put(writer, network->ssid, 16);
	_putU8(writer, network->version);
	_put(writer, network->serverRandom, 16);
	_putU16(writer, network->securityMode);
	_putU16(writer, network->appVersion);
	_putU8(writer, network->acceptPolicy);
	_putU8(writer, network->maxParticipants);
	_putU8(writer, network->numParticipants);
	if (network->participantCount > LDND_MAX_PARTICIPANTS || network->applicationDataLength > LDND_MAX_APPLICATION_DATA) {
		writer->overflow = true;
		return;
	}
	_putU32(writer, network->participantCount);
	for (unsigned i = 0; i < network->participantCount; ++i) {
		_putParticipant(writer, &network->participants[i]);
	}
	_putBytes(writer, network->applicationData, network->applicationDataLength);
	_putU64(writer, network->challenge);
	_put(writer, network->nonce, 4);
}

struct Reader {
	const uint8_t* data;
	size_t length;
	size_t offset;
	bool failed;
};

static const uint8_t* _take(struct Reader* reader, size_t length) {
	if (reader->failed || length > reader->length - reader->offset) {
		reader->failed = true;
		return NULL;
	}
	const uint8_t* data = &reader->data[reader->offset];
	reader->offset += length;
	return data;
}

static bool _atEnd(const struct Reader* reader) {
	return reader->offset == reader->length;
}

static uint8_t _getU8(struct Reader* reader) {
	const uint8_t* data = _take(reader, 1);
	return data ? data[0] : 0;
}

static uint16_t _getU16(struct Reader* reader) {
	const uint8_t* data = _take(reader, 2);
	return data ? (uint16_t) (data[0] | (data[1] << 8)) : 0;
}

static uint32_t _getU32(struct Reader* reader) {
	const uint8_t* data = _take(reader, 4);
	return data ? data[0] | (data[1] << 8) | (data[2] << 16) | ((uint32_t) data[3] << 24) : 0;
}

static uint64_t _getU64(struct Reader* reader) {
	uint64_t low = _getU32(reader);
	return low | ((uint64_t) _getU32(reader) << 32);
}

static bool _getBool(struct Reader* reader) {
	uint8_t value = _getU8(reader);
	if (value > 1) {
		reader->failed = true;
	}
	return value == 1;
}

static void _getFixed(struct Reader* reader, void* out, size_t length) {
	const uint8_t* data = _take(reader, length);
	if (data) {
		memcpy(out, data, length);
	} else {
		memset(out, 0, length);
	}
}

// A `bytes` or `str` field, left in place in the body.
static const uint8_t* _getBytes(struct Reader* reader, size_t* length) {
	*length = _getU32(reader);
	const uint8_t* data = _take(reader, *length);
	if (!data) {
		*length = 0;
	}
	return data;
}

// A `str` field, copied out (truncated to fit) and NUL-terminated.
static void _getString(struct Reader* reader, char* out, size_t size) {
	size_t length;
	const uint8_t* data = _getBytes(reader, &length);
	if (length >= size) {
		length = size - 1;
	}
	if (length) {
		memcpy(out, data, length);
	}
	out[length] = 0;
}

static void _getParticipant(struct Reader* reader, struct LdndParticipant* participant) {
	_getFixed(reader, participant->ip, 4);
	_getFixed(reader, participant->mac, 6);
	participant->connected = _getBool(reader);
	size_t nameLength;
	const uint8_t* name = _getBytes(reader, &nameLength);
	// An LDN participant name is at most 32 bytes on the air, so this never actually cuts anything off.
	if (nameLength > LDND_MAX_PARTICIPANT_NAME) {
		nameLength = LDND_MAX_PARTICIPANT_NAME;
	}
	if (nameLength) {
		memcpy(participant->name, name, nameLength);
	}
	participant->nameLength = (uint8_t) nameLength;
	participant->appVersion = _getU16(reader);
	participant->platform = _getU8(reader);
}

static void _getNetworkInfo(struct Reader* reader, struct LdndNetworkInfo* network) {
	memset(network, 0, sizeof(*network));
	network->protocol = _getU8(reader);
	_getFixed(reader, network->address, 6);
	network->band = _getU8(reader);
	network->channel = _getU8(reader);
	network->localCommunicationId = _getU64(reader);
	network->sceneId = _getU16(reader);
	_getFixed(reader, network->ssid, 16);
	network->version = _getU8(reader);
	_getFixed(reader, network->serverRandom, 16);
	network->securityMode = _getU16(reader);
	network->appVersion = _getU16(reader);
	network->acceptPolicy = _getU8(reader);
	network->maxParticipants = _getU8(reader);
	network->numParticipants = _getU8(reader);
	uint32_t participants = _getU32(reader);
	if (participants > LDND_MAX_PARTICIPANTS) {
		reader->failed = true;
		return;
	}
	network->participantCount = (uint8_t) participants;
	for (unsigned i = 0; i < participants; ++i) {
		_getParticipant(reader, &network->participants[i]);
	}
	size_t appDataLength;
	const uint8_t* appData = _getBytes(reader, &appDataLength);
	if (appDataLength > LDND_MAX_APPLICATION_DATA) {
		reader->failed = true;
		return;
	}
	if (appDataLength) {
		memcpy(network->applicationData, appData, appDataLength);
	}
	network->applicationDataLength = (uint16_t) appDataLength;
	network->challenge = _getU64(reader);
	_getFixed(reader, network->nonce, 4);
}

// Every reply opens with `status u8, error_message opt<str>`, and a failed one may stop right there. Returns the
// status, with ldnd's message (if any) in `result`.
static int _getReplyStatus(struct Reader* reader, const char* operation, struct LdndResult* result) {
	uint8_t status = _getU8(reader);
	char message[LDND_MAX_MESSAGE] = "";
	if (_getBool(reader)) {
		_getString(reader, message, sizeof(message));
	}
	if (reader->failed) {
		return _setResult(result, LDND_ERR_PROTOCOL, "ldnd's reply to %s does not decode", operation);
	}
	return _setResult(result, status, "%s", message);
}

// ---------------------------------------------------------------------------------------------------------------
// The pipe
// ---------------------------------------------------------------------------------------------------------------

// A request waiting for its reply. Lives on the waiting thread's stack, linked into the connection while it waits.
struct Pending {
	struct Pending* next;
	uint32_t requestId;
	bool replied;
	uint8_t* body;
	size_t length;
};

struct LdndConnection {
	HANDLE pipe;
	HANDLE readEvent;
	HANDLE writeEvent;
	HANDLE abortEvent; // manual reset; set once, by LdndAbort, and every pipe wait gives up on it
	HANDLE thread;
	struct LdndCallbacks callbacks;

	CRITICAL_SECTION writeLock; // one frame on the pipe at a time
	CRITICAL_SECTION stateLock; // guards everything below
	CONDITION_VARIABLE replyCondition;
	struct Pending* pending;
	uint32_t nextRequestId;
	bool failed;
};

// Waits for an overlapped read or write to finish, or for LdndAbort - in which case it is cancelled, and waited
// for anyway, since `overlapped` lives on the caller's stack.
static bool _finishIo(struct LdndConnection* conn, OVERLAPPED* overlapped, DWORD* transferred) {
	HANDLE handles[2] = {overlapped->hEvent, conn->abortEvent};
	if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) {
		CancelIoEx(conn->pipe, overlapped);
		GetOverlappedResult(conn->pipe, overlapped, transferred, TRUE);
		return false;
	}
	return GetOverlappedResult(conn->pipe, overlapped, transferred, FALSE);
}

static bool _aborted(struct LdndConnection* conn) {
	return WaitForSingleObject(conn->abortEvent, 0) == WAIT_OBJECT_0;
}

// Both check for LdndAbort before every read/write too: an overlapped call that completes at once never waits, so a
// busy pipe would otherwise keep going after it.
static bool _readExact(struct LdndConnection* conn, uint8_t* buffer, size_t length) {
	size_t done = 0;
	while (done < length) {
		if (_aborted(conn)) {
			return false;
		}
		OVERLAPPED overlapped;
		memset(&overlapped, 0, sizeof(overlapped));
		overlapped.hEvent = conn->readEvent;
		ResetEvent(conn->readEvent);
		DWORD got = 0;
		if (!ReadFile(conn->pipe, buffer + done, (DWORD) (length - done), &got, &overlapped)) {
			if (GetLastError() != ERROR_IO_PENDING || !_finishIo(conn, &overlapped, &got)) {
				return false;
			}
		}
		if (!got) {
			return false;
		}
		done += got;
	}
	return true;
}

static bool _writeAll(struct LdndConnection* conn, const uint8_t* buffer, size_t length) {
	size_t done = 0;
	while (done < length) {
		if (_aborted(conn)) {
			return false;
		}
		OVERLAPPED overlapped;
		memset(&overlapped, 0, sizeof(overlapped));
		overlapped.hEvent = conn->writeEvent;
		ResetEvent(conn->writeEvent);
		DWORD put = 0;
		if (!WriteFile(conn->pipe, buffer + done, (DWORD) (length - done), &put, &overlapped)) {
			if (GetLastError() != ERROR_IO_PENDING || !_finishIo(conn, &overlapped, &put)) {
				return false;
			}
		}
		if (!put) {
			return false;
		}
		done += put;
	}
	return true;
}

static void _put32(uint8_t* out, uint32_t value) {
	out[0] = (uint8_t) value;
	out[1] = (uint8_t) (value >> 8);
	out[2] = (uint8_t) (value >> 16);
	out[3] = (uint8_t) (value >> 24);
}

static uint32_t _get32(const uint8_t* in) {
	return in[0] | (in[1] << 8) | (in[2] << 16) | ((uint32_t) in[3] << 24);
}

static void _markFailed(struct LdndConnection* conn) {
	EnterCriticalSection(&conn->stateLock);
	conn->failed = true;
	WakeAllConditionVariable(&conn->replyCondition);
	LeaveCriticalSection(&conn->stateLock);
}

// One frame, written whole: the body is `head` followed by `tail` (either may be empty).
static bool _sendFrame(struct LdndConnection* conn, uint8_t op, uint8_t subOp, uint32_t requestId, const uint8_t* head, size_t headLength,
                       const uint8_t* tail, size_t tailLength) {
	size_t bodyLength = headLength + tailLength;
	if (bodyLength > MAX_BODY) {
		return false;
	}
	uint8_t stackFrame[HEADER_LENGTH + MAX_REQUEST + 64];
	uint8_t* frame = stackFrame;
	if (HEADER_LENGTH + bodyLength > sizeof(stackFrame)) {
		frame = malloc(HEADER_LENGTH + bodyLength);
		if (!frame) {
			return false;
		}
	}
	frame[0] = op;
	frame[1] = subOp;
	_put32(&frame[2], requestId);
	_put32(&frame[6], (uint32_t) bodyLength);
	if (headLength) {
		memcpy(&frame[HEADER_LENGTH], head, headLength);
	}
	if (tailLength) {
		memcpy(&frame[HEADER_LENGTH + headLength], tail, tailLength);
	}
	EnterCriticalSection(&conn->writeLock);
	bool ok = _writeAll(conn, frame, HEADER_LENGTH + bodyLength);
	LeaveCriticalSection(&conn->writeLock);
	if (frame != stackFrame) {
		free(frame);
	}
	if (!ok) {
		_markFailed(conn);
	}
	return ok;
}

static void _deliverReply(struct LdndConnection* conn, uint32_t requestId, uint8_t* body, size_t length) {
	EnterCriticalSection(&conn->stateLock);
	for (struct Pending* pending = conn->pending; pending; pending = pending->next) {
		if (pending->requestId == requestId && !pending->replied) {
			pending->body = body;
			pending->length = length;
			pending->replied = true;
			body = NULL;
			WakeAllConditionVariable(&conn->replyCondition);
			break;
		}
	}
	LeaveCriticalSection(&conn->stateLock);
	// Nobody is waiting for it: the reply to a request sent without waiting, or to one that timed out.
	free(body);
}

static void _dispatchEvent(struct LdndConnection* conn, const uint8_t* body, size_t length) {
	if (!conn->callbacks.event) {
		return;
	}
	struct Reader reader = {body, length, 0, false};
	struct LdndEvent event;
	memset(&event, 0, sizeof(event));
	struct LdndNetworkInfo network;
	struct LdndParticipant participant;
	char message[512] = "";

	event.kind = (enum LdndEventKind) _getU8(&reader);
	switch (event.kind) {
	case LDND_EVENT_NETWORK_FOUND:
		_getNetworkInfo(&reader, &network);
		event.network = &network;
		break;
	case LDND_EVENT_SCAN_DONE:
	case LDND_EVENT_LOG_DROPPED:
		event.count = _getU32(&reader);
		break;
	case LDND_EVENT_JOIN:
	case LDND_EVENT_LEAVE:
		event.handle = _getU32(&reader);
		event.index = _getU8(&reader);
		memset(&participant, 0, sizeof(participant));
		_getParticipant(&reader, &participant);
		event.participant = &participant;
		break;
	case LDND_EVENT_DISCONNECT:
		event.handle = _getU32(&reader);
		event.reason = _getU8(&reader);
		break;
	case LDND_EVENT_APP_DATA_CHANGED:
		event.handle = _getU32(&reader);
		event.oldData = _getBytes(&reader, &event.oldDataLength);
		event.newData = _getBytes(&reader, &event.newDataLength);
		break;
	case LDND_EVENT_POLICY_CHANGED:
		event.handle = _getU32(&reader);
		event.oldPolicy = _getU8(&reader);
		event.newPolicy = _getU8(&reader);
		break;
	case LDND_EVENT_CHANNEL_ERROR:
		event.handle = _getU32(&reader);
		event.status = _getU8(&reader);
		_getString(&reader, message, sizeof(message));
		event.message = message;
		break;
	case LDND_EVENT_LOG:
		_getString(&reader, message, sizeof(message));
		event.message = message;
		break;
	case LDND_EVENT_RADIO_STATE:
		event.radioState = _getU8(&reader);
		if (_getBool(&reader)) {
			_getString(&reader, message, sizeof(message));
			event.message = message;
		}
		break;
	default:
		// A kind this client does not know yet.
		return;
	}
	if (!reader.failed) {
		conn->callbacks.event(conn->callbacks.context, &event);
	}
}

static DWORD WINAPI _readerThread(LPVOID context) {
	struct LdndConnection* conn = context;
	uint8_t header[HEADER_LENGTH];
	while (_readExact(conn, header, sizeof(header))) {
		uint8_t op = header[0];
		uint32_t requestId = _get32(&header[2]);
		uint32_t length = _get32(&header[6]);
		if (length > MAX_BODY) {
			break;
		}
		uint8_t* body = NULL;
		if (length) {
			body = malloc(length);
			if (!body || !_readExact(conn, body, length)) {
				free(body);
				break;
			}
		}
		switch (op) {
		case OP_REPLY:
			_deliverReply(conn, requestId, body, length);
			body = NULL;
			break;
		case OP_EVENT:
			_dispatchEvent(conn, body, length);
			break;
		case OP_DATA:
			if (length >= 4 && conn->callbacks.data) {
				conn->callbacks.data(conn->callbacks.context, _get32(body), &body[4], length - 4);
			}
			break;
		default:
			// Unknown ops are skipped whole: body_length still frames them.
			break;
		}
		free(body);
	}
	_markFailed(conn);
	return 0;
}

// Sends a request and waits up to `timeoutMs` for its reply, whose body (possibly empty) is handed back in
// `*reply` for the caller to free.
static int _request(struct LdndConnection* conn, const char* operation, uint8_t op, uint8_t subOp, const uint8_t* body, size_t length, DWORD timeoutMs,
                    uint8_t** reply, size_t* replyLength, struct LdndResult* result) {
	*reply = NULL;
	*replyLength = 0;
	struct Pending pending;
	memset(&pending, 0, sizeof(pending));

	EnterCriticalSection(&conn->stateLock);
	if (conn->failed) {
		LeaveCriticalSection(&conn->stateLock);
		return _setResult(result, LDND_ERR_PIPE, "the connection to ldnd is closed");
	}
	pending.requestId = ++conn->nextRequestId;
	pending.next = conn->pending;
	conn->pending = &pending;
	LeaveCriticalSection(&conn->stateLock);

	bool sent = _sendFrame(conn, op, subOp, pending.requestId, body, length, NULL, 0);

	EnterCriticalSection(&conn->stateLock);
	DWORD start = GetTickCount();
	while (sent && !pending.replied && !conn->failed) {
		DWORD elapsed = GetTickCount() - start;
		if (elapsed >= timeoutMs) {
			break;
		}
		SleepConditionVariableCS(&conn->replyCondition, &conn->stateLock, timeoutMs - elapsed);
	}
	for (struct Pending** link = &conn->pending; *link; link = &(*link)->next) {
		if (*link == &pending) {
			*link = pending.next;
			break;
		}
	}
	bool failed = conn->failed;
	LeaveCriticalSection(&conn->stateLock);

	if (!pending.replied) {
		if (sent && !failed) {
			return _setResult(result, LDND_ERR_TIMEOUT, "ldnd did not answer %s within %lu ms", operation, (unsigned long) timeoutMs);
		}
		return _setResult(result, LDND_ERR_PIPE, "the connection to ldnd closed before it answered %s", operation);
	}
	*reply = pending.body;
	*replyLength = pending.length;
	return LDND_OK;
}

// An LDN request whose reply is a bare status.
static int _statusRequest(struct LdndConnection* conn, const char* operation, uint8_t subOp, const uint8_t* body, size_t length,
                          struct LdndResult* result) {
	uint8_t* reply;
	size_t replyLength;
	int code = _request(conn, operation, OP_LDN, subOp, body, length, REQUEST_TIMEOUT_MS, &reply, &replyLength, result);
	if (code == LDND_OK) {
		struct Reader reader = {reply, replyLength, 0, false};
		code = _getReplyStatus(&reader, operation, result);
		free(reply);
	}
	return code;
}

static HANDLE _openPipe(const char* path, DWORD* error) {
	for (int attempt = 0; attempt < 5; ++attempt) {
		HANDLE pipe = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (pipe != INVALID_HANDLE_VALUE) {
			return pipe;
		}
		*error = GetLastError();
		// ldnd keeps an instance listening at all times, so busy only ever means "between two clients".
		if (*error != ERROR_PIPE_BUSY) {
			break;
		}
		WaitNamedPipeA(path, 1000);
	}
	return INVALID_HANDLE_VALUE;
}

static bool _hello(struct LdndConnection* conn, const char* clientName, const char* clientVersion, struct LdndHello* hello, struct LdndResult* result) {
	if (!clientName) {
		clientName = "";
	}
	if (!clientVersion) {
		clientVersion = "";
	}
	uint8_t buffer[MAX_REQUEST];
	struct Writer writer = {buffer, sizeof(buffer), 0, false};
	_putU32(&writer, LDND_PROTOCOL_VERSION);
	_putU8(&writer, WIRELESS_PROTOCOL_LDN);
	_putBytes(&writer, clientName, strlen(clientName));
	_putBytes(&writer, clientVersion, strlen(clientVersion));
	if (writer.overflow) {
		_setResult(result, LDND_ERR_ARGS, "the client name or version is too long");
		return false;
	}

	uint8_t* reply;
	size_t replyLength;
	int code = _request(conn, "Hello", OP_HELLO, 0, buffer, writer.length, REQUEST_TIMEOUT_MS, &reply, &replyLength, result);
	if (code == LDND_ERR_PIPE) {
		// The only daemon that hangs up on a well-formed Hello is one that does not speak this protocol at all.
		_setResult(result, code, "ldnd closed the connection without answering Hello (an ldnd older than protocol %d?)", LDND_PROTOCOL_VERSION);
	}
	if (code != LDND_OK) {
		return false;
	}
	struct Reader reader = {reply, replyLength, 0, false};
	code = _getReplyStatus(&reader, "Hello", result);
	// Even a refusal carries the rest (only a Hello ldnd cannot parse gets a bare status), so it is read either way:
	// it says which version ldnd does speak. A refusal keeps its own status whatever follows it.
	if (code >= LDND_OK) {
		struct LdndHello answer;
		memset(&answer, 0, sizeof(answer));
		answer.protocolVersion = _getU32(&reader);
		_getString(&reader, answer.daemonVersion, sizeof(answer.daemonVersion));
		answer.ldnCapabilities = _getU8(&reader);
		answer.nwmCapabilities = _getU8(&reader);
		answer.radioReady = _getBool(&reader);
		if (!reader.failed && hello) {
			*hello = answer;
		} else if (reader.failed && code == LDND_OK) {
			code = _setResult(result, LDND_ERR_PROTOCOL, "ldnd's reply to Hello does not decode");
		}
	}
	free(reply);
	return code == LDND_OK;
}

struct LdndConnection* LdndOpen(const char* pipePath, const char* clientName, const char* clientVersion, const struct LdndCallbacks* callbacks,
                                struct LdndHello* hello, struct LdndResult* result) {
	_setResult(result, LDND_OK, NULL);
	if (hello) {
		memset(hello, 0, sizeof(*hello));
	}
	if (!pipePath) {
		pipePath = kDefaultPipe;
	}
	DWORD error = 0;
	HANDLE pipe = _openPipe(pipePath, &error);
	if (pipe == INVALID_HANDLE_VALUE) {
		if (error == ERROR_FILE_NOT_FOUND) {
			_setResult(result, LDND_ERR_PIPE, "%s does not exist: ldnd is not running", pipePath);
		} else if (error == ERROR_PIPE_BUSY) {
			_setResult(result, LDND_ERR_PIPE, "%s stayed busy", pipePath);
		} else {
			_setResult(result, LDND_ERR_PIPE, "could not open %s (error %lu)", pipePath, (unsigned long) error);
		}
		return NULL;
	}

	struct LdndConnection* conn = calloc(1, sizeof(*conn));
	if (!conn) {
		CloseHandle(pipe);
		_setResult(result, LDND_ERR_ARGS, "out of memory");
		return NULL;
	}
	conn->pipe = pipe;
	if (callbacks) {
		conn->callbacks = *callbacks;
	}
	conn->readEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	conn->writeEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	conn->abortEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	InitializeCriticalSection(&conn->writeLock);
	InitializeCriticalSection(&conn->stateLock);
	InitializeConditionVariable(&conn->replyCondition);
	if (conn->readEvent && conn->writeEvent && conn->abortEvent) {
		conn->thread = CreateThread(NULL, 0, _readerThread, conn, 0, NULL);
	}
	if (!conn->thread) {
		_setResult(result, LDND_ERR_ARGS, "could not start the ldnd reader thread");
		LdndClose(conn);
		return NULL;
	}
	if (!_hello(conn, clientName, clientVersion, hello, result)) {
		LdndClose(conn);
		return NULL;
	}
	return conn;
}

void LdndAbort(struct LdndConnection* conn) {
	_markFailed(conn);
	SetEvent(conn->abortEvent);
}

bool LdndIsOpen(struct LdndConnection* conn) {
	EnterCriticalSection(&conn->stateLock);
	bool open = !conn->failed;
	LeaveCriticalSection(&conn->stateLock);
	return open;
}

void LdndClose(struct LdndConnection* conn) {
	if (!conn) {
		return;
	}
	if (conn->abortEvent) {
		LdndAbort(conn);
	}
	if (conn->thread) {
		WaitForSingleObject(conn->thread, INFINITE);
		CloseHandle(conn->thread);
	}
	CloseHandle(conn->pipe);
	if (conn->readEvent) {
		CloseHandle(conn->readEvent);
	}
	if (conn->writeEvent) {
		CloseHandle(conn->writeEvent);
	}
	if (conn->abortEvent) {
		CloseHandle(conn->abortEvent);
	}
	DeleteCriticalSection(&conn->writeLock);
	DeleteCriticalSection(&conn->stateLock);
	free(conn);
}

int LdndScan(struct LdndConnection* conn, const struct LdndScanRequest* request, struct LdndNetworkInfo* networks, size_t maxNetworks, size_t* count,
             struct LdndResult* result) {
	*count = 0;
	uint8_t buffer[MAX_REQUEST];
	struct Writer writer = {buffer, sizeof(buffer), 0, false};
	_putBytes(&writer, request->channels, request->channelCount); // list<u8> and bytes encode alike
	if (request->dwellMs) {
		_putU8(&writer, 1);
		_putU32(&writer, request->dwellMs);
	} else {
		_putU8(&writer, 0);
	}
	_putU32(&writer, 0); // protocols: ldnd's (1 and 3)
	_putU8(&writer, 0); // timeout_ms
	if (writer.overflow) {
		return _setResult(result, LDND_ERR_ARGS, "too many channels");
	}

	size_t channels = request->channelCount ? request->channelCount : SCAN_DEFAULT_CHANNELS;
	uint32_t dwell = request->dwellMs ? request->dwellMs : SCAN_DEFAULT_DWELL_MS;
	DWORD timeout = (DWORD) (channels * (dwell + SCAN_SETTLE_MS) + REQUEST_TIMEOUT_MS);

	uint8_t* reply;
	size_t replyLength;
	int code = _request(conn, "Scan", OP_LDN, LDN_SCAN, buffer, writer.length, timeout, &reply, &replyLength, result);
	if (code != LDND_OK) {
		return code;
	}
	struct Reader reader = {reply, replyLength, 0, false};
	code = _getReplyStatus(&reader, "Scan", result);
	if (code == LDND_OK) {
		uint32_t found = _getU32(&reader);
		for (uint32_t i = 0; i < found && !reader.failed; ++i) {
			struct LdndNetworkInfo skipped;
			_getNetworkInfo(&reader, i < maxNetworks ? &networks[i] : &skipped);
		}
		if (reader.failed) {
			code = _setResult(result, LDND_ERR_PROTOCOL, "ldnd's reply to Scan does not decode");
		} else {
			*count = found < maxNetworks ? found : maxNetworks;
		}
	}
	free(reply);
	return code;
}

int LdndScanCancel(struct LdndConnection* conn) {
	// Its reply arrives with no waiter registered and is dropped by the reader.
	EnterCriticalSection(&conn->stateLock);
	bool failed = conn->failed;
	uint32_t requestId = ++conn->nextRequestId;
	LeaveCriticalSection(&conn->stateLock);
	if (failed || !_sendFrame(conn, OP_LDN, LDN_SCAN_CANCEL, requestId, NULL, 0, NULL, 0)) {
		return LDND_ERR_PIPE;
	}
	return LDND_OK;
}

int LdndConnect(struct LdndConnection* conn, const struct LdndConnectRequest* request, struct LdndNetworkReply* reply, struct LdndResult* result) {
	memset(reply, 0, sizeof(*reply));
	uint8_t buffer[MAX_REQUEST];
	struct Writer writer = {buffer, sizeof(buffer), 0, false};
	_putNetworkInfo(&writer, request->network);
	_putBytes(&writer, request->password, request->passwordLength);
	_putBytes(&writer, request->name, request->name ? strlen(request->name) : 0);
	_putU16(&writer, request->appVersion);
	_putU8(&writer, request->platform);
	_putU8(&writer, request->enableChallenge);
	_putU64(&writer, request->deviceId);
	_putU8(&writer, 0); // client_random: ldnd picks one
	_putU8(&writer, 0); // dev
	if (request->timeoutMs) {
		_putU8(&writer, 1);
		_putU32(&writer, request->timeoutMs);
	} else {
		_putU8(&writer, 0);
	}
	if (writer.overflow) {
		return _setResult(result, LDND_ERR_ARGS, "the Connect request does not fit");
	}

	DWORD timeout = request->timeoutMs ? request->timeoutMs + CONNECT_GRACE_MS : CONNECT_UNBOUNDED_MS;
	uint8_t* body;
	size_t bodyLength;
	int code = _request(conn, "Connect", OP_LDN, LDN_CONNECT, buffer, writer.length, timeout, &body, &bodyLength, result);
	if (code != LDND_OK) {
		return code;
	}
	struct Reader reader = {body, bodyLength, 0, false};
	code = _getReplyStatus(&reader, "Connect", result);
	// A refused join still has the rest (its auth_status says why); other failures may be a bare status.
	if (code >= LDND_OK && !_atEnd(&reader)) {
		reply->handle = _getU32(&reader);
		reply->haveAuthStatus = _getBool(&reader);
		if (reply->haveAuthStatus) {
			reply->authStatus = _getU8(&reader);
		}
		reply->haveNetwork = _getBool(&reader);
		if (reply->haveNetwork) {
			_getNetworkInfo(&reader, &reply->network);
		}
		if (_getBool(&reader)) {
			reply->participantIndex = _getU8(&reader);
		}
		if (reader.failed) {
			reply->haveAuthStatus = false;
			reply->haveNetwork = false;
		}
	}
	if (code == LDND_OK && (reader.failed || !reply->handle || !reply->haveNetwork || reply->participantIndex >= reply->network.participantCount)) {
		code = _setResult(result, LDND_ERR_PROTOCOL, "ldnd's reply to Connect does not describe the joined network");
	}
	free(body);
	return code;
}

int LdndCloseNetwork(struct LdndConnection* conn, uint32_t network, struct LdndResult* result) {
	uint8_t body[4];
	_put32(body, network);
	return _statusRequest(conn, "CloseNetwork", LDN_CLOSE_NETWORK, body, sizeof(body), result);
}

int LdndOpenDatagram(struct LdndConnection* conn, uint32_t network, uint16_t port, uint32_t* channel, uint16_t* boundPort, struct LdndResult* result) {
	*channel = 0;
	*boundPort = 0;
	uint8_t body[6];
	_put32(body, network);
	body[4] = (uint8_t) port;
	body[5] = (uint8_t) (port >> 8);
	uint8_t* reply;
	size_t replyLength;
	int code = _request(conn, "OpenDatagram", OP_LDN, LDN_OPEN_DATAGRAM, body, sizeof(body), REQUEST_TIMEOUT_MS, &reply, &replyLength, result);
	if (code != LDND_OK) {
		return code;
	}
	struct Reader reader = {reply, replyLength, 0, false};
	code = _getReplyStatus(&reader, "OpenDatagram", result);
	if (code == LDND_OK) {
		*channel = _getU32(&reader);
		*boundPort = _getU16(&reader);
		if (reader.failed || !*channel) {
			code = _setResult(result, LDND_ERR_PROTOCOL, "ldnd's reply to OpenDatagram does not decode");
		}
	}
	free(reply);
	return code;
}

int LdndCloseChannel(struct LdndConnection* conn, uint32_t channel, struct LdndResult* result) {
	uint8_t body[4];
	_put32(body, channel);
	return _statusRequest(conn, "CloseChannel", LDN_CLOSE_CHANNEL, body, sizeof(body), result);
}

int LdndSendDatagram(struct LdndConnection* conn, uint32_t channel, const uint8_t peer[4], uint16_t port, const void* data, size_t length) {
	if (length > MAX_BODY - 4 - DATAGRAM_PREFIX_LENGTH) {
		return LDND_ERR_ARGS;
	}
	uint8_t head[4 + DATAGRAM_PREFIX_LENGTH];
	_put32(head, channel);
	memcpy(&head[4], peer, 4);
	head[8] = (uint8_t) port;
	head[9] = (uint8_t) (port >> 8);
	if (!LdndIsOpen(conn) || !_sendFrame(conn, OP_DATA, 0, 0, head, sizeof(head), data, length)) {
		return LDND_ERR_PIPE;
	}
	return LDND_OK;
}

#else // !_WIN32

struct LdndConnection* LdndOpen(const char* pipePath, const char* clientName, const char* clientVersion, const struct LdndCallbacks* callbacks,
                                struct LdndHello* hello, struct LdndResult* result) {
	(void) pipePath;
	(void) clientName;
	(void) clientVersion;
	(void) callbacks;
	if (hello) {
		memset(hello, 0, sizeof(*hello));
	}
	_setResult(result, LDND_ERR_PIPE, "ldnd is only reachable on Windows");
	return NULL;
}

void LdndClose(struct LdndConnection* conn) {
	(void) conn;
}

void LdndAbort(struct LdndConnection* conn) {
	(void) conn;
}

bool LdndIsOpen(struct LdndConnection* conn) {
	(void) conn;
	return false;
}

int LdndScan(struct LdndConnection* conn, const struct LdndScanRequest* request, struct LdndNetworkInfo* networks, size_t maxNetworks, size_t* count,
             struct LdndResult* result) {
	(void) conn;
	(void) request;
	(void) networks;
	(void) maxNetworks;
	*count = 0;
	return _setResult(result, LDND_ERR_PIPE, NULL);
}

int LdndScanCancel(struct LdndConnection* conn) {
	(void) conn;
	return LDND_ERR_PIPE;
}

int LdndConnect(struct LdndConnection* conn, const struct LdndConnectRequest* request, struct LdndNetworkReply* reply, struct LdndResult* result) {
	(void) conn;
	(void) request;
	memset(reply, 0, sizeof(*reply));
	return _setResult(result, LDND_ERR_PIPE, NULL);
}

int LdndCloseNetwork(struct LdndConnection* conn, uint32_t network, struct LdndResult* result) {
	(void) conn;
	(void) network;
	return _setResult(result, LDND_ERR_PIPE, NULL);
}

int LdndOpenDatagram(struct LdndConnection* conn, uint32_t network, uint16_t port, uint32_t* channel, uint16_t* boundPort, struct LdndResult* result) {
	(void) conn;
	(void) network;
	(void) port;
	*channel = 0;
	*boundPort = 0;
	return _setResult(result, LDND_ERR_PIPE, NULL);
}

int LdndCloseChannel(struct LdndConnection* conn, uint32_t channel, struct LdndResult* result) {
	(void) conn;
	(void) channel;
	return _setResult(result, LDND_ERR_PIPE, NULL);
}

int LdndSendDatagram(struct LdndConnection* conn, uint32_t channel, const uint8_t peer[4], uint16_t port, const void* data, size_t length) {
	(void) conn;
	(void) channel;
	(void) peer;
	(void) port;
	(void) data;
	(void) length;
	return LDND_ERR_PIPE;
}

#endif
