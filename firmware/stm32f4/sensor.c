#include "sensor.h"

#define SENSOR_MAX 1023

static unsigned int rng_state = 0xC0FFEE01u;
static int tri_pos = 0;
static int tri_dir = 1;

void sensor_seed(unsigned int s)
{
  rng_state = s ? s : 1u;
  tri_pos = 0;
  tri_dir = 1;
}

unsigned int sensor_rng(void)
{
  unsigned int x = rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state = x;
  return x;
}

int sensor_read(void)
{
  int base = tri_pos;
  int noise;
  int v;

  tri_pos += tri_dir * 41;
  if (tri_pos >= SENSOR_MAX) { tri_pos = SENSOR_MAX; tri_dir = -1; }
  if (tri_pos <= 0)          { tri_pos = 0;          tri_dir = 1; }

  noise = (int)(sensor_rng() % 65u) - 32;
  v = base + noise;
  if (v < 0) v = 0;
  if (v > SENSOR_MAX) v = SENSOR_MAX;
  return v;
}
