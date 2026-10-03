/* Native link-session tracer for Game Boy cores (Gr3nSkyDragon fork). See linktrace.h.
 *
 * Two taps share one process-wide JSON Lines sink:
 *  - hooks: breakpoints at the routines the 3DS Virtual Console patches (hook table from pret's vc/*.patch.template),
 *    installed through a debugger module from the core thread at game start; and
 *  - bytes: every serial byte the GB lockstep driver exchanges between two linked cores.
 *
 * Record fields: t_ms, wall_ms, frame, src, instance, player, dir (tx|rx|evt|enter|serial), layer
 * (hook|serial|meta), routine, hook, label, seq, len, hex, regs, ram. */
#include <mgba/internal/gb/linktrace.h>

#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/debugger/debugger.h>
#include <mgba/internal/gb/gb.h>
#include <mgba-util/threading.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
typedef unsigned (*LinkTraceProgressFn)(void* ctx);
void LinkTraceWatchdogStart(const char* dir, LinkTraceProgressFn progress, void* ctx);
void LinkTraceWatchdogStop(void);
#endif

#define MAX_HOOKS 48
#define MAX_RAM 24
#define MAX_ROUTINES 24
#define MAX_PLAYERS 4
#define MAX_DUMP 2048
#define EXCHANGE_MAX 0x800
#define LINE_SLACK 1536

enum HookKind {
	KIND_EVENT,
	KIND_EXCHANGE
};

struct HookDef {
	char name[48];
	char label[72];
	char routine[48];
	enum HookKind kind;
	int bank;
	uint16_t addr;
};

struct RamDef {
	char name[48];
	uint16_t addr;
};

struct RoutineDef {
	char name[48];
	int bank;
	uint16_t addr;
};

struct GameDef {
	char title[24];
	char game[16];
	struct HookDef hooks[MAX_HOOKS];
	size_t nHooks;
	struct RamDef ram[MAX_RAM];
	size_t nRam;
	struct RoutineDef routines[MAX_ROUTINES];
	size_t nRoutines;
};

struct HookRuntime {
	ssize_t id;
	ssize_t retId;
	bool pending;
	uint16_t de;
	uint16_t bc;
};

struct GBLinkTrace {
	struct mDebuggerModule d;
	struct mCore* core;
	struct GB* gb;
	int player;
	char instance[32];
	bool attached;
	bool sinkHeld;
	bool bytes;
	struct GameDef game;
	struct HookRuntime rt[MAX_HOOKS];
	ssize_t exchangeBytesId;
	ssize_t linkMenuId;
};

// Process-wide sink shared by every core in the process (both windows of a multiplayer pair).
static FILE* sSink;
static Mutex sSinkMutex;
static bool sSinkMutexReady;
static int sSinkRefs;
static uint64_t sSinkStartMs;
static uint32_t sSeq;

// Registry so the lockstep tap can find the label of a core without a back pointer.
static struct {
	struct GB* gb;
	int player;
	char instance[32];
	bool bytes;
} sRegistry[MAX_PLAYERS];

#ifdef _WIN32
// Sum of the frame counters of every traced core; the watchdog treats a counter that stops moving as a stall.
static unsigned _progress(void* ctx) {
	UNUSED(ctx);
	unsigned total = 0;
	size_t i;
	for (i = 0; i < MAX_PLAYERS; ++i) {
		struct GB* gb = sRegistry[i].gb;
		if (gb) {
			total += gb->video.frameCounter;
		}
	}
	return total;
}
#endif

static uint64_t _nowMs(void) {
	struct timespec ts;
	if (!timespec_get(&ts, TIME_UTC)) {
		return 0;
	}
	return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) (ts.tv_nsec / 1000000L);
}

static const char* _option(struct mCore* core, const char* key, const char* env) {
	const char* value = core ? mCoreConfigGetValue(&core->config, key) : NULL;
	if (value && value[0]) {
		return value;
	}
	value = getenv(env);
	return value && value[0] ? value : NULL;
}

static bool _sinkAcquire(const char* dir) {
	if (!sSinkMutexReady) {
		MutexInit(&sSinkMutex);
		sSinkMutexReady = true;
	}
	MutexLock(&sSinkMutex);
	if (!sSink) {
		char path[1024];
		time_t now = time(NULL);
		struct tm* tmv = localtime(&now);
		char stamp[32] = "run";
		if (tmv) {
			strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tmv);
		}
		snprintf(path, sizeof(path), "%s/gbtrace_%s.jsonl", dir, stamp);
		sSink = fopen(path, "w");
		if (!sSink) {
			MutexUnlock(&sSinkMutex);
			return false;
		}
		sSinkStartMs = _nowMs();
		sSeq = 0;
	}
	++sSinkRefs;
	MutexUnlock(&sSinkMutex);
	return true;
}

static void _sinkRelease(void) {
	if (!sSinkMutexReady) {
		return;
	}
	MutexLock(&sSinkMutex);
	if (sSinkRefs > 0 && --sSinkRefs == 0 && sSink) {
		fclose(sSink);
		sSink = NULL;
	}
	MutexUnlock(&sSinkMutex);
}

static void _jsonEscape(char* out, size_t outSize, const char* in) {
	size_t o = 0;
	for (; *in && o + 2 < outSize; ++in) {
		if (*in == '"' || *in == '\\') {
			out[o++] = '\\';
			out[o++] = *in;
		} else if ((unsigned char) *in < 0x20) {
			out[o++] = '?';
		} else {
			out[o++] = *in;
		}
	}
	out[o] = '\0';
}

static char* _hexString(const uint8_t* data, size_t len) {
	static const char digits[] = "0123456789abcdef";
	char* out = malloc(len * 3 + 1);
	if (!out) {
		return NULL;
	}
	size_t i;
	for (i = 0; i < len; ++i) {
		out[i * 3] = digits[data[i] >> 4];
		out[i * 3 + 1] = digits[data[i] & 0xF];
		out[i * 3 + 2] = ' ';
	}
	out[len ? len * 3 - 1 : 0] = '\0';
	return out;
}

// Writes one record. `extra` is a pre-built JSON fragment (without a leading comma) or NULL.
static void _emit(struct GB* gb, const char* instance, int player, const char* dir, const char* layer, const char* routine,
                  const char* hook, const char* label, long len, const char* hex, const char* extra) {
	if (!sSink) {
		return;
	}
	char inst[64], rout[96], hk[96], lb[144];
	_jsonEscape(inst, sizeof(inst), instance ? instance : "");
	_jsonEscape(rout, sizeof(rout), routine ? routine : "");
	_jsonEscape(hk, sizeof(hk), hook ? hook : "");
	_jsonEscape(lb, sizeof(lb), label ? label : "");
	size_t hexLen = hex ? strlen(hex) : 0;
	size_t cap = LINE_SLACK + hexLen + (extra ? strlen(extra) : 0);
	char* line = malloc(cap);
	if (!line) {
		return;
	}
	uint64_t now = _nowMs();
	MutexLock(&sSinkMutex);
	if (sSink) {
		uint32_t seq = ++sSeq;
		int n = snprintf(line, cap, "{\"t_ms\":%llu,\"wall_ms\":%llu,\"frame\":%u,\"src\":\"mgba\",\"instance\":\"%s\",\"player\":%d,"
		                 "\"dir\":\"%s\",\"layer\":\"%s\"",
		                 (unsigned long long) (now - sSinkStartMs), (unsigned long long) now, gb ? gb->video.frameCounter : 0,
		                 inst, player, dir, layer);
		if (routine && routine[0]) {
			n += snprintf(line + n, cap - n, ",\"routine\":\"%s\"", rout);
		}
		if (hook && hook[0]) {
			n += snprintf(line + n, cap - n, ",\"hook\":\"%s\"", hk);
		}
		if (label && label[0]) {
			n += snprintf(line + n, cap - n, ",\"label\":\"%s\"", lb);
		}
		n += snprintf(line + n, cap - n, ",\"seq\":%u", seq);
		if (len >= 0) {
			n += snprintf(line + n, cap - n, ",\"len\":%ld", len);
		}
		if (hex) {
			n += snprintf(line + n, cap - n, ",\"hex\":\"%s\"", hex);
		}
		if (extra && extra[0]) {
			n += snprintf(line + n, cap - n, ",%s", extra);
		}
		snprintf(line + n, cap - n, "}\n");
		fputs(line, sSink);
		fflush(sSink);
	}
	MutexUnlock(&sSinkMutex);
	free(line);
}

void GBLinkTraceSerial(struct GB* gb, int player, uint8_t tx, uint8_t rx) {
	if (!sSink) {
		return;
	}
	size_t i;
	for (i = 0; i < MAX_PLAYERS; ++i) {
		if (sRegistry[i].gb == gb) {
			if (!sRegistry[i].bytes) {
				return;
			}
			char hex[8];
			snprintf(hex, sizeof(hex), "%02x", tx);
			char extra[48];
			snprintf(extra, sizeof(extra), "\"rx\":\"%02x\",\"lockstep_id\":%d", rx, player);
			_emit(gb, sRegistry[i].instance, sRegistry[i].player, "serial", "serial", "GBSIOLockstep", NULL, NULL, 1, hex, extra);
			return;
		}
	}
}

void GBLinkTraceLockstep(struct GB* gb, int lockstepId, const char* what, int phase, int32_t a, int32_t b) {
	if (!sSink) {
		return;
	}
	static volatile int sBudget = 6000;
	if (sBudget <= 0) {
		return;
	}
	--sBudget;
	size_t i;
	for (i = 0; i < MAX_PLAYERS; ++i) {
		if (sRegistry[i].gb == gb) {
			char extra[96];
			snprintf(extra, sizeof(extra), "\"phase\":%d,\"a\":%d,\"b\":%d,\"lockstep_id\":%d", phase, (int) a, (int) b, lockstepId);
			_emit(gb, sRegistry[i].instance, sRegistry[i].player, "evt", "lockstep", what, NULL, NULL, -1, NULL, extra);
			return;
		}
	}
}

#ifdef ENABLE_DEBUGGERS
static uint16_t _reg(struct GBLinkTrace* t, const char* name) {
	int32_t value = 0;
	if (!t->core->readRegister(t->core, name, &value)) {
		return 0;
	}
	return (uint16_t) value;
}

static uint8_t _read8(struct GBLinkTrace* t, uint16_t addr) {
	return (uint8_t) t->core->rawRead8(t->core, addr, -1);
}

// Dumps [addr, addr+len) from the current memory map as hex, capped at MAX_DUMP bytes.
static char* _dump(struct GBLinkTrace* t, uint16_t addr, size_t len) {
	if (len > MAX_DUMP) {
		len = MAX_DUMP;
	}
	uint8_t* buf = malloc(len ? len : 1);
	if (!buf) {
		return NULL;
	}
	size_t i;
	for (i = 0; i < len; ++i) {
		buf[i] = _read8(t, (uint16_t) (addr + i));
	}
	char* hex = _hexString(buf, len);
	free(buf);
	return hex;
}

static void _regsAndRam(struct GBLinkTrace* t, char* out, size_t size, bool withRam) {
	int n = snprintf(out, size, "\"regs\":{\"a\":%u,\"bc\":%u,\"de\":%u,\"hl\":%u,\"sp\":%u,\"pc\":%u}",
	                 _reg(t, "a") & 0xFF, _reg(t, "bc"), _reg(t, "de"), _reg(t, "hl"), _reg(t, "sp"), _reg(t, "pc"));
	if (!withRam || n < 0 || (size_t) n >= size) {
		return;
	}
	n += snprintf(out + n, size - n, ",\"ram\":{");
	size_t i;
	bool first = true;
	for (i = 0; i < t->game.nRam && (size_t) n < size - 64; ++i) {
		n += snprintf(out + n, size - n, "%s\"%s\":%u", first ? "" : ",", t->game.ram[i].name, _read8(t, t->game.ram[i].addr));
		first = false;
	}
	snprintf(out + n, size - n, "}");
}

static void _onExchangeCall(struct GBLinkTrace* t, size_t index) {
	struct HookDef* h = &t->game.hooks[index];
	struct HookRuntime* rt = &t->rt[index];
	uint16_t hl = _reg(t, "hl");
	rt->de = _reg(t, "de");
	rt->bc = _reg(t, "bc");
	rt->pending = true;
	char extra[256];
	_regsAndRam(t, extra, sizeof(extra), false);
	char* hex = rt->bc <= EXCHANGE_MAX ? _dump(t, hl, rt->bc) : NULL;
	_emit(t->gb, t->instance, t->player, "tx", "hook", "Serial_ExchangeBytes", h->name, h->label, rt->bc, hex ? hex : "", extra);
	free(hex);
}

static void _onExchangeReturn(struct GBLinkTrace* t, size_t index) {
	struct HookDef* h = &t->game.hooks[index];
	struct HookRuntime* rt = &t->rt[index];
	if (!rt->pending) {
		return;
	}
	rt->pending = false;
	char extra[256];
	_regsAndRam(t, extra, sizeof(extra), false);
	char* hex = rt->bc <= EXCHANGE_MAX ? _dump(t, rt->de, rt->bc) : NULL;
	_emit(t->gb, t->instance, t->player, "rx", "hook", "Serial_ExchangeBytes", h->name, h->label, rt->bc, hex ? hex : "", extra);
	free(hex);
}

static void _onEvent(struct GBLinkTrace* t, size_t index) {
	struct HookDef* h = &t->game.hooks[index];
	char extra[1024];
	_regsAndRam(t, extra, sizeof(extra), true);
	_emit(t->gb, t->instance, t->player, "evt", "hook", h->routine, h->name, h->label, -1, NULL, extra);
}

// Entry of Serial_ExchangeBytes: calls from hooked sites are recorded by their hook; any other caller is logged with its data.
static void _onExchangeEntry(struct GBLinkTrace* t) {
	uint16_t sp = _reg(t, "sp");
	uint16_t caller = (uint16_t) (_read8(t, sp) | (_read8(t, (uint16_t) (sp + 1)) << 8));
	uint16_t hl = _reg(t, "hl");
	uint16_t bc = _reg(t, "bc");
	const char* known = NULL;
	size_t i;
	for (i = 0; i < t->game.nHooks; ++i) {
		if (t->game.hooks[i].kind == KIND_EXCHANGE && (uint16_t) (t->game.hooks[i].addr + 3) == caller) {
			known = t->game.hooks[i].name;
			break;
		}
	}
	char regs[256];
	_regsAndRam(t, regs, sizeof(regs), false);
	char extra[512];
	if (known) {
		snprintf(extra, sizeof(extra), "\"caller\":%u,\"known_hook\":\"%s\",%s", caller, known, regs);
	} else {
		snprintf(extra, sizeof(extra), "\"caller\":%u,\"unmapped_caller\":true,%s", caller, regs);
	}
	char* hex = (!known && bc <= EXCHANGE_MAX) ? _dump(t, hl, bc) : NULL;
	_emit(t->gb, t->instance, t->player, "enter", "serial", "Serial_ExchangeBytes", NULL, NULL, bc, hex, extra);
	free(hex);
}

static void _onLinkMenu(struct GBLinkTrace* t) {
	char extra[1024];
	_regsAndRam(t, extra, sizeof(extra), true);
	_emit(t->gb, t->instance, t->player, "enter", "serial", "Serial_ExchangeLinkMenuSelection", NULL, NULL, -1, NULL, extra);
}

static void _moduleNoop(struct mDebuggerModule* module) {
	UNUSED(module);
}

static void _modulePaused(struct mDebuggerModule* module, int32_t timeoutMs) {
	UNUSED(module);
	UNUSED(timeoutMs);
}

static void _moduleEntered(struct mDebuggerModule* module, enum mDebuggerEntryReason reason, struct mDebuggerEntryInfo* info) {
	struct GBLinkTrace* t = (struct GBLinkTrace*) module;
	module->isPaused = false;
	if (reason != DEBUGGER_ENTER_BREAKPOINT) {
		return;
	}
	if (info->pointId == t->exchangeBytesId) {
		_onExchangeEntry(t);
		return;
	}
	if (info->pointId == t->linkMenuId) {
		_onLinkMenu(t);
		return;
	}
	size_t i;
	for (i = 0; i < t->game.nHooks; ++i) {
		if (t->rt[i].id == info->pointId) {
			if (t->game.hooks[i].kind == KIND_EXCHANGE) {
				_onExchangeCall(t, i);
			} else {
				_onEvent(t, i);
			}
			return;
		}
		if (t->rt[i].retId == info->pointId) {
			_onExchangeReturn(t, i);
			return;
		}
	}
}

static ssize_t _setBreakpoint(struct GBLinkTrace* t, int bank, uint16_t addr) {
	struct mDebuggerPlatform* platform = t->d.p ? t->d.p->platform : NULL;
	if (!platform || !platform->setBreakpoint) {
		return -1;
	}
	struct mBreakpoint bp = {
		.address = addr,
		.segment = bank == 0 ? -1 : bank,
		.type = BREAKPOINT_HARDWARE,
	};
	return platform->setBreakpoint(platform, &t->d, &bp);
}
#endif

// Hook table format (text, '#' comments):
//   title POKEMON RED          starts a game section (header title; matched on its first 11 characters)
//   game red                   label used as the "instance" field
//   ram <name> <hexaddr>       WRAM/HRAM variable snapshotted on event hooks
//   routine <name> <bank> <hexaddr>
//   hook <name> <event|exchange_bytes> <bank> <hexaddr> <label> <routine>
static bool _loadGame(const char* path, const char* romTitle, struct GameDef* out) {
	FILE* f = fopen(path, "r");
	if (!f) {
		return false;
	}
	char line[512];
	bool inGame = false, found = false;
	memset(out, 0, sizeof(*out));
	while (fgets(line, sizeof(line), f)) {
		char a[64], b[96], c[96], d[96], e[96];
		unsigned bank, addr;
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
			continue;
		}
		if (strncmp(line, "title ", 6) == 0) {
			if (found) {
				break;
			}
			char title[32];
			strncpy(title, line + 6, sizeof(title) - 1);
			title[sizeof(title) - 1] = '\0';
			title[strcspn(title, "\r\n")] = '\0';
			inGame = strncmp(title, romTitle, 11) == 0;
			if (inGame) {
				found = true;
				strncpy(out->title, title, sizeof(out->title) - 1);
			}
			continue;
		}
		if (!inGame) {
			continue;
		}
		if (sscanf(line, "game %15s", out->game) == 1) {
			continue;
		}
		if (sscanf(line, "ram %47s %x", b, &addr) == 2 && out->nRam < MAX_RAM) {
			strncpy(out->ram[out->nRam].name, b, sizeof(out->ram[0].name) - 1);
			out->ram[out->nRam++].addr = (uint16_t) addr;
		} else if (sscanf(line, "routine %47s %u %x", b, &bank, &addr) == 3 && out->nRoutines < MAX_ROUTINES) {
			strncpy(out->routines[out->nRoutines].name, b, sizeof(out->routines[0].name) - 1);
			out->routines[out->nRoutines].bank = (int) bank;
			out->routines[out->nRoutines++].addr = (uint16_t) addr;
		} else if (sscanf(line, "hook %47s %63s %u %x %71s %47s", b, a, &bank, &addr, d, e) == 6 && out->nHooks < MAX_HOOKS) {
			struct HookDef* h = &out->hooks[out->nHooks++];
			strncpy(h->name, b, sizeof(h->name) - 1);
			strncpy(h->label, d, sizeof(h->label) - 1);
			strncpy(h->routine, e, sizeof(h->routine) - 1);
			h->kind = strcmp(a, "exchange_bytes") == 0 ? KIND_EXCHANGE : KIND_EVENT;
			h->bank = (int) bank;
			h->addr = (uint16_t) addr;
		}
		UNUSED(c);
	}
	fclose(f);
	return found;
}

struct GBLinkTrace* GBLinkTraceCreate(struct mCore* core, struct mDebugger* debugger) {
	if (!core || core->platform(core) != mPLATFORM_GB) {
		return NULL;
	}
	const char* dir = _option(core, "linkTrace.dir", "MGBA_LINKTRACE_DIR");
	if (!dir) {
		return NULL;
	}
	const char* hooksPath = _option(core, "linkTrace.hooks", "MGBA_LINKTRACE_HOOKS");
	const char* bytesOpt = _option(core, "linkTrace.bytes", "MGBA_LINKTRACE_BYTES");

	struct GBLinkTrace* t = calloc(1, sizeof(*t));
	if (!t) {
		return NULL;
	}
	t->core = core;
	t->gb = core->board;
	t->bytes = !bytesOpt || atoi(bytesOpt) != 0;
	t->exchangeBytesId = -1;
	t->linkMenuId = -1;
	size_t i;
	for (i = 0; i < MAX_HOOKS; ++i) {
		t->rt[i].id = -1;
		t->rt[i].retId = -1;
	}
	if (!_sinkAcquire(dir)) {
		free(t);
		return NULL;
	}
	t->sinkHeld = true;

	char romTitle[17] = {0};
	for (i = 0; i < 16; ++i) {
		uint32_t ch = core->rawRead8(core, 0x134 + i, 0);
		romTitle[i] = (ch >= 0x20 && ch < 0x7F) ? (char) ch : '\0';
		if (!romTitle[i]) {
			break;
		}
	}

	bool haveGame = hooksPath && _loadGame(hooksPath, romTitle, &t->game);
	snprintf(t->instance, sizeof(t->instance), "%s", haveGame && t->game.game[0] ? t->game.game : (romTitle[0] ? romTitle : "gb"));

	// Register for the byte-level lockstep tap.
	MutexLock(&sSinkMutex);
	for (i = 0; i < MAX_PLAYERS; ++i) {
		if (!sRegistry[i].gb) {
			sRegistry[i].gb = t->gb;
			sRegistry[i].player = (int) i;
			sRegistry[i].bytes = t->bytes;
			snprintf(sRegistry[i].instance, sizeof(sRegistry[i].instance), "%s", t->instance);
			t->player = (int) i;
			break;
		}
	}
	MutexUnlock(&sSinkMutex);

#ifdef _WIN32
	{
		const char* wd = _option(core, "linkTrace.watchdog", "MGBA_LINKTRACE_WATCHDOG");
		if (!wd || atoi(wd) != 0) {
			LinkTraceWatchdogStart(dir, _progress, NULL);
		}
	}
#endif

	size_t installed = 0, failed = 0;
#ifdef ENABLE_DEBUGGERS
	if (haveGame && debugger) {
		if (!core->debugger) {
			mDebuggerAttach(debugger, core);
		}
		t->d.type = DEBUGGER_CUSTOM;
		t->d.init = _moduleNoop;
		t->d.deinit = _moduleNoop;
		t->d.paused = _modulePaused;
		t->d.update = _moduleNoop;
		t->d.entered = _moduleEntered;
		t->d.custom = _moduleNoop;
		t->d.interrupt = _moduleNoop;
		t->d.isPaused = false;
		t->d.needsCallback = false;
		mDebuggerAttachModule(core->debugger, &t->d);
		t->attached = true;

		for (i = 0; i < t->game.nHooks; ++i) {
			struct HookDef* h = &t->game.hooks[i];
			t->rt[i].id = _setBreakpoint(t, h->bank, h->addr);
			bool ok = t->rt[i].id >= 0;
			if (h->kind == KIND_EXCHANGE) {
				// The hook address is the `call Serial_ExchangeBytes`; the peer's data is ready 3 bytes later.
				t->rt[i].retId = _setBreakpoint(t, h->bank, (uint16_t) (h->addr + 3));
				ok = ok && t->rt[i].retId >= 0;
			}
			ok ? ++installed : ++failed;
		}
		for (i = 0; i < t->game.nRoutines; ++i) {
			struct RoutineDef* r = &t->game.routines[i];
			if (strcmp(r->name, "Serial_ExchangeBytes") == 0) {
				t->exchangeBytesId = _setBreakpoint(t, r->bank, r->addr);
			} else if (strcmp(r->name, "Serial_ExchangeLinkMenuSelection") == 0) {
				t->linkMenuId = _setBreakpoint(t, r->bank, r->addr);
			}
		}
	}
#else
	UNUSED(debugger);
#endif

	char extra[320], title[64];
	_jsonEscape(title, sizeof(title), romTitle);
	snprintf(extra, sizeof(extra), "\"title\":\"%s\",\"hooks_file\":%s,\"hooks_matched\":%s,\"hooks_installed\":%u,\"hooks_failed\":%u,\"bytes\":%s",
	         title, hooksPath ? "true" : "false", haveGame ? "true" : "false", (unsigned) installed, (unsigned) failed,
	         t->bytes ? "true" : "false");
	_emit(t->gb, t->instance, t->player, "evt", "meta", "start", NULL, NULL, -1, NULL, extra);
	return t;
}

void GBLinkTraceDestroy(struct GBLinkTrace* t) {
	if (!t) {
		return;
	}
#ifdef ENABLE_DEBUGGERS
	if (t->attached && t->core && t->core->debugger) {
		mDebuggerDetachModule(t->core->debugger, &t->d);
	}
#endif
	size_t i;
	MutexLock(&sSinkMutex);
	for (i = 0; i < MAX_PLAYERS; ++i) {
		if (sRegistry[i].gb == t->gb) {
			sRegistry[i].gb = NULL;
		}
	}
	bool anyLeft = false;
	for (i = 0; i < MAX_PLAYERS; ++i) {
		anyLeft = anyLeft || sRegistry[i].gb;
	}
	MutexUnlock(&sSinkMutex);
#ifdef _WIN32
	if (!anyLeft) {
		LinkTraceWatchdogStop();
	}
#else
	UNUSED(anyLeft);
#endif
	if (t->sinkHeld) {
		_sinkRelease();
	}
	free(t);
}
