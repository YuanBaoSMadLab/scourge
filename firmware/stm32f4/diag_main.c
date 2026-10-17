#include "hw.h"
#include "sys.h"
#include "uart.h"

#define MARK(a, v) (*(volatile unsigned int*)(a) = (v))

int main(void)
{
  MARK(0x20000000, 0xCAFEBABEu);
  sys_init();
  MARK(0x20000004, 0x11111111u);
  systick_init(1000);
  uart_init();
  MARK(0x20000008, 0x22222222u);

  uart_putc('A');
  MARK(0x2000000C, 0x33333333u);

  uart_puts("DIAG OK\r\n");
  MARK(0x20000010, 0x44444444u);

  for (;;) { __asm__ volatile ("wfi"); }
}
