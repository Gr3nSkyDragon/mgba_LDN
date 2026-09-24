cmake_minimum_required(VERSION 3.22.1)
project(esp32test C)

# The shared, unmodified ESP32 sources from the mGBA tree.
set(ESP32_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../../gba/sio/esp32")

add_library(esp32test SHARED
    jni.c
    probe.c
    crc32.c
    ${ESP32_DIR}/esp32-wire.c
    ${ESP32_DIR}/esp32-serial.c)
target_include_directories(esp32test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim ${ESP32_DIR})
target_link_libraries(esp32test log)
