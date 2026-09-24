/* Copyright (c) 2026 mgba_ldn contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ldnd.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

enum {
	OP_SOCKET = 1,
	OP_BIND = 2,
	OP_SETSOCKOPT = 3,
	OP_GETSOCKNAME = 4,
	OP_START = 5,
	OP_SENDTO = 6,
	OP_CLOSE = 7,
	OP_REPLY = 8,
	OP_DATA = 9,

	HEADER_LENGTH = 21,
	MAX_BLOB = 262144,
	REPLY_TIMEOUT_MS = 5000,
};

struct LdndConnection {
	HANDLE pipe;
	HANDLE readEvent;
	HANDLE writeEvent;
	HANDLE thread;

	CRITICAL_SECTION writeLock;
	CRITICAL_SECTION requestLock; // one request at a time
	CRITICAL_SECTION stateLock;
	CONDITION_VARIABLE replyCondition;

	// The reply the current request is waiting for
	bool waiting;
	bool replied;
	uint32_t waitSocket;
	uint32_t replyError;
	uint8_t* replyBlob;
	size_t replyLength;

	volatile bool closing;
	volatile bool failed;

	CRITICAL_SECTION callbacksLock;
	struct {
		LdndDataCallback callback;
		void* context;
	} callbacks[8];
};

static bool _readExact(struct LdndConnection* conn, uint8_t* buffer, size_t length) {
	size_t done = 0;
	while (done < length) {
		OVERLAPPED overlapped;
		memset(&overlapped, 0, sizeof(overlapped));
		overlapped.hEvent = conn->readEvent;
		ResetEvent(conn->readEvent);
		DWORD got = 0;
		if (!ReadFile(conn->pipe, buffer + done, (DWORD) (length - done), &got, &overlapped)) {
			if (GetLastError() != ERROR_IO_PENDING) {
				return false;
			}
			if (!GetOverlappedResult(conn->pipe, &overlapped, &got, TRUE)) {
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
		OVERLAPPED overlapped;
		memset(&overlapped, 0, sizeof(overlapped));
		overlapped.hEvent = conn->writeEvent;
		ResetEvent(conn->writeEvent);
		DWORD put = 0;
		if (!WriteFile(conn->pipe, buffer + done, (DWORD) (length - done), &put, &overlapped)) {
			if (GetLastError() != ERROR_IO_PENDING) {
				return false;
			}
			if (!GetOverlappedResult(conn->pipe, &overlapped, &put, TRUE)) {
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
	out[0] = value;
	out[1] = value >> 8;
	out[2] = value >> 16;
	out[3] = value >> 24;
}

static uint32_t _get32(const uint8_t* in) {
	return in[0] | (in[1] << 8) | (in[2] << 16) | ((uint32_t) in[3] << 24);
}

static bool _sendFrame(struct LdndConnection* conn, uint8_t op, uint32_t socketId, uint32_t arg0, uint32_t arg1, uint32_t arg2, const void* blob, size_t length) {
	if (length > MAX_BLOB) {
		return false;
	}
	uint8_t* frame = malloc(HEADER_LENGTH + length);
	if (!frame) {
		return false;
	}
	frame[0] = op;
	_put32(&frame[1], socketId);
	_put32(&frame[5], arg0);
	_put32(&frame[9], arg1);
	_put32(&frame[13], arg2);
	_put32(&frame[17], (uint32_t) length);
	if (length) {
		memcpy(&frame[HEADER_LENGTH], blob, length);
	}
	EnterCriticalSection(&conn->writeLock);
	bool ok = _writeAll(conn, frame, HEADER_LENGTH + length);
	LeaveCriticalSection(&conn->writeLock);
	free(frame);
	if (!ok) {
		conn->failed = true;
	}
	return ok;
}

static DWORD WINAPI _readerThread(LPVOID context) {
	struct LdndConnection* conn = context;
	uint8_t header[HEADER_LENGTH];

	while (!conn->closing) {
		if (!_readExact(conn, header, sizeof(header))) {
			break;
		}
		uint8_t op = header[0];
		uint32_t socketId = _get32(&header[1]);
		uint32_t arg0 = _get32(&header[5]);
		uint32_t length = _get32(&header[17]);
		if (length > MAX_BLOB) {
			break;
		}
		uint8_t* blob = NULL;
		if (length) {
			blob = malloc(length);
			if (!blob || !_readExact(conn, blob, length)) {
				free(blob);
				break;
			}
		}

		if (op == OP_REPLY) {
			EnterCriticalSection(&conn->stateLock);
			if (conn->waiting && conn->waitSocket == socketId) {
				conn->replyError = arg0;
				free(conn->replyBlob);
				conn->replyBlob = blob;
				conn->replyLength = length;
				blob = NULL;
				conn->replied = true;
				WakeAllConditionVariable(&conn->replyCondition);
			}
			LeaveCriticalSection(&conn->stateLock);
		} else if (op == OP_DATA) {
			EnterCriticalSection(&conn->callbacksLock);
			struct { LdndDataCallback callback; void* context; } snapshot[8];
			memcpy(snapshot, conn->callbacks, sizeof(snapshot));
			LeaveCriticalSection(&conn->callbacksLock);
			for (size_t i = 0; i < sizeof(snapshot) / sizeof(snapshot[0]); ++i) {
				if (snapshot[i].callback && snapshot[i].callback(snapshot[i].context, socketId, blob, length)) {
					break;
				}
			}
		}
		free(blob);
	}

	conn->failed = true;
	EnterCriticalSection(&conn->stateLock);
	WakeAllConditionVariable(&conn->replyCondition);
	LeaveCriticalSection(&conn->stateLock);
	return 0;
}

struct LdndConnection* LdndOpen(const char* pipePath) {
	if (!pipePath) {
		pipePath = "\\\\.\\pipe\\ldnd";
	}
	HANDLE pipe = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 20; ++attempt) {
		pipe = CreateFileA(pipePath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (pipe != INVALID_HANDLE_VALUE) {
			break;
		}
		DWORD error = GetLastError();
		if (error == ERROR_PIPE_BUSY) {
			WaitNamedPipeA(pipePath, 2000);
			continue;
		}
		// A previous client (e.g. our own earlier connection, freshly closed) may still be in the process of
		// disconnecting on ldnd's side; briefly the pipe does not exist at all rather than being busy. Worth a
		// short retry before giving up.
		if (error == ERROR_FILE_NOT_FOUND) {
			Sleep(100);
			continue;
		}
		return NULL;
	}
	if (pipe == INVALID_HANDLE_VALUE) {
		return NULL;
	}

	struct LdndConnection* conn = calloc(1, sizeof(*conn));
	if (!conn) {
		CloseHandle(pipe);
		return NULL;
	}
	conn->pipe = pipe;
	conn->readEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	conn->writeEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	InitializeCriticalSection(&conn->writeLock);
	InitializeCriticalSection(&conn->requestLock);
	InitializeCriticalSection(&conn->stateLock);
	InitializeCriticalSection(&conn->callbacksLock);
	InitializeConditionVariable(&conn->replyCondition);
	conn->thread = CreateThread(NULL, 0, _readerThread, conn, 0, NULL);
	if (!conn->thread) {
		conn->closing = true;
		LdndClose(conn);
		return NULL;
	}
	return conn;
}

void LdndClose(struct LdndConnection* conn) {
	if (!conn) {
		return;
	}
	conn->closing = true;
	if (conn->thread) {
		CancelIoEx(conn->pipe, NULL);
		WaitForSingleObject(conn->thread, 5000);
		CloseHandle(conn->thread);
	}
	CloseHandle(conn->pipe);
	CloseHandle(conn->readEvent);
	CloseHandle(conn->writeEvent);
	DeleteCriticalSection(&conn->writeLock);
	DeleteCriticalSection(&conn->requestLock);
	DeleteCriticalSection(&conn->stateLock);
	DeleteCriticalSection(&conn->callbacksLock);
	free(conn->replyBlob);
	free(conn);
}

bool LdndAddDataCallback(struct LdndConnection* conn, LdndDataCallback callback, void* context) {
	bool added = false;
	EnterCriticalSection(&conn->callbacksLock);
	for (size_t i = 0; i < sizeof(conn->callbacks) / sizeof(conn->callbacks[0]); ++i) {
		if (!conn->callbacks[i].callback) {
			conn->callbacks[i].callback = callback;
			conn->callbacks[i].context = context;
			added = true;
			break;
		}
	}
	LeaveCriticalSection(&conn->callbacksLock);
	return added;
}

void LdndRemoveDataCallback(struct LdndConnection* conn, LdndDataCallback callback, void* context) {
	EnterCriticalSection(&conn->callbacksLock);
	for (size_t i = 0; i < sizeof(conn->callbacks) / sizeof(conn->callbacks[0]); ++i) {
		if (conn->callbacks[i].callback == callback && conn->callbacks[i].context == context) {
			conn->callbacks[i].callback = NULL;
			conn->callbacks[i].context = NULL;
			break;
		}
	}
	LeaveCriticalSection(&conn->callbacksLock);
}

// Sends a request and waits for the daemon's REPLY on the same socket id. On success the reply blob (if the caller
// asked for it) is returned in *blob / *blobLength and belongs to the caller.
static int _request(struct LdndConnection* conn, uint8_t op, uint32_t socketId, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                    const void* payload, size_t payloadLength, uint8_t** blob, size_t* blobLength) {
	if (conn->failed) {
		return LDND_ERR_PIPE;
	}
	EnterCriticalSection(&conn->requestLock);
	EnterCriticalSection(&conn->stateLock);
	conn->waiting = true;
	conn->replied = false;
	conn->waitSocket = socketId;
	free(conn->replyBlob);
	conn->replyBlob = NULL;
	conn->replyLength = 0;
	LeaveCriticalSection(&conn->stateLock);

	int result = LDND_OK;
	if (!_sendFrame(conn, op, socketId, arg0, arg1, arg2, payload, payloadLength)) {
		result = LDND_ERR_PIPE;
	}

	EnterCriticalSection(&conn->stateLock);
	if (result == LDND_OK) {
		DWORD deadline = GetTickCount() + REPLY_TIMEOUT_MS;
		while (!conn->replied && !conn->failed) {
			DWORD now = GetTickCount();
			if ((int32_t) (deadline - now) <= 0) {
				break;
			}
			SleepConditionVariableCS(&conn->replyCondition, &conn->stateLock, deadline - now);
		}
		if (conn->replied) {
			result = (int) conn->replyError;
			if (blob) {
				*blob = conn->replyBlob;
				*blobLength = conn->replyLength;
				conn->replyBlob = NULL;
			}
		} else {
			result = conn->failed ? LDND_ERR_PIPE : LDND_ERR_TIMEOUT;
		}
	}
	conn->waiting = false;
	LeaveCriticalSection(&conn->stateLock);
	LeaveCriticalSection(&conn->requestLock);
	return result;
}

int LdndSocket(struct LdndConnection* conn, int domain, int type, int protocol, uint32_t* socketId) {
	static uint32_t nextSocket = 0;
	if (!socketId) {
		return LDND_ERR_ARGS;
	}
	uint32_t sid = InterlockedIncrement((LONG*) &nextSocket);
	int result = _request(conn, OP_SOCKET, sid, (uint32_t) domain, (uint32_t) type, (uint32_t) protocol, NULL, 0, NULL, NULL);
	if (result == LDND_OK) {
		*socketId = sid;
	}
	return result;
}

int LdndBind(struct LdndConnection* conn, uint32_t socketId, const void* sockaddr, size_t length) {
	return _request(conn, OP_BIND, socketId, 0, 0, 0, sockaddr, length, NULL, NULL);
}

int LdndSetSockOpt(struct LdndConnection* conn, uint32_t socketId, int level, int option, const void* value, size_t length) {
	return _request(conn, OP_SETSOCKOPT, socketId, (uint32_t) level, (uint32_t) option, 0, value, length, NULL, NULL);
}

int LdndGetSockName(struct LdndConnection* conn, uint32_t socketId, void* out, size_t* length) {
	uint8_t* blob = NULL;
	size_t blobLength = 0;
	int result = _request(conn, OP_GETSOCKNAME, socketId, 0, 0, 0, NULL, 0, &blob, &blobLength);
	if (result == LDND_OK && out && length) {
		if (blobLength > *length) {
			blobLength = *length;
		}
		if (blobLength) {
			memcpy(out, blob, blobLength);
		}
		*length = blobLength;
	}
	free(blob);
	return result;
}

int LdndStart(struct LdndConnection* conn, uint32_t socketId) {
	return _request(conn, OP_START, socketId, 0, 0, 0, NULL, 0, NULL, NULL);
}

int LdndSendTo(struct LdndConnection* conn, uint32_t socketId, int flags, const void* data, size_t length) {
	if (conn->failed) {
		return LDND_ERR_PIPE;
	}
	return _sendFrame(conn, OP_SENDTO, socketId, (uint32_t) flags, 0, 0, data, length) ? LDND_OK : LDND_ERR_PIPE;
}

int LdndCloseSocket(struct LdndConnection* conn, uint32_t socketId) {
	if (conn->failed) {
		return LDND_ERR_PIPE;
	}
	return _sendFrame(conn, OP_CLOSE, socketId, 0, 0, 0, NULL, 0) ? LDND_OK : LDND_ERR_PIPE;
}

#else // !_WIN32

struct LdndConnection* LdndOpen(const char* pipePath) {
	(void) pipePath;
	return NULL;
}
void LdndClose(struct LdndConnection* conn) {
	(void) conn;
}
bool LdndAddDataCallback(struct LdndConnection* conn, LdndDataCallback callback, void* context) {
	(void) conn;
	(void) callback;
	(void) context;
	return false;
}
void LdndRemoveDataCallback(struct LdndConnection* conn, LdndDataCallback callback, void* context) {
	(void) conn;
	(void) callback;
	(void) context;
}
int LdndSocket(struct LdndConnection* conn, int domain, int type, int protocol, uint32_t* socketId) {
	(void) conn; (void) domain; (void) type; (void) protocol; (void) socketId;
	return LDND_ERR_PIPE;
}
int LdndBind(struct LdndConnection* conn, uint32_t socketId, const void* sockaddr, size_t length) {
	(void) conn; (void) socketId; (void) sockaddr; (void) length;
	return LDND_ERR_PIPE;
}
int LdndSetSockOpt(struct LdndConnection* conn, uint32_t socketId, int level, int option, const void* value, size_t length) {
	(void) conn; (void) socketId; (void) level; (void) option; (void) value; (void) length;
	return LDND_ERR_PIPE;
}
int LdndGetSockName(struct LdndConnection* conn, uint32_t socketId, void* out, size_t* length) {
	(void) conn; (void) socketId; (void) out; (void) length;
	return LDND_ERR_PIPE;
}
int LdndStart(struct LdndConnection* conn, uint32_t socketId) {
	(void) conn; (void) socketId;
	return LDND_ERR_PIPE;
}
int LdndSendTo(struct LdndConnection* conn, uint32_t socketId, int flags, const void* data, size_t length) {
	(void) conn; (void) socketId; (void) flags; (void) data; (void) length;
	return LDND_ERR_PIPE;
}
int LdndCloseSocket(struct LdndConnection* conn, uint32_t socketId) {
	(void) conn; (void) socketId;
	return LDND_ERR_PIPE;
}

#endif
