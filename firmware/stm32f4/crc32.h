#ifndef CRC32_H
#define CRC32_H

unsigned int crc32(const unsigned char* data, unsigned int len);
unsigned int crc32_update(unsigned int crc, const unsigned char* data, unsigned int len);

#endif
