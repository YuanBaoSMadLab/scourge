#ifndef HW_H
#define HW_H

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define RCC_BASE        0x40023800u
#define RCC_CR          (*(volatile u32*)(RCC_BASE + 0x00))
#define RCC_CFGR        (*(volatile u32*)(RCC_BASE + 0x08))
#define RCC_AHB1ENR     (*(volatile u32*)(RCC_BASE + 0x30))
#define RCC_APB1ENR     (*(volatile u32*)(RCC_BASE + 0x44))

#define GPIOA_BASE      0x40020000u
#define GPIOA_MODER     (*(volatile u32*)(GPIOA_BASE + 0x00))
#define GPIOA_ODR       (*(volatile u32*)(GPIOA_BASE + 0x14))
#define GPIOA_BSRR      (*(volatile u32*)(GPIOA_BASE + 0x18))
#define GPIOA_AFR0      (*(volatile u32*)(GPIOA_BASE + 0x20))

#define GPIOD_BASE      0x40020C00u
#define GPIOD_MODER     (*(volatile u32*)(GPIOD_BASE + 0x00))
#define GPIOD_ODR       (*(volatile u32*)(GPIOD_BASE + 0x14))
#define GPIOD_BSRR      (*(volatile u32*)(GPIOD_BASE + 0x18))

#define USART2_BASE     0x40004400u
#define USART2_SR       (*(volatile u32*)(USART2_BASE + 0x00))
#define USART2_DR       (*(volatile u32*)(USART2_BASE + 0x04))
#define USART2_BRR      (*(volatile u32*)(USART2_BASE + 0x08))
#define USART2_CR1      (*(volatile u32*)(USART2_BASE + 0x0C))

#define SYSTICK_BASE    0xE000E010u
#define SYSTICK_CTRL    (*(volatile u32*)(SYSTICK_BASE + 0x00))
#define SYSTICK_LOAD    (*(volatile u32*)(SYSTICK_BASE + 0x04))
#define SYSTICK_VAL     (*(volatile u32*)(SYSTICK_BASE + 0x08))

#define RCC_AHB1ENR_GPIOAEN  (1u << 0)
#define RCC_AHB1ENR_GPIODEN  (1u << 3)
#define RCC_APB1ENR_USART2EN (1u << 17)

#define USART_CR1_UE  (1u << 13)
#define USART_CR1_TE  (1u << 3)
#define USART_CR1_RE  (1u << 2)
#define USART_SR_TXE  (1u << 7)
#define USART_SR_RXNE (1u << 5)
#define USART_SR_TC   (1u << 6)

#define SYSTICK_CTRL_ENABLE (1u << 0)
#define SYSTICK_CTRL_TICKINT (1u << 1)
#define SYSTICK_CTRL_CLKSOURCE (1u << 2)

#endif
