/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "main.h"

#include <mgba/internal/debugger/cli-debugger.h>

#ifdef ENABLE_SCRIPTING
#include <mgba/core/scripting.h>

#ifdef ENABLE_PYTHON
#include "platform/python/engine.h"
#endif
#endif

#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/input.h>
#include <mgba/core/serialize.h>
#include <mgba/core/thread.h>
#include <mgba/core/version.h>
#include <mgba/internal/gba/input.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/sio/rfu.h>
#endif

#include <mgba/feature/commandline.h>
#include <mgba-util/vfs.h>

#include <SDL.h>

#include <errno.h>
#include <signal.h>

#define PORT "sdl"

static void mSDLDeinit(struct mSDLRenderer* renderer);

static int mSDLRun(struct mSDLRenderer* renderer, struct mArguments* args);

static struct mStandardLogger _logger;

static struct VFile* _state = NULL;

static void _loadState(struct mCoreThread* thread) {
	mCoreLoadStateNamed(thread->core, _state, SAVESTATE_RTC);
}

#ifdef M_CORE_GBA
// The wireless adapter, attached the way the Qt frontend's Emulation > Wireless Adapter menu attaches it. rfu.backend
// (e.g. `-C rfu.backend=ldnd`, or the MGBA_RFU_BACKEND environment variable, which wins) names what carries its
// "air": "local" (other mGBA processes on this computer), "ldnd" (a Switch through ldnd; LDN_DAEMON names ldnd's
// pipe if it is not the default one), "esp32", or "none" (an adapter with nobody in range). The adapter writes
// rfu-trace.log in the working directory by default; rfu.trace (or MGBA_RFU_TRACE) can override its path.
static struct GBASIORFU _rfu;
static struct GBASIORFUBackend* _rfuBackend;
static bool _rfuCreated;

static const char* _rfuSetting(struct mCore* core, const char* env, const char* key) {
	const char* value = getenv(env);
	if (!value || !value[0]) {
		value = mCoreConfigGetValue(&core->config, key);
	}
	return value;
}

// Before the game starts; false if the backend named is not one there is.
static bool _createRFU(struct mCore* core) {
	const char* backend = _rfuSetting(core, "MGBA_RFU_BACKEND", "rfu.backend");
	if (!backend || !backend[0] || !strcmp(backend, "off") || core->platform(core) != mPLATFORM_GBA) {
		return true;
	}
	_rfuBackend = NULL;
	if (strcmp(backend, "none")) {
		_rfuBackend = GBASIORFUBackendCreate(backend);
		if (!_rfuBackend) {
			printf("Unknown wireless adapter backend \"%s\": use local, ldnd, esp32 or none.\n", backend);
			return false;
		}
	}
	GBASIORFUCreate(&_rfu, _rfuBackend);
	const char* trace = _rfuSetting(core, "MGBA_RFU_TRACE", "rfu.trace");
	if (!trace || !trace[0]) {
		trace = "rfu-trace.log";
	}
	GBASIORFUSetTraceFile(&_rfu, trace);
	GBASIORFUTrace(&_rfu, "APP    mGBA %s (SDL), wireless adapter backend \"%s\"", projectVersion, backend);
	printf("Wireless adapter: %s, logging to %s\n", backend, trace);
	_rfuCreated = true;
	return true;
}

// The emulation thread's start and clean callbacks: the adapter is on the link port for as long as the game runs.
static void _attachRFU(struct mCoreThread* thread) {
	if (_rfuCreated) {
		thread->core->setPeripheral(thread->core, mPERIPH_GBA_LINK_PORT, &_rfu.d);
	}
}

static void _detachRFU(struct mCoreThread* thread) {
	if (_rfuCreated) {
		thread->core->setPeripheral(thread->core, mPERIPH_GBA_LINK_PORT, NULL);
	}
}

static void _destroyRFU(void) {
	if (!_rfuCreated) {
		return;
	}
	GBASIORFUDestroy(&_rfu);
	GBASIORFUBackendDestroy(_rfuBackend);
	_rfuBackend = NULL;
	_rfuCreated = false;
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
	// Without a parent console (e.g. launched from Explorer), freopen("CONOUT$") fails and
	// leaves stdout closed, so the next printf trips the CRT's file handle validation.
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		freopen("CONOUT$", "w", stdout);
	}
#endif
	struct mSDLRenderer renderer = {0};

	struct mCoreOptions opts = {
		.useBios = true,
		.rewindEnable = true,
		.rewindBufferCapacity = 600,
		.rewindBufferInterval = 1,
		.audioBuffers = 1024,
		.videoSync = false,
		.audioSync = true,
		.volume = 0x100,
		.logLevel = mLOG_WARN | mLOG_ERROR | mLOG_FATAL,
	};

	struct mArguments args;
	struct mGraphicsOpts graphicsOpts;

	struct mSubParser subparser;

	mSubParserGraphicsInit(&subparser, &graphicsOpts);
	bool parsed = mArgumentsParse(&args, argc, argv, &subparser, 1);
	if (!args.fname && !args.showVersion) {
		parsed = false;
	}
	if (!parsed || args.showHelp) {
		usage(argv[0], NULL, NULL, &subparser, 1);
		mArgumentsDeinit(&args);
		return !parsed;
	}
	if (args.showVersion) {
		version(argv[0]);
		mArgumentsDeinit(&args);
		return 0;
	}

	if (!SDL_OK(SDL_Init(SDL_INIT_VIDEO))) {
		printf("Could not initialize video: %s\n", SDL_GetError());
		mArgumentsDeinit(&args);
		return 1;
	}

	renderer.core = mCoreFind(args.fname);
	if (!renderer.core) {
		printf("Could not run game. Are you sure the file exists and is a compatible game?\n");
		mArgumentsDeinit(&args);
		return 1;
	}

	if (!renderer.core->init(renderer.core)) {
		mArgumentsDeinit(&args);
		return 1;
	}

	renderer.core->baseVideoSize(renderer.core, &renderer.width, &renderer.height);
	renderer.ratio = graphicsOpts.multiplier;
	if (renderer.ratio == 0) {
		renderer.ratio = 1;
	}
	opts.width = renderer.width * renderer.ratio;
	opts.height = renderer.height * renderer.ratio;

	mInputMapInit(&renderer.core->inputMap, &GBAInputInfo);
	mCoreInitConfig(renderer.core, PORT);
	mArgumentsApply(&args, &subparser, 1, &renderer.core->config);

	mCoreConfigSetDefaultIntValue(&renderer.core->config, "logToStdout", true);
	mCoreConfigLoadDefaults(&renderer.core->config, &opts);
	mCoreLoadConfig(renderer.core);
	mStandardLoggerInit(&_logger);
	mStandardLoggerConfig(&_logger, &renderer.core->config);
	mLogSetDefaultLogger(&_logger.d);

	renderer.viewportWidth = renderer.core->opts.width;
	renderer.viewportHeight = renderer.core->opts.height;
	renderer.player.fullscreen = renderer.core->opts.fullscreen;
	renderer.player.windowUpdated = 0;

	renderer.lockAspectRatio = renderer.core->opts.lockAspectRatio;
	renderer.lockIntegerScaling = renderer.core->opts.lockIntegerScaling;
	renderer.interframeBlending = renderer.core->opts.interframeBlending;
	renderer.filter = renderer.core->opts.resampleVideo;

#ifdef BUILD_GL
	if (mSDLGLCommonInit(&renderer)) {
		mSDLGLCreate(&renderer);
	} else
#elif defined(BUILD_GLES2) || defined(USE_EPOXY)
	if (mSDLGLCommonInit(&renderer))
	{
		mSDLGLES2Create(&renderer);
	} else
#endif
	{
		mSDLSWCreate(&renderer);
	}

	if (!renderer.init(&renderer)) {
		mArgumentsDeinit(&args);
		mCoreConfigDeinit(&renderer.core->config);
		renderer.core->deinit(renderer.core);
		return 1;
	}

	renderer.player.bindings = &renderer.core->inputMap;
	mSDLInitBindingsGBA(&renderer.core->inputMap);
	mSDLInitEvents(&renderer.events);
	mSDLEventsLoadConfig(&renderer.events, mCoreConfigGetInput(&renderer.core->config));
	mSDLAttachPlayer(&renderer.events, &renderer.player, -1);
	mSDLPlayerLoadConfig(&renderer.player, mCoreConfigGetInput(&renderer.core->config));

#if SDL_VERSION_ATLEAST(2, 0, 0)
	renderer.core->setPeripheral(renderer.core, mPERIPH_RUMBLE, &renderer.player.rumble.d.d);
#endif

	int ret;

	// TODO: Use opts and config
	ret = mSDLRun(&renderer, &args);
	mSDLDetachPlayer(&renderer.events, &renderer.player);
	mInputMapDeinit(&renderer.core->inputMap);

	mSDLDeinit(&renderer);
	mStandardLoggerDeinit(&_logger);

	mArgumentsDeinit(&args);
	mCoreConfigFreeOpts(&opts);
	mCoreConfigDeinit(&renderer.core->config);
	renderer.core->deinit(renderer.core);

	return ret;
}

#if defined(_WIN32) && !defined(_UNICODE)
#include <mgba-util/string.h>

int wmain(int argc, wchar_t** argv) {
	char** argv8 = malloc(sizeof(char*) * argc);
	int i;
	for (i = 0; i < argc; ++i) {
		argv8[i] = utf16to8((uint16_t*) argv[i], wcslen(argv[i]) * 2);
	}
	__argv = argv8;
	int ret = main(argc, argv8);
	for (i = 0; i < argc; ++i) {
		free(argv8[i]);
	}
	free(argv8);
	return ret;
}
#endif

int mSDLRun(struct mSDLRenderer* renderer, struct mArguments* args) {
	struct mCoreThread thread = {
		.core = renderer->core
	};
#ifdef M_CORE_GBA
	if (!_createRFU(renderer->core)) {
		return 1;
	}
	thread.startCallback = _attachRFU;
	thread.cleanCallback = _detachRFU;
#endif
	if (!mCoreLoadFile(renderer->core, args->fname)) {
#ifdef M_CORE_GBA
		_destroyRFU();
#endif
		return 1;
	}
	mCoreAutoloadSave(renderer->core);
	mArgumentsApplyFileLoads(args, renderer->core);
#ifdef ENABLE_SCRIPTING
	struct mScriptBridge* bridge = mScriptBridgeCreate();
#ifdef ENABLE_PYTHON
	mPythonSetup(bridge);
#endif
#ifdef ENABLE_DEBUGGERS
	CLIDebuggerScriptEngineInstall(bridge);
#endif
#endif

#ifdef ENABLE_DEBUGGERS
	struct mDebugger debugger;
	mDebuggerInit(&debugger);
	bool hasDebugger = mArgumentsApplyDebugger(args, renderer->core, &debugger);

	if (hasDebugger) {
		mDebuggerAttach(&debugger, renderer->core);
		mDebuggerEnter(&debugger, DEBUGGER_ENTER_MANUAL, NULL);
#ifdef ENABLE_SCRIPTING
		mScriptBridgeSetDebugger(bridge, &debugger);
#endif
	} else {
		mDebuggerDeinit(&debugger);
	}
#endif

	renderer->audio.samples = renderer->core->opts.audioBuffers;
	renderer->audio.sampleRate = 44100;
	thread.logger.logger = &_logger.d;

	bool didFail = !mCoreThreadStart(&thread);

	if (!didFail) {
#if SDL_VERSION_ATLEAST(2, 0, 0)
		renderer->core->currentVideoSize(renderer->core, &renderer->width, &renderer->height);
		unsigned width = renderer->width * renderer->ratio;
		unsigned height = renderer->height * renderer->ratio;
		if (width != (unsigned) renderer->viewportWidth && height != (unsigned) renderer->viewportHeight) {
			SDL_SetWindowSize(renderer->window, width, height);
			renderer->player.windowUpdated = 1;
		}
		mSDLSetScreensaverSuspendable(&renderer->events, renderer->core->opts.suspendScreensaver);
		mSDLSuspendScreensaver(&renderer->events);
#endif
		if (mSDLInitAudio(&renderer->audio, &thread)) {
			if (args->savestate) {
				struct VFile* state = VFileOpen(args->savestate, O_RDONLY);
				if (state) {
					_state = state;
					mCoreThreadRunFunction(&thread, _loadState);
					_state = NULL;
					state->close(state);
				}
			}
			renderer->runloop(renderer, &thread);
			mSDLPauseAudio(&renderer->audio);
			if (mCoreThreadHasCrashed(&thread)) {
				didFail = true;
				printf("The game crashed!\n");
				mCoreThreadEnd(&thread);
			}
		} else {
			didFail = true;
			printf("Could not initialize audio.\n");
		}
#if SDL_VERSION_ATLEAST(2, 0, 0)
		mSDLResumeScreensaver(&renderer->events);
		mSDLSetScreensaverSuspendable(&renderer->events, false);
#endif

		mCoreThreadJoin(&thread);
	} else {
		printf("Could not run game. Are you sure the file exists and is a compatible game?\n");
	}
#ifdef M_CORE_GBA
	_destroyRFU();
#endif
	renderer->core->unloadROM(renderer->core);

#ifdef ENABLE_SCRIPTING
	mScriptBridgeDestroy(bridge);
#endif

#ifdef ENABLE_DEBUGGERS
	if (hasDebugger) {
		renderer->core->detachDebugger(renderer->core);
		mDebuggerDeinit(&debugger);
	}
#endif

	return didFail;
}

static void mSDLDeinit(struct mSDLRenderer* renderer) {
	mSDLDeinitEvents(&renderer->events);
	mSDLDeinitAudio(&renderer->audio);
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_DestroyWindow(renderer->window);
#endif

	renderer->deinit(renderer);

	SDL_Quit();
}
