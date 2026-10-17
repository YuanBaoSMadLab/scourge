#ifndef UART_H
#define UART_H

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char* s);
char uart_getc(void);
int  uart_getline(char* buf, int maxlen);

int  uart_available(void);
void uart_putu(unsigned int v);
void uart_puti(int v);
void uart_puthex(unsigned int v, int width);

#endif
