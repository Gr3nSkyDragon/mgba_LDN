package io.mgbaldn.gba;

import java.nio.ByteBuffer;

/** The mGBA core (native/core_jni.c). All calls except setKeys must be made under Emulator's lock. */
final class Native {
    static {
        System.loadLibrary("mgbaldn");
    }

    private Native() {}

    /** Loads a ROM with the given save file (created if missing). {@code video} is a direct buffer of 256*224*4 bytes the core draws into (stride 256 pixels). */
    static native boolean load(String romPath, String savePath, ByteBuffer video);

    static native void unload();

    static native void reset();

    static native int width();

    static native int height();

    static native int sampleRate();

    /** Bit i = GBA key i (A, B, Select, Start, Right, Left, Up, Down, R, L). Safe from any thread. */
    static native void setKeys(int keys);

    /** Runs one frame; returns the number of stereo audio frames written into {@code audio}. */
    static native int runFrame(short[] audio);

    /** Folder the core keeps save files in (<folder>/<rom name>.sav). Takes effect the next time a ROM is loaded. */
    static native void setSaveDir(String path);

    /** Where the wireless adapter protocol trace goes (null: none). Takes effect the next time an adapter is attached. */
    static native void setTrace(String path);

    /** Adds a line (wall-clock stamped by the caller) to the adapter trace, if an adapter is attached. */
    static native void traceNote(String note);

    /** 0 = no adapter, 1 = wireless adapter (ESP32), 2 = cable adapter (RFU cable wrapper over the ESP32). */
    static native boolean setAdapter(int mode);

    /** The USB serial link the ESP32 backend talks through (UsbLink). */
    static native void setUsbLink(UsbLink link);
}
