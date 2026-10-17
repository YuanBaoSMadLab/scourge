#include "hw.h"
#include "sys.h"

static volatile unsigned int tick_ms = 0;

void sys_init(void)
{
  RCC_AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIODEN;
  RCC_APB1ENR |= RCC_APB1ENR_USART2EN;
  (void)RCC_CR; (void)RCC_CFGR;
}

void systick_init(unsigned int hz)
{
  unsigned int reload = 16000000u / hz - 1u;
  SYSTICK_LOAD = reload;
  SYSTICK_VAL = 0;
  SYSTICK_CTRL = SYSTICK_CTRL_CLKSOURCE | SYSTICK_CTRL_TICKINT | SYSTICK_CTRL_ENABLE;
}

unsigned int get_tick(void)
{
  return tick_ms;
}

void delay_ms(unsigned int ms)
{
  unsigned int start = tick_ms;
  while ((tick_ms - start) < ms) { __asm__ volatile ("wfi"); }
}

void sys_tick_isr(void)
{
  tick_ms++;
}
