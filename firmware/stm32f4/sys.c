#include "hw.h"
#include "sys.h"

static volatile unsigned int tick_ms = 0;
static int led_state = 0;

void sys_init(void)
{
  RCC_AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIODEN;
  RCC_APB1ENR |= RCC_APB1ENR_USART2EN;
  (void)RCC_CR; (void)RCC_CFGR;
  led_init();
}

void led_init(void)
{
  GPIOD_MODER |= (1u << (12 * 2));
  GPIOD_BSRR = (1u << (12 + 16));
  led_state = 0;
}

void led_on(void)
{
  GPIOD_BSRR = (1u << 12);
  led_state = 1;
}

void led_off(void)
{
  GPIOD_BSRR = (1u << (12 + 16));
  led_state = 0;
}

void led_toggle(void)
{
  if (led_state) led_off(); else led_on();
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
