#include "filter.h"

static struct {
  int   buf[FILTER_MAX_WINDOW];
  int   window;
  int   count;
  int   head;
  long  sum;
} ma;

void filter_init(void)
{
  int i;
  ma.window = 8;
  ma.count = 0;
  ma.head = 0;
  ma.sum = 0;
  for (i = 0; i < FILTER_MAX_WINDOW; i++) ma.buf[i] = 0;
}

void filter_reset(void)
{
  filter_init();
}

void filter_set_window(int w)
{
  if (w < 1) w = 1;
  if (w > FILTER_MAX_WINDOW) w = FILTER_MAX_WINDOW;
  filter_init();
  ma.window = w;
}

int filter_window(void)
{
  return ma.window;
}

int filter_push(int sample)
{
  if (ma.count == ma.window) {
    ma.sum -= ma.buf[ma.head];
  } else {
    ma.count++;
  }
  ma.buf[ma.head] = sample;
  ma.sum += sample;
  ma.head = (ma.head + 1) % ma.window;
  return filter_value();
}

int filter_value(void)
{
  if (ma.count == 0) return 0;
  return (int)(ma.sum / ma.count);
}
