/* Stall watchdog for the native link tracer (Windows only). See linktrace.h.
 *
 * Multiplayer link sessions can deadlock (core threads waiting on each other, UI thread waiting on a core). A debugger is
 * not always at hand, so when the progress counter stops advancing this writes every thread's call stack, with symbol
 * names from the PDBs, to <dir>/stall_<timestamp>.txt. Deliberately includes no mGBA headers: mgba-util/common.h defines
 * `restrict`, which clashes with the Windows SDK headers. */
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef unsigned (*LinkTraceProgressFn)(void* ctx);

typedef BOOL(WINAPI* SymInitializeFn)(HANDLE, PCSTR, BOOL);
typedef DWORD(WINAPI* SymSetOptionsFn)(DWORD);
typedef BOOL(WINAPI* SymFromAddrFn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
typedef BOOL(WINAPI* SymGetLineFn)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
typedef BOOL(WINAPI* StackWalkFn)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                                  PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
typedef HRESULT(WINAPI* GetThreadDescFn)(HANDLE, PWSTR*);

static struct {
	HMODULE dbghelp;
	SymInitializeFn symInitialize;
	SymSetOptionsFn symSetOptions;
	SymFromAddrFn symFromAddr;
	SymGetLineFn symGetLine;
	StackWalkFn stackWalk;
	PFUNCTION_TABLE_ACCESS_ROUTINE64 funcTable;
	PGET_MODULE_BASE_ROUTINE64 moduleBase;
	GetThreadDescFn threadDesc;
	bool symsReady;
} sDbg;

static HANDLE sThread;
static HANDLE sStop;
static char sDir[1024];
static LinkTraceProgressFn sProgress;
static void* sProgressCtx;

static void _put(HANDLE file, const char* format, ...) {
	char buf[1024];
	va_list args;
	va_start(args, format);
	int n = _vsnprintf(buf, sizeof(buf) - 1, format, args);
	va_end(args);
	if (n < 0) {
		n = sizeof(buf) - 1;
	}
	DWORD written;
	WriteFile(file, buf, (DWORD) n, &written, NULL);
}

static bool _loadDbgHelp(void) {
	if (sDbg.symsReady) {
		return true;
	}
	sDbg.dbghelp = LoadLibraryA("dbghelp.dll");
	if (!sDbg.dbghelp) {
		return false;
	}
	sDbg.symInitialize = (SymInitializeFn) GetProcAddress(sDbg.dbghelp, "SymInitialize");
	sDbg.symSetOptions = (SymSetOptionsFn) GetProcAddress(sDbg.dbghelp, "SymSetOptions");
	sDbg.symFromAddr = (SymFromAddrFn) GetProcAddress(sDbg.dbghelp, "SymFromAddr");
	sDbg.symGetLine = (SymGetLineFn) GetProcAddress(sDbg.dbghelp, "SymGetLineFromAddr64");
	sDbg.stackWalk = (StackWalkFn) GetProcAddress(sDbg.dbghelp, "StackWalk64");
	sDbg.funcTable = (PFUNCTION_TABLE_ACCESS_ROUTINE64) GetProcAddress(sDbg.dbghelp, "SymFunctionTableAccess64");
	sDbg.moduleBase = (PGET_MODULE_BASE_ROUTINE64) GetProcAddress(sDbg.dbghelp, "SymGetModuleBase64");
	sDbg.threadDesc = (GetThreadDescFn) GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
	if (!sDbg.symInitialize || !sDbg.symFromAddr || !sDbg.stackWalk || !sDbg.funcTable || !sDbg.moduleBase) {
		return false;
	}
	if (sDbg.symSetOptions) {
		sDbg.symSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_INCLUDE_32BIT_MODULES);
	}
	// Search next to the executable (mgba.dll's PDB lives there) rather than relying on the default search path.
	char exeDir[MAX_PATH] = "";
	if (GetModuleFileNameA(NULL, exeDir, sizeof(exeDir)) > 0) {
		char* slash = strrchr(exeDir, '\\');
		if (slash) {
			*slash = '\0';
		}
	}
	sDbg.symsReady = sDbg.symInitialize(GetCurrentProcess(), exeDir[0] ? exeDir : NULL, TRUE) != FALSE;
	return sDbg.symsReady;
}

static void _dumpThread(HANDLE file, DWORD tid) {
	HANDLE process = GetCurrentProcess();
	HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
	if (!th) {
		_put(file, "\r\n-- thread %lu: cannot open (error %lu)\r\n", tid, GetLastError());
		return;
	}
	char name[128] = "";
	if (sDbg.threadDesc) {
		PWSTR wname = NULL;
		if (SUCCEEDED(sDbg.threadDesc(th, &wname)) && wname) {
			WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof(name) - 1, NULL, NULL);
			LocalFree(wname);
		}
	}
	_put(file, "\r\n-- thread %lu %s\r\n", tid, name);

	if (SuspendThread(th) == (DWORD) -1) {
		_put(file, "   (cannot suspend)\r\n");
		CloseHandle(th);
		return;
	}
	CONTEXT ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.ContextFlags = CONTEXT_FULL;
	if (GetThreadContext(th, &ctx)) {
		STACKFRAME64 frame;
		memset(&frame, 0, sizeof(frame));
		frame.AddrPC.Offset = ctx.Rip;
		frame.AddrFrame.Offset = ctx.Rbp;
		frame.AddrStack.Offset = ctx.Rsp;
		frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
		int i;
		for (i = 0; i < 40; ++i) {
			if (!sDbg.stackWalk(IMAGE_FILE_MACHINE_AMD64, process, th, &frame, &ctx, NULL, sDbg.funcTable, sDbg.moduleBase,
			                    NULL)) {
				break;
			}
			if (!frame.AddrPC.Offset) {
				break;
			}
			union {
				SYMBOL_INFO info;
				char raw[sizeof(SYMBOL_INFO) + 256];
			} sym;
			memset(&sym, 0, sizeof(sym));
			sym.info.SizeOfStruct = sizeof(SYMBOL_INFO);
			sym.info.MaxNameLen = 255;
			DWORD64 disp = 0;
			char where[320] = "?";
			if (sDbg.symFromAddr(process, frame.AddrPC.Offset, &disp, &sym.info)) {
				_snprintf(where, sizeof(where) - 1, "%s+0x%llx", sym.info.Name, (unsigned long long) disp);
			}
			char line[200] = "";
			if (sDbg.symGetLine) {
				IMAGEHLP_LINE64 il;
				memset(&il, 0, sizeof(il));
				il.SizeOfStruct = sizeof(il);
				DWORD ld = 0;
				if (sDbg.symGetLine(process, frame.AddrPC.Offset, &ld, &il)) {
					const char* base = strrchr(il.FileName, '\\');
					_snprintf(line, sizeof(line) - 1, "  [%s:%lu]", base ? base + 1 : il.FileName, il.LineNumber);
				}
			}
			_put(file, "   #%02d 0x%016llx %s%s\r\n", i, (unsigned long long) frame.AddrPC.Offset, where, line);
		}
	} else {
		_put(file, "   (GetThreadContext failed: %lu)\r\n", GetLastError());
	}
	ResumeThread(th);
	CloseHandle(th);
}

static void _dumpAll(void) {
	if (!_loadDbgHelp()) {
		return;
	}
	char path[1200];
	SYSTEMTIME st;
	GetLocalTime(&st);
	_snprintf(path, sizeof(path) - 1, "%s/stall_%04d%02d%02d-%02d%02d%02d.txt", sDir, st.wYear, st.wMonth, st.wDay, st.wHour,
	          st.wMinute, st.wSecond);
	HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}
	_put(file, "mGBA link tracer stall dump: progress counters stopped advancing.\r\n");
	_put(file, "Time %04d-%02d-%02d %02d:%02d:%02d, pid %lu\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
	     st.wSecond, GetCurrentProcessId());

	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap != INVALID_HANDLE_VALUE) {
		THREADENTRY32 te;
		te.dwSize = sizeof(te);
		DWORD self = GetCurrentThreadId();
		if (Thread32First(snap, &te)) {
			do {
				if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self) {
					_dumpThread(file, te.th32ThreadID);
				}
			} while (Thread32Next(snap, &te));
		}
		CloseHandle(snap);
	}
	_put(file, "\r\nend of dump\r\n");
	CloseHandle(file);
}

static DWORD WINAPI _watchdog(LPVOID unused) {
	(void) unused;
	unsigned last = sProgress(sProgressCtx);
	ULONGLONG lastChange = GetTickCount64();
	bool dumped = false;
	while (WaitForSingleObject(sStop, 500) == WAIT_TIMEOUT) {
		unsigned now = sProgress(sProgressCtx);
		if (now != last) {
			last = now;
			lastChange = GetTickCount64();
			dumped = false;
		} else if (!dumped && now > 0 && GetTickCount64() - lastChange >= 6000) {
			dumped = true;
			_dumpAll();
		}
	}
	return 0;
}

void LinkTraceWatchdogStart(const char* dir, LinkTraceProgressFn progress, void* ctx) {
	if (sThread || !dir || !progress) {
		return;
	}
	strncpy(sDir, dir, sizeof(sDir) - 1);
	sProgress = progress;
	sProgressCtx = ctx;
	sStop = CreateEventA(NULL, TRUE, FALSE, NULL);
	sThread = CreateThread(NULL, 0, _watchdog, NULL, 0, NULL);
}

void LinkTraceWatchdogStop(void) {
	if (!sThread) {
		return;
	}
	SetEvent(sStop);
	WaitForSingleObject(sThread, 3000);
	CloseHandle(sThread);
	CloseHandle(sStop);
	sThread = NULL;
	sStop = NULL;
}
