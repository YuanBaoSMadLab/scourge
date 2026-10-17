#ifndef SENSOR_H
#define SENSOR_H

void sensor_seed(unsigned int s);
int  sensor_read(void);      /* 0..1023 deterministic sample */
unsigned int sensor_rng(void);

#endif
