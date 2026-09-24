package io.mgbaldn.esp32test;

/** The native ESP32 session (shared C code from the mGBA tree, running against {@link UsbLink}). */
final class NativeProbe {
    static {
        System.loadLibrary("esp32test");
    }

    private NativeProbe() {}

    /** Blocks until the session ends. Returns 0 on full success. Must be called on a background thread. */
    static native int run(UsbLink link, int seconds);

    static native void stop();
}
