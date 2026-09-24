cmake_minimum_required(VERSION 3.22.1)
project(mgbaldn C)

# libmgba.a per ABI comes from build-core-android.cmd (the same sources as the desktop build).
set(MGBA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../..") # the repository root (src/platform/android-ldn/app/src/main/cpp)
set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../core/${ANDROID_ABI}")

add_library(mgba STATIC IMPORTED)
set_target_properties(mgba PROPERTIES IMPORTED_LOCATION "${CORE_DIR}/libmgba.a")

add_library(mgbaldn SHARED core_jni.c)
target_include_directories(mgbaldn PRIVATE "${MGBA_ROOT}/include" "${CORE_DIR}/include")
# Must match the definitions the core was built with (they change struct layouts).
target_compile_definitions(mgbaldn PRIVATE
    BUILD_STATIC ENABLE_DEBUGGERS ENABLE_DIRECTORIES ENABLE_GDB_STUB ENABLE_VFS ENABLE_VFS_FD
    HAVE_CRC32 HAVE_FREELOCALE HAVE_FUTIMENS HAVE_FUTIMES HAVE_LOCALE HAVE_LOCALTIME_R HAVE_NEWLOCALE
    HAVE_PTHREAD_CREATE HAVE_PTHREAD_SETNAME_NP HAVE_REALPATH HAVE_SETLOCALE HAVE_STRDUP HAVE_STRLCPY HAVE_STRNDUP
    HAVE_STRTOF_L HAVE_USELOCALE HAVE_VASPRINTF HAVE_XLOCALE M_CORE_GB M_CORE_GBA USE_MINIZIP USE_PTHREADS USE_ZLIB)
target_link_libraries(mgbaldn mgba z log android m)
