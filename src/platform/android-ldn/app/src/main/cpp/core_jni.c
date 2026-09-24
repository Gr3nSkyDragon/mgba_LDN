// JNI glue between the Android app and mGBA's core (libmgba.a, built from the same sources as the desktop build).
//  - runs a game (load/reset/runFrame/setKeys), hands video to a Java direct buffer and audio to a short[]
//  - attaches the wireless adapter (the RFU driver) with the ESP32 backend, whose serial port is the app's USB link
//    (Esp32SerialSetOps: the seam added to esp32-serial.h), reached from the backend's own I/O thread via JNI.
#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/sio/rfu.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/vfs.h>
#include <fcntl.h>

// The seam (src/gba/sio/esp32/esp32-serial.h); declared here to avoid depending on the source directory layout.
struct Esp32Serial;
struct Esp32SerialOps {
	struct Esp32Serial* (*open)(const char* name, unsigned baud);
	void (*close)(struct Esp32Serial*);
	int (*read)(struct Esp32Serial*, uint8_t* buffer, size_t capacity);
	bool (*write)(struct Esp32Serial*, const void* data, size_t length);
	bool (*find)(char* out, size_t capacity);
};
void Esp32SerialSetOps(const struct Esp32SerialOps* ops);

#define TAG "mgba-ldn"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// ---- the emulated game --------------------------------------------------------------------------------------------

static struct mCore* gCore;
static mColor* gVideo;
static unsigned gWidth, gHeight;
static volatile uint32_t gKeys;

// The GBA's audio rate is not fixed: a game changes it through SOUNDBIAS (32768, 65536, 131072 or 262144 Hz; FireRed
// uses one of the high ones) and the core reports that through mAVStream.audioRateChanged, producing samples at the new
// rate. The output device runs at a fixed rate, so each frame's samples are resampled here (box-filtered down).
enum { kOutputRate = 32768, kMaxSourceFrames = 16384 };
static volatile unsigned gSourceRate = kOutputRate;
static double gPhase;
static double gSumL, gSumR;
static unsigned gCount;

static void _audioRateChanged(struct mAVStream* stream, unsigned rate) {
	(void) stream;
	if (rate) {
		gSourceRate = rate;
	}
}

static struct mAVStream gStream;

static struct GBASIORFU gRfu;
static struct GBASIORFUBackend* gBackend;
static bool gRfuAttached;
static char gTracePath[512];
static char gSaveDir[512];

static void _detachAdapter(void) {
	if (!gRfuAttached) {
		return;
	}
	if (gCore) {
		gCore->setPeripheral(gCore, mPERIPH_GBA_LINK_PORT, NULL);
	}
	GBASIORFUDestroy(&gRfu);
	GBASIORFUBackendDestroy(gBackend);
	gBackend = NULL;
	gRfuAttached = false;
}

static bool _attachAdapter(const char* name) {
	_detachAdapter();
	if (!gCore || !name || !name[0]) {
		return true;
	}
	gBackend = GBASIORFUBackendCreate(name);
	if (!gBackend) {
		LOGI("unknown wireless adapter backend %s", name);
		return false;
	}
	GBASIORFUCreate(&gRfu, gBackend);
	if (gTracePath[0]) {
		GBASIORFUSetTraceFile(&gRfu, gTracePath);
	}
	gCore->setPeripheral(gCore, mPERIPH_GBA_LINK_PORT, &gRfu.d);
	gRfuAttached = true;
	return true;
}

static void _unload(void) {
	_detachAdapter();
	if (gCore) {
		gCore->unloadROM(gCore);
		gCore->deinit(gCore);
		gCore = NULL;
	}
}

JNIEXPORT jboolean JNICALL Java_io_mgbaldn_gba_Native_load(JNIEnv* env, jclass clazz, jstring path, jstring savePath, jobject videoBuffer) {
	(void) clazz;
	_unload();
	const char* romPath = (*env)->GetStringUTFChars(env, path, NULL);
	struct mCore* core = mCoreFind(romPath);
	if (!core) {
		LOGI("no core for %s", romPath);
		(*env)->ReleaseStringUTFChars(env, path, romPath);
		return JNI_FALSE;
	}
	mCoreInitConfig(core, NULL);
	core->init(core);
	// A bare config has no defaults, so the options it maps are zero - including the volume, which silenced the game once
	// mCoreLoadForeignConfig (below, for the save folder) applied them.
	mCoreConfigSetDefaultIntValue(&core->config, "volume", 0x100);
	mCoreConfigSetDefaultIntValue(&core->config, "mute", 0);
	gVideo = (mColor*) (*env)->GetDirectBufferAddress(env, videoBuffer);
	// The buffer is 256x224 mColors: large enough for a GBA (240x160) and a Game Boy (160x144) frame, stride 256.
	core->setVideoBuffer(core, gVideo, 256);
	// Room for a whole frame at the highest rate (262144 Hz is ~4400 samples a frame); the core's own limit is 0x4000.
	core->setAudioBufferSize(core, kMaxSourceFrames);
	memset(&gStream, 0, sizeof(gStream));
	gStream.audioRateChanged = _audioRateChanged;
	core->setAVStream(core, &gStream);
	bool ok = mCoreLoadFile(core, romPath);
	(*env)->ReleaseStringUTFChars(env, path, romPath);
	if (!ok) {
		core->deinit(core);
		return JNI_FALSE;
	}
	// The save file is chosen by the app (by default <ROM name>.sav, or whichever save was imported for this ROM), and the core
	// reads and writes only that file.
	bool haveSave = false;
	if (savePath) {
		const char* saveFile = (*env)->GetStringUTFChars(env, savePath, NULL);
		struct VFile* vf = VFileOpen(saveFile, O_CREAT | O_RDWR);
		if (vf) {
			haveSave = core->loadSave(core, vf);
		}
		(*env)->ReleaseStringUTFChars(env, savePath, saveFile);
	}
	if (!haveSave) {
		mCoreAutoloadSave(core);
	}
	core->reset(core);
	core->currentVideoSize(core, &gWidth, &gHeight);
	gSourceRate = core->audioSampleRate(core);
	gPhase = 0;
	gSumL = gSumR = 0;
	gCount = 0;
	gCore = core;
	return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_io_mgbaldn_gba_Native_unload(JNIEnv* env, jclass clazz) {
	(void) env;
	(void) clazz;
	_unload();
}

JNIEXPORT void JNICALL Java_io_mgbaldn_gba_Native_reset(JNIEnv* env, jclass clazz) {
	(void) env;
	(void) clazz;
	if (gCore) {
		gCore->reset(gCore);
	}
}

JNIEXPORT jint JNICALL Java_io_mgbaldn_gba_Native_width(JNIEnv* env, jclass clazz) {
	(void) env;
	(void) clazz;
	return (jint) gWidth;
}

JNIEXPORT jint JNICALL Java_io_mgbaldn_gba_Native_height(JNIEnv* env, jclass clazz) {
	(void) env;
	(void) clazz;
	return (jint) gHeight;
}

JNIEXPORT jint JNICALL Java_io_mgbaldn_gba_Native_sampleRate(JNIEnv* env, jclass clazz) {
	(void) env;
	(void) clazz;
	return kOutputRate;
}

JNIEXPORT void JNICALL Java_io_mgbaldn_gba_Native_setKeys(JNIEnv* env, jclass clazz, jint keys) {
	(void) env;
	(void) clazz;
	gKeys = (uint32_t) keys;
}

// Runs one frame. The picture is left in the video buffer (alpha forced opaque); the audio is copied into `audio` as
// interleaved 16-bit stereo, and the number of stereo frames is returned (0 when no game is loaded).
JNIEXPORT jint JNICALL Java_io_mgbaldn_gba_Native_runFrame(JNIEnv* env, jclass clazz, jshortArray audio) {
	(void) clazz;
	if (!gCore) {
		return 0;
	}
	gCore->setKeys(gCore, gKeys);
	gCore->runFrame(gCore);
	gCore->currentVideoSize(gCore, &gWidth, &gHeight);
	for (unsigned y = 0; y < gHeight; ++y) {
		mColor* row = gVideo + (size_t) y * 256;
		for (unsigned x = 0; x < gWidth; ++x) {
			row[x] |= 0xFF000000u;
		}
	}

	struct mAudioBuffer* buffer = gCore->getAudioBuffer(gCore);
	size_t available = mAudioBufferAvailable(buffer);
	if (!available) {
		return 0;
	}
	static int16_t source[kMaxSourceFrames * 2];
	static int16_t output[kMaxSourceFrames * 2];
	if (available > kMaxSourceFrames) {
		available = kMaxSourceFrames;
	}
	size_t produced = mAudioBufferRead(buffer, source, available);
	double ratio = (double) kOutputRate / (double) gSourceRate; // output frames per source frame (<= 1)
	size_t written = 0;
	jsize capacity = (*env)->GetArrayLength(env, audio) / 2;
	for (size_t i = 0; i < produced && written < (size_t) capacity; ++i) {
		gSumL += source[i * 2];
		gSumR += source[i * 2 + 1];
		++gCount;
		gPhase += ratio;
		if (gPhase >= 1.0) {
			gPhase -= 1.0;
			output[written * 2] = (int16_t) (gSumL / gCount);
			output[written * 2 + 1] = (int16_t) (gSumR / gCount);
			++written;
			gSumL = gSumR = 0;
			gCount = 0;
		}
	}
	if (written) {
		(*env)->SetShortArrayRegion(env, audio, 0, (jsize) (written * 2), output);
	}
	return (jint) written;
}

JNIEXPORT void JNICALL Java_io_mgbaldn_gba_Native_setSaveDir(JNIEnv* env, jclass clazz, jstring path) {
	(void) clazz;
	const char* text = (*env)->GetStringUTFChars(env, path, NULL);
	snprintf(gSaveDir, sizeof(gSaveDir), "%s", text);
	(*env)->ReleaseStringUTFChars(env, path, text);
}

// A line in the adapter trace stamped with wall-clock time, so emulation speed on the phone can be read from the log.
JNIEXPORT void JNICALL Java_io_mgbaldn_gba_Native_traceNote(JNIEnv* env, jclass clazz, jstring note) {
	(void) clazz;
	if (!gRfuAttached) {
		return;
	}
	const char* text = (*env)->GetStringUTFChars(env, note, NULL);
	GBASIORFUTrace(&gRfu, "APP    %s", text);
	(*env)->ReleaseStringUTFChars(env, note, text);
}

JNIEXPORT void JNICALL Java_io_mgbaldn_gba_Native_setTrace(JNIEnv* env, jclass clazz, jstring path) {
	(void) clazz;
	if (!path) {
		gTracePath[0] = 0;
		return;
	}
	const char* text = (*env)->GetStringUTFChars(env, path, NULL);
	snprintf(gTracePath, sizeof(gTracePath), "%s", text);
	(*env)->ReleaseStringUTFChars(env, path, text);
}

// 0 = no adapter, 1 = ESP32. Called between frames (Java takes care of that), like the desktop menu's switching.
JNIEXPORT jboolean JNICALL Java_io_mgbaldn_gba_Native_setAdapter(JNIEnv* env, jclass clazz, jint mode) {
	(void) env;
	(void) clazz;
	return _attachAdapter(mode == 1 ? "esp32" : NULL) ? JNI_TRUE : JNI_FALSE;
}

// ---- the ESP32's serial port, provided by Java (UsbLink) --------------------------------------------------------

static JavaVM* gVm;
static jobject gLink;
static jmethodID gRead, gWrite, gPresent;
static pthread_key_t gThreadKey;
static pthread_once_t gKeyOnce = PTHREAD_ONCE_INIT;

enum { kBufferSize = 512 };

struct ThreadJni {
	JNIEnv* env;
	jbyteArray buffer;
};

static void _threadDone(void* value) {
	struct ThreadJni* tj = value;
	if (!tj) {
		return;
	}
	if (tj->buffer) {
		(*tj->env)->DeleteGlobalRef(tj->env, tj->buffer);
	}
	(*gVm)->DetachCurrentThread(gVm);
	free(tj);
}

static void _makeKey(void) {
	pthread_key_create(&gThreadKey, _threadDone);
}

// The backend calls the serial functions from its own I/O thread, which the JVM does not know: attach it on first use.
static struct ThreadJni* _tj(void) {
	pthread_once(&gKeyOnce, _makeKey);
	struct ThreadJni* tj = pthread_getspecific(gThreadKey);
	if (tj) {
		return tj;
	}
	JNIEnv* env = NULL;
	if ((*gVm)->AttachCurrentThread(gVm, &env, NULL) != JNI_OK) {
		return NULL;
	}
	tj = calloc(1, sizeof(*tj));
	tj->env = env;
	jbyteArray local = (*env)->NewByteArray(env, kBufferSize);
	tj->buffer = (*env)->NewGlobalRef(env, local);
	(*env)->DeleteLocalRef(env, local);
	pthread_setspecific(gThreadKey, tj);
	return tj;
}

struct Esp32Serial {
	int unused;
};
static struct Esp32Serial sPort;

static bool _find(char* out, size_t capacity) {
	struct ThreadJni* tj = _tj();
	if (!tj || !gLink || !(*tj->env)->CallBooleanMethod(tj->env, gLink, gPresent)) {
		return false;
	}
	strncpy(out, "usb", capacity);
	out[capacity - 1] = 0;
	return true;
}

static struct Esp32Serial* _open(const char* name, unsigned baud) {
	(void) name;
	(void) baud;
	char probe[8];
	return _find(probe, sizeof(probe)) ? &sPort : NULL; // Java already opened the device (permission, line coding)
}

static void _close(struct Esp32Serial* port) {
	(void) port;
}

static int _read(struct Esp32Serial* port, uint8_t* buffer, size_t capacity) {
	(void) port;
	struct ThreadJni* tj = _tj();
	if (!tj || !gLink) {
		return -1;
	}
	jint got = (*tj->env)->CallIntMethod(tj->env, gLink, gRead, tj->buffer);
	if (got > 0) {
		if ((size_t) got > capacity) {
			got = (jint) capacity;
		}
		(*tj->env)->GetByteArrayRegion(tj->env, tj->buffer, 0, got, (jbyte*) buffer);
	}
	return got;
}

static bool _write(struct Esp32Serial* port, const void* data, size_t length) {
	(void) port;
	struct ThreadJni* tj = _tj();
	if (!tj || !gLink) {
		return false;
	}
	const uint8_t* bytes = data;
	while (length) {
		size_t n = length < kBufferSize ? length : kBufferSize;
		(*tj->env)->SetByteArrayRegion(tj->env, tj->buffer, 0, (jsize) n, (const jbyte*) bytes);
		if (!(*tj->env)->CallBooleanMethod(tj->env, gLink, gWrite, tj->buffer, (jint) n)) {
			return false;
		}
		bytes += n;
		length -= n;
	}
	return true;
}

static const struct Esp32SerialOps kJavaOps = {_open, _close, _read, _write, _find};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
	(void) reserved;
	gVm = vm;
	return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL Java_io_mgbaldn_gba_Native_setUsbLink(JNIEnv* env, jclass clazz, jobject link) {
	(void) clazz;
	if (gLink) {
		(*env)->DeleteGlobalRef(env, gLink);
		gLink = NULL;
	}
	if (link) {
		gLink = (*env)->NewGlobalRef(env, link);
		jclass cls = (*env)->GetObjectClass(env, link);
		gRead = (*env)->GetMethodID(env, cls, "read", "([B)I");
		gWrite = (*env)->GetMethodID(env, cls, "write", "([BI)Z");
		gPresent = (*env)->GetMethodID(env, cls, "present", "()Z");
	}
	Esp32SerialSetOps(&kJavaOps);
}
