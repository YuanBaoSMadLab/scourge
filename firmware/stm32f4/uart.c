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
