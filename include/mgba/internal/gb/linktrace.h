/* Native link-session tracer for Game Boy cores (Gr3nSkyDragon fork).
 *
 * Writes JSON Lines transcripts of the link protocol so they can be diffed against 3DS Virtual Console / UDS
 * captures. This is the in-process alternative to scripting: Lua script loads interrupt the core thread, which
 * deadlocks when the core is waiting on a lockstep partner in a multiplayer pair, so the hooks are installed from the
 * core thread at game start instead (before any link exists) and nothing ever interrupts a running core.
 *
 * Enabled by configuration (config.ini key or -C option) or environment variable:
 *   linkTrace.dir    / MGBA_LINKTRACE_DIR     directory for the transcript; tracing is off when unset
 *   linkTrace.hooks  / MGBA_LINKTRACE_HOOKS   hook table text file (generated from the pret VC patch templates)
 *   linkTrace.bytes  / MGBA_LINKTRACE_BYTES   1 (default) logs every serial byte exchanged through the lockstep driver
 *
 * Contains no ROM data beyond the link buffers that are exchanged. */
#ifndef GB_LINKTRACE_H
#define GB_LINKTRACE_H

#include <mgba-util/common.h>

CXX_GUARD_START

struct GB;
struct mCore;
struct mDebugger;
struct GBLinkTrace;

// Called from the core thread when a Game Boy core starts. Returns NULL when tracing is not enabled.
struct GBLinkTrace* GBLinkTraceCreate(struct mCore* core, struct mDebugger* debugger);
void GBLinkTraceDestroy(struct GBLinkTrace*);

// Byte-level tap, called by the GB lockstep driver when a serial transfer completes for one player.
void GBLinkTraceSerial(struct GB* gb, int player, uint8_t tx, uint8_t rx);

// Lockstep decision tap (rate limited): what a node's lockstep update decided and why. `phase` is mLockstepPhase;
// a and b are context-dependent integers (for the secondary: unused posted cycles and the event delta).
void GBLinkTraceLockstep(struct GB* gb, int lockstepId, const char* what, int phase, int32_t a, int32_t b);

CXX_GUARD_END

#endif
