#ifndef SYS_H
#define SYS_H

void sys_init(void);
void systick_init(unsigned int hz);
void delay_ms(unsigned int ms);
unsigned int get_tick(void);
void sys_tick_isr(void);

#endif
