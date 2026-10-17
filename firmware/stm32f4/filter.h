#ifndef FILTER_H
#define FILTER_H

#define FILTER_MAX_WINDOW 64

void filter_init(void);
void filter_reset(void);
void filter_set_window(int w);
int  filter_window(void);
int  filter_push(int sample);   /* returns current moving average */
int  filter_value(void);        /* current moving average without pushing */

#endif
