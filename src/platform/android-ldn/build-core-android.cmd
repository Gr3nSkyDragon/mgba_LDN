@echo off
rem Builds mGBA's core (the same sources as the desktop build, including the RFU driver and the ESP32 backend) as a static
rem library per Android ABI, into core\<abi>\ (libmgba.a plus the generated include\mgba\flags.h the app compiles against).
rem Usage: build-core-android.cmd [abi ...]      default: arm64-v8a
setlocal
rem The Android SDK: set ANDROID_HOME (or ANDROID_SDK_ROOT) first; NDK_VERSION and CMAKE_VERSION can be overridden too.
set SDK=%ANDROID_HOME%
if "%SDK%"=="" set SDK=%ANDROID_SDK_ROOT%
if "%SDK%"=="" (
  echo Set ANDROID_HOME to the Android SDK folder.
  exit /b 1
)
if "%NDK_VERSION%"=="" set NDK_VERSION=26.3.11579264
if "%CMAKE_VERSION%"=="" set CMAKE_VERSION=3.22.1
set CMAKE=%SDK%\cmake\%CMAKE_VERSION%\bin\cmake.exe
set NINJA=%SDK%\cmake\%CMAKE_VERSION%\bin\ninja.exe
set NDK=%SDK%\ndk\%NDK_VERSION%
set SRC=%~dp0..\..\..
set ABIS=%*
if "%ABIS%"=="" set ABIS=arm64-v8a
for %%A in (%ABIS%) do (
  echo === %%A
  "%CMAKE%" -S "%SRC%" -B "%~dp0core-build\%%A" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_TOOLCHAIN_FILE="%NDK%\build\cmake\android.toolchain.cmake" -DANDROID_ABI=%%A -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release ^
    -DBUILD_QT=OFF -DBUILD_SDL=OFF -DBUILD_LIBRETRO=OFF -DBUILD_SHARED=OFF -DBUILD_STATIC=ON -DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF -DBUILD_HEADLESS=OFF -DBUILD_EXAMPLE=OFF -DBUILD_TEST=OFF -DBUILD_SUITE=OFF -DBUILD_PERF=OFF -DBUILD_UPDATER=OFF ^
    -DUSE_LDN_BROADCAST=OFF -DUSE_PNG=OFF -DUSE_ZLIB=ON -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF -DUSE_SQLITE3=OFF -DUSE_LZMA=OFF -DUSE_ELF=OFF -DUSE_EDITLINE=OFF -DUSE_DISCORD_RPC=OFF -DUSE_JSON_C=OFF -DUSE_FFMPEG=OFF ^
    -DENABLE_SCRIPTING=OFF -DBUILD_PYTHON=OFF -DM_CORE_GB=ON -DM_CORE_GBA=ON -DDISABLE_FRONTENDS=ON || exit /b 1
  "%CMAKE%" --build "%~dp0core-build\%%A" -j8 || exit /b 1
  if not exist "%~dp0core\%%A\include\mgba" mkdir "%~dp0core\%%A\include\mgba"
  copy /y "%~dp0core-build\%%A\libmgba.a" "%~dp0core\%%A\libmgba.a" >nul
  copy /y "%~dp0core-build\%%A\include\mgba\flags.h" "%~dp0core\%%A\include\mgba\flags.h" >nul
)
echo done
