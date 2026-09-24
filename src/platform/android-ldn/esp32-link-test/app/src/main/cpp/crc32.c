// CRC-32/ISO-HDLC (the ordinary zlib crc32), bitwise; frames are small so speed is irrelevant.
#include <mgba-util/crc32.h>

uint32_t crc32(uint32_t crc, const void* buf, size_t size) {
	const uint8_t* p = buf;
	crc = ~crc;
	while (size--) {
		crc ^= *p++;
		for (int i = 0; i < 8; ++i) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
		}
	}
	return ~crc;
}
