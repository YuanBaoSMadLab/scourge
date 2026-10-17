#include "crc32.h"

static unsigned int crc_table[256];
static int crc_table_ready = 0;

static void crc_build_table(void)
{
  unsigned int i;
  int k;
  for (i = 0; i < 256; i++) {
    unsigned int c = i;
    for (k = 0; k < 8; k++)
      c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    crc_table[i] = c;
  }
  crc_table_ready = 1;
}

unsigned int crc32_update(unsigned int crc, const unsigned char* data, unsigned int len)
{
  unsigned int i;
  if (!crc_table_ready) crc_build_table();
  crc = ~crc;
  for (i = 0; i < len; i++)
    crc = crc_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  return ~crc;
}

unsigned int crc32(const unsigned char* data, unsigned int len)
{
  return crc32_update(0u, data, len);
}
