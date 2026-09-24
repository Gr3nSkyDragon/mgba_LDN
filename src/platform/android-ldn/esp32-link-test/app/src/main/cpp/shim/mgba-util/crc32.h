// Stand-in for mGBA's <mgba-util/crc32.h> so esp32-wire.c builds without the rest of mGBA.
#ifndef CRC32_SHIM_H
#define CRC32_SHIM_H
#include <stddef.h>
#include <stdint.h>
uint32_t crc32(uint32_t crc, const void* buf, size_t size);
#endif
