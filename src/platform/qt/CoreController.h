/* Copyright (c) 2013-2017 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QElapsedTimer>
#include <QMutex>
#include <QObject>

#include <atomic>
#include <QSize>
#include <QTimer>

#include "VFileDevice.h"

#include <functional>
#include <memory>

#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/thread.h>
#include <mgba/core/cache-set.h>

#ifdef M_CORE_GB
#include <mgba/internal/gb/sio/printer.h>
#include <mgba/internal/gb/sio/uds-gblink.h>
#endif
#ifdef M_CORE_GBA
#include <mgba/internal/gba/sio/dolphin.h>
#include <mgba/internal/gba/sio/rfu.h>
#include <mgba/internal/gba/sio/rfu-wrapper.h>
#include <mgba/internal/gba/sio/rfu-wrapper-air.h>
#include <mgba/internal/gba/sio/rfu-udp.h>
#endif

#ifdef M_CORE_GBA
#include <mgba/gba/interface.h>
#endif

struct GBLinkTrace;
struct mCore;

namespace QGBA {

class ConfigController;
class InputController;
class LogController;
class MemoryAccessLogController;
class MultiplayerController;
class Override;

class CoreController : public QObject {
Q_OBJECT

public:
	static const bool VIDEO_SYNC = false;
	static const bool AUDIO_SYNC = true;

	enum class Feature {
		OPENGL = mCORE_FEATURE_OPENGL,
	};

	class Interrupter {
	public:
		Interrupter();
		Interrupter(CoreController*);
		Interrupter(std::shared_ptr<CoreController>);
		Interrupter(const Interrupter&);
		~Interrupter();

		Interrupter& operator=(const Interrupter&);

		void interrupt(CoreController*);
		void interrupt(std::shared_ptr<CoreController>);
		void resume();

		bool held() const;

	private:
		void interrupt();
		void resume(CoreController*);

		CoreController* m_parent;
	};

	CoreController(mCore* core, QObject* parent = nullptr);
	~CoreController();

	mCoreThread* thread() { return &m_threadContext; }

	void setPath(const QString& path, const QString& base = {});
	QString path() const { return m_path; }
	QString baseDirectory() const { return m_baseDirectory; }
	QString savePath() const { return m_savePath; }

	const mColor* drawContext();
	QImage getPixels();

	bool isPaused();
	bool hasStarted();

	QString title() { return m_dbTitle.isNull() ? m_internalTitle : m_dbTitle; }
	QString intenralTitle() { return m_internalTitle; }
	QString dbTitle() { return m_dbTitle; }

	mPlatform platform() const;
	QSize screenDimensions() const;
	unsigned videoScale() const;
	bool supportsFeature(Feature feature) const { return m_threadContext.core->supportsFeature(m_threadContext.core, static_cast<mCoreFeature>(feature)); }
	bool hardwareAccelerated() const { return m_hwaccel; }

	void loadConfig(ConfigController*);

	mCheatDevice* cheatDevice() { return m_threadContext.core->cheatDevice(m_threadContext.core); }

#ifdef ENABLE_DEBUGGERS
	mDebugger* debugger() { return &m_debugger; }
	void attachDebugger(bool interrupt = true);
	void detachDebugger();
	void attachDebuggerModule(mDebuggerModule*, bool interrupt = true);
	void detachDebuggerModule(mDebuggerModule*);

	std::weak_ptr<MemoryAccessLogController> memoryAccessLogController();
#endif

	void setMultiplayerController(MultiplayerController*);
	void clearMultiplayerController();
	MultiplayerController* multiplayerController() { return m_multiplayer; }

#ifdef M_CORE_GBA
	bool isDolphinConnected() const { return !SOCKET_FAILED(m_dolphin.data); }
#endif

	mCacheSet* graphicCaches();
	int stateSlot() const { return m_stateSlot; }

	void setOverride(std::unique_ptr<Override> override);
	Override* override() { return m_override.get(); }

	void setInputController(InputController*);
	void setLogger(LogController*);

	bool audioSync() const { return m_audioSync; }
	bool videoSync() const { return m_videoSync; }

	void addFrameAction(std::function<void ()> callback);
	uint64_t frameCounter() const { return m_frameCounter; }

public slots:
	void start();
	void stop();
	void reset();
	void setPaused(bool paused);
	void frameAdvance();
	void setSync(bool enable);
	void showResetInfo(bool enable);

	void setRewinding(bool);
	void rewind(int count = 0);

	void setFastForward(bool);
	void forceFastForward(bool);

	void changePlayer(int id);

	void overrideMute(bool);

	void loadState(int slot = 0);
	void loadState(const QString& path, int flags = -1);
	void loadState(QIODevice* iodev, int flags = -1);
	void saveState(int slot = 0);
	void saveState(const QString& path, int flags = -1);
	void saveState(QIODevice* iodev, int flags = -1);
	void loadBackupState();
	void saveBackupState();

	void loadSave(const QString&, bool temporary);
	void loadSave(VFile*, bool temporary, const QString& path = {});
	void loadPatch(const QString&);
	void scanCard(const QString&);
	void scanCards(const QStringList&);
	void replaceGame(const QString&);
	void yankPak();
	void blockSave() { m_saveBlocked = true; }

	void addKey(int key);
	void clearKey(int key);
	void setAutofire(int key, bool enable);

#ifdef USE_PNG
	void screenshot();
#endif

	void setRealTime();
	void setFixedTime(const QDateTime& time);
	void setFakeEpoch(const QDateTime& time);
	void setTimeOffset(qint64 offset);

	void importSharkport(const QString& path);
	void exportSharkport(const QString& path);

#ifdef M_CORE_GB
	void attachPrinter();
	void detachPrinter();
	void endPrint();
#endif

#ifdef M_CORE_GBA
	void attachBattleChipGate();
	void detachBattleChipGate();
	void setRFUBackend(const QString&);
	void setRFULogging(bool enabled);
	void setRFUESP32Port(const QString& port);
	bool rfuEnabled() const;
	// The chosen backend drives the RFU Cable Wrapper instead of the wireless adapter.
	void setRFUCableWrapper(bool enabled);
	void setRFUWrapperLogging(bool enabled);
	bool rfuWrapperEnabled() const;
	// Virtual Console (Gen 1-2): the Game Boy link cable carried over UDS to the 3DS VC (src/gb/sio/uds-gblink.c). With the ESP32
	// backend it is on for any Game Boy game the wrapper knows (the wrapper runs on the board); with Local it needs this box, "Virtual
	// Console (local only)", and joins Azahar's UDS bridge.
	void setVCWrapper(bool enabled);
	bool vcWrapperRequested() const { return m_vcWrapper; }
	// The 3DS UDS key file the real-radio mode needs (Settings > BIOS), as a path the program can open.
	void setVCKeyFile(const QString& path);
	// The connection of the attached wireless adapter's backend, or else the RFU Cable Wrapper's wireless side, for the
	// status menu. False when neither is attached. `backend` gets its name, `wrapper` whether it is the wrapper's.
	bool rfuStatus(GBASIORFUBackendStatus* out, QString* backend, bool* wrapper) const;
	// "Check now": asks that backend to contact its device if it is idle.
	void probeRFU();
	void setBattleChipId(uint16_t id);
	void setBattleChipFlavor(int flavor);

	bool attachDolphin(const Address& address);
	void detachDolphin();
#endif

	void setAVStream(mAVStream*);
	void clearAVStream();

	void clearOverride();

	void startVideoLog(const QString& path, bool compression = true);
	void startVideoLog(VFile* vf, bool compression = true);
	void endVideoLog(bool closeVf = true);

	void setFramebufferHandle(int fb);

signals:
	void started();
	void paused();
	void unpaused();
	void stopping();
	void crashed(const QString& errorMessage);
	void failed();
	void frameAvailable();
	void didReset();
	void stateLoaded();
	void rewound();

	void rewindChanged(bool);
	void fastForwardChanged(bool);

	void unimplementedBiosCall(int);
	void statusPosted(const QString& message);
	void logPosted(int level, int category, const QString& log);

	void imagePrinted(const QImage&);

private:
	void updateKeys();
	int updateAutofire();
	void finishFrame();

	void updatePlayerSave();

	void updateFastForward();

	void updateROMInfo();

#ifdef M_CORE_GBA
	void attachRFU();
	void detachRFU();
	void updateRFUWrapperTrace();
	bool startRFUWrapper(const QString& connection);
	void stopRFUWrapper();
	bool startRFU(const QString& backend);
	void stopRFU();
	void applyRFU();
#endif

	mCoreThread m_threadContext{};
	struct CoreLogger : public mLogger {
		CoreController* self;
	} m_logger{};
	bool m_crashSeen = false;

	QString m_path;
	QString m_baseDirectory;
	QString m_savePath;

	bool m_patched = false;
	bool m_preload = true;
	bool m_saveBlocked = false;

	uint32_t m_crc32;
	QString m_internalTitle;
	QString m_dbTitle;
	bool m_showResetInfo = false;

	QByteArray m_activeBuffer;
	QByteArray m_completeBuffer;
	bool m_hwaccel = false;

	std::unique_ptr<mCacheSet> m_cacheSet;
	std::unique_ptr<Override> m_override;

	uint64_t m_frameCounter;
	QList<std::function<void()>> m_resetActions;
	QList<std::function<void()>> m_frameActions;
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
	QRecursiveMutex m_actionMutex;
#else
	QMutex m_actionMutex{QMutex::Recursive};
#endif
	int m_moreFrames = -1;
	QMutex m_bufferMutex;

	int m_activeKeys = 0;
	int m_removedKeys = 0;
	bool m_autofire[32] = {};
	int m_autofireStatus[32] = {};
	int m_autofireThreshold = 1;

	VFileDevice m_backupLoadState;
	QByteArray m_backupSaveState{nullptr};
	int m_stateSlot = 1;
	QString m_statePath;
	VFile* m_stateVf;
	int m_loadStateFlags;
	int m_saveStateFlags;

	bool m_audioSync = AUDIO_SYNC;
	bool m_videoSync = VIDEO_SYNC;

	bool m_autosave;
	bool m_autoload;
	int m_autosaveCounter = 0;

#ifdef ENABLE_DEBUGGERS
	struct mDebugger m_debugger;
#endif

	struct GBLinkTrace* m_linkTrace = nullptr;

	int m_fastForward = false;
	int m_fastForwardForced = false;
	int m_fastForwardVolume = -1;
	bool m_fastForwardMute = false;
	float m_fastForwardRatio = -1.f;
	float m_fastForwardHeldRatio = -1.f;
	float m_fpsTarget;

	bool m_mute = false;
	// Mute changes are applied on the core thread (see applyPendingMute) so that a window gaining focus never has to
	// interrupt a core that is blocked waiting on its multiplayer lockstep partner.
	std::atomic<bool> m_muteDirty{false};
	void applyPendingMute();

	InputController* m_inputController = nullptr;
	LogController* m_log = nullptr;
	MultiplayerController* m_multiplayer = nullptr;
#ifdef M_CORE_GBA
	GBASIODolphin m_dolphin;
#endif

#ifdef ENABLE_DEBUGGERS
	std::shared_ptr<MemoryAccessLogController> m_malController;
#endif

	mVideoLogContext* m_vl = nullptr;
	VFile* m_vlVf = nullptr;

#ifdef M_CORE_GB
	struct QGBPrinter : public GBPrinter {
		CoreController* parent;
	} m_printer;
#endif

#ifdef M_CORE_GBA
	GBASIOBattlechipGate m_battlechip;
	GBASIORFU m_rfu;
	GBASIORFUBackend* m_rfuBackend = nullptr;
	bool m_rfuAttached = false;
	QString m_rfuBackendName;
	QString m_rfuRequestedBackend = QStringLiteral("off"); // what the menu asked for, whether or not it is attached
	QString m_rfuEsp32Port; // the "ESP32" backend's serial port (e.g. "COM4"); empty auto-detects
	bool m_rfuLogEnabled = false; // "Save adapter log": write <config dir>/rfu-trace.log while an adapter is attached
	bool m_rfuTraceOn = false; // this controller currently holds a trace file
	MultiplayerController* m_rfuSavedMultiplayer = nullptr;
	GBASIORFUWrapper m_rfuWrapper;
	bool m_rfuWrapperAttached = false;
	QString m_rfuWrapperConnection; // the connection the attached wrapper was started with
	QByteArray m_rfuWrapperConnectionName;
	bool m_rfuCableWrapper = false; // the menu's "Cable wrapper": m_rfuRequestedBackend drives the wrapper
	bool m_vcWrapper = false; // the menu's "Virtual Console (local only)"
	void applyVC();
	void stopVC();
	GBVCLink* m_vcLink = nullptr;
	bool m_vcLinkBoard = false; // what the running link was made for (ESP32: the board; else Azahar's bridge), to restart it on a change
	QString m_vcLinkPort;
	QString m_vcLinkKey;
	QString m_vcKeyFile;
	bool m_rfuWrapperLogEnabled = false; // "Save adapter log" of the wrapper: <config dir>/rfu-wrapper-trace.log
	bool m_rfuWrapperTraceHeld = false; // this controller holds a reference on the cable trace file
	QByteArray m_eReaderData;
#endif
};

}
