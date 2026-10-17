#include "hw.h"
#include "uart.h"

void uart_init(void)
{
  RCC_AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
  RCC_APB1ENR |= RCC_APB1ENR_USART2EN;

  GPIOA_MODER &= ~((3u << (2 * 2)) | (3u << (3 * 2)));
  GPIOA_MODER |=  ((2u << (2 * 2)) | (2u << (3 * 2)));
  GPIOA_AFR0 &= ~((0xFu << (2 * 4)) | (0xFu << (3 * 4)));
  GPIOA_AFR0 |=  ((7u << (2 * 4)) | (7u << (3 * 4)));

  USART2_BRR = 139;
  USART2_CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

int uart_available(void)
{
  return (USART2_SR & USART_SR_RXNE) ? 1 : 0;
}

void uart_putc(char c)
{
  if (c == '\n') uart_putc('\r');
  while ((USART2_SR & USART_SR_TXE) == 0) {}
  USART2_DR = (unsigned int)(unsigned char)c;
}

void uart_puts(const char* s)
{
  while (*s) uart_putc(*s++);
}

char uart_getc(void)
{
  while ((USART2_SR & USART_SR_RXNE) == 0) {}
  return (char)(USART2_DR & 0xFFu);
}

int uart_getline(char* buf, int maxlen)
{
  int i = 0;
  while (i < maxlen - 1) {
    char c = uart_getc();
    if (c == '\r' || c == '\n') {
      uart_putc('\n');
      break;
    }
    if (c == 127 || c == 8) {
      if (i > 0) { i--; uart_puts("\b \b"); }
      continue;
    }
    if (c < 32) continue;
    buf[i++] = c;
    uart_putc(c);
  }
  buf[i] = 0;
  return i;
}

void uart_putu(unsigned int v)
{
  char tmp[10];
  int k = 0;
  if (v == 0) { uart_putc('0'); return; }
  while (v) { tmp[k++] = (char)('0' + (v % 10u)); v /= 10u; }
  while (k) uart_putc(tmp[--k]);
}

void uart_puti(int v)
{
  if (v < 0) { uart_putc('-'); uart_putu((unsigned int)(-(long)v)); }
  else uart_putu((unsigned int)v);
}

void uart_puthex(unsigned int v, int width)
{
  static const char hx[] = "0123456789ABCDEF";
  int i;
  for (i = width - 1; i >= 0; i--)
    uart_putc(hx[(v >> (i * 4)) & 0xFu]);
}
