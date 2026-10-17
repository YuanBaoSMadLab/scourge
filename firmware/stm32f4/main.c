#include "hw.h"
#include "sys.h"
#include "uart.h"

static void led_on(void)  { GPIOD_MODER |= (1u << (12 * 2)); GPIOD_BSRR = (1u << 12); }
static void led_off(void) { GPIOD_MODER |= (1u << (12 * 2)); GPIOD_BSRR = (1u << (12 + 16)); }

int main(void)
{
  sys_init();
  systick_init(1000);
  uart_init();

  uart_puts("STM32F4-DISCO FIRMWARE booting...\r\n");

  int sum = 0;
  int i;
  for (i = 1; i <= 100; i++) sum += i;
  uart_puts("sum(1..100) = ");
  {
    char tmp[12]; int n = sum, k = 0;
    if (n == 0) tmp[k++] = '0';
    while (n) { tmp[k++] = (char)('0' + n % 10); n /= 10; }
    while (k) uart_putc(tmp[--k]);
  }
  uart_puts("\r\n");

  int blink;
  for (blink = 0; blink < 4; blink++) {
    led_on(); delay_ms(100); led_off(); delay_ms(100);
  }

  uart_puts("SMOKE OK\r\n");
  for (;;) { __asm__ volatile ("wfi"); }
}
