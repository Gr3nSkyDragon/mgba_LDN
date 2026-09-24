# mGBA LDN for Android

An Android app around the same mGBA core (with the Wireless Adapter driver and the ESP32 backend) as the desktop build. It
trades Generation 3 games with a Nintendo Switch through an ESP32 running the GB-Link Switch LDN firmware, connected to the
phone by USB (OTG). It joins only; the Switch must host.

The core is built from the sources in this repository (`src/gba/sio/esp32`, `src/gba/sio/rfu*.c`); the app adds only the
Android front end (`app/src/main/java`, video, audio, touch controls, USB) and a small JNI layer (`app/src/main/cpp/core_jni.c`).
The ESP32's serial port is provided to the shared backend through `Esp32SerialSetOps` (`src/gba/sio/esp32/esp32-serial.h`), backed
by `UsbLink.java`.

## Building

You need JDK 17, the Android SDK (platform 34, build tools 34, CMake 3.22.1) and NDK 26.3.11579264, plus Gradle 8.7.

1. Set `ANDROID_HOME` to the SDK folder and put the JDK in `JAVA_HOME`.
2. Build the core for the phone's ABI (default `arm64-v8a`):

       build-core-android.cmd arm64-v8a

   This writes `core/<abi>/libmgba.a`. Add other ABIs to the list *and* to `abiFilters` in `app/build.gradle`.
3. Create `local.properties` containing `sdk.dir=<the SDK folder, with forward slashes>`.
4. Run Gradle's `assembleRelease` (or `assembleDebug`) in this folder. The APK is `app/build/outputs/apk/`.

The release build is signed with the debug key, which is fine for sideloading but means later builds must reuse the same
key to update an installed copy.

## esp32-link-test

`esp32-link-test/` is a minimal app that runs only the ESP32 session (handshake, joining the Switch's room, receiving its
beacon) with a log you can copy or share. It was used to prove the USB path before the emulator existed and is useful for
diagnosing a board or a phone. It builds the same way (it needs no prebuilt core).

## Notes

- ROMs and saves live in `<Internal storage>/mGBA/ROMs` and `/Saves` when the app has all-files access, otherwise in the
  app's own folder. A ROM plays `<ROM name>.sav` unless another save was imported for it.
- The app records the wireless adapter protocol trace while the adapter is on; "Save diagnostic log" writes it to Downloads.
- Do not use fast-forward while trading.
