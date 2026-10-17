#include "stats.h"

static int  ring[STATS_CAP];
static int  head;
static int  count;
static long sum;

int isqrt(int v)
{
  int x, y;
  if (v <= 0) return 0;
  x = v;
  y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + v / x) / 2;
  }
  return x;
}

void stats_reset(void)
{
  int i;
  head = 0;
  count = 0;
  sum = 0;
  for (i = 0; i < STATS_CAP; i++) ring[i] = 0;
}

void stats_push(int x)
{
  if (count == STATS_CAP) {
    sum -= ring[head];
  } else {
    count++;
  }
  ring[head] = x;
  sum += x;
  head = (head + 1) % STATS_CAP;
}

int stats_count(void) { return count; }
long stats_sum(void)  { return sum; }

int stats_mean(void)
{
  if (count == 0) return 0;
  return (int)(sum / count);
}

int stats_min(void)
{
  int i, m, lim;
  if (count == 0) return 0;
  lim = count;
  m = ring[0];
  for (i = 1; i < lim; i++)
    if (ring[i] < m) m = ring[i];
  return m;
}

int stats_max(void)
{
  int i, m, lim;
  if (count == 0) return 0;
  lim = count;
  m = ring[0];
  for (i = 1; i < lim; i++)
    if (ring[i] > m) m = ring[i];
  return m;
}

long stats_variance(void)
{
  int i, lim, mean;
  long ssq;
  if (count < 1) return 0;
  lim = count;
  mean = stats_mean();
  ssq = 0;
  for (i = 0; i < lim; i++) {
    long d = (long)ring[i] - mean;
    ssq += d * d;
  }
  return ssq / count;
}

int stats_stddev(void)
{
  return isqrt((int)stats_variance());
}
