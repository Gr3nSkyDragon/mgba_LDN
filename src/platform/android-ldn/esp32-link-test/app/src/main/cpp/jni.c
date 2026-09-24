// JNI glue: installs a Java-backed Esp32SerialOps (the seam added to mGBA's esp32-serial.h) and runs the probe.
// Everything runs on the one Java thread that called NativeProbe.run, so the JNIEnv is valid throughout.
#include <jni.h>
#include <android/log.h>
#include <string.h>

#include "esp32-serial.h"
#include "probe.h"

static JNIEnv* gEnv;
static jobject gLink;
static jmethodID gRead;
static jmethodID gWrite;
static jmethodID gLogM;
static jmethodID gPresent;
static jbyteArray gBuffer;

enum { kBufferSize = 512 };

struct Esp32Serial {
	int unused;
};
static struct Esp32Serial sPort;

static struct Esp32Serial* _open(const char* name, unsigned baud) {
	(void) name;
	(void) baud;
	return &sPort; // the Java side already opened the device (permission + line coding) before calling run()
}

static void _close(struct Esp32Serial* port) {
	(void) port;
}

static int _read(struct Esp32Serial* port, uint8_t* buffer, size_t capacity) {
	(void) port;
	int want = capacity < kBufferSize ? (int) capacity : kBufferSize;
	(void) want;
	jint got = (*gEnv)->CallIntMethod(gEnv, gLink, gRead, gBuffer);
	if (got > 0) {
		(*gEnv)->GetByteArrayRegion(gEnv, gBuffer, 0, got > (jint) capacity ? (jint) capacity : got, (jbyte*) buffer);
	}
	return got;
}

static bool _write(struct Esp32Serial* port, const void* data, size_t length) {
	(void) port;
	const uint8_t* bytes = data;
	while (length) {
		size_t n = length < kBufferSize ? length : kBufferSize;
		(*gEnv)->SetByteArrayRegion(gEnv, gBuffer, 0, (jint) n, (const jbyte*) bytes);
		if (!(*gEnv)->CallBooleanMethod(gEnv, gLink, gWrite, gBuffer, (jint) n)) {
			return false;
		}
		bytes += n;
		length -= n;
	}
	return true;
}

static bool _find(char* out, size_t capacity) {
	if (!(*gEnv)->CallBooleanMethod(gEnv, gLink, gPresent)) {
		return false;
	}
	strncpy(out, "usb", capacity);
	out[capacity - 1] = 0;
	return true;
}

static const struct Esp32SerialOps kJavaOps = {_open, _close, _read, _write, _find};

static void _log(const char* line) {
	__android_log_print(ANDROID_LOG_INFO, "esp32test", "%s", line);
	jstring text = (*gEnv)->NewStringUTF(gEnv, line);
	(*gEnv)->CallVoidMethod(gEnv, gLink, gLogM, text);
	(*gEnv)->DeleteLocalRef(gEnv, text);
}

JNIEXPORT jint JNICALL Java_io_mgbaldn_esp32test_NativeProbe_run(JNIEnv* env, jclass clazz, jobject link, jint seconds) {
	(void) clazz;
	gEnv = env;
	gLink = link;
	jclass cls = (*env)->GetObjectClass(env, link);
	gRead = (*env)->GetMethodID(env, cls, "read", "([B)I");
	gWrite = (*env)->GetMethodID(env, cls, "write", "([BI)Z");
	gLogM = (*env)->GetMethodID(env, cls, "log", "(Ljava/lang/String;)V");
	gPresent = (*env)->GetMethodID(env, cls, "present", "()Z");
	jbyteArray local = (*env)->NewByteArray(env, kBufferSize);
	gBuffer = (*env)->NewGlobalRef(env, local);
	(*env)->DeleteLocalRef(env, local);

	Esp32SerialSetOps(&kJavaOps);
	int rc = probe_run((unsigned) seconds, _log);
	Esp32SerialSetOps(NULL);

	(*env)->DeleteGlobalRef(env, gBuffer);
	gBuffer = NULL;
	return rc;
}

JNIEXPORT void JNICALL Java_io_mgbaldn_esp32test_NativeProbe_stop(JNIEnv* env, jclass clazz) {
	(void) env;
	(void) clazz;
	probe_stop();
}
