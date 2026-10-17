#ifndef STATS_H
#define STATS_H

#define STATS_CAP 256

void stats_reset(void);
void stats_push(int x);

int  stats_count(void);
long stats_sum(void);
int  stats_mean(void);
int  stats_min(void);
int  stats_max(void);
long stats_variance(void);
int  stats_stddev(void);

int  isqrt(int v);

#endif
