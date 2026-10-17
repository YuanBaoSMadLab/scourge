#include "hw.h"
#include "sys.h"

extern unsigned int __stack_top;
extern int main(void);

void Reset_Handler(void);
void NMI_Handler(void)        { for (;;); }
void HardFault_Handler(void)  { for (;;); }
void SVC_Handler(void)        { for (;;); }
void PendSV_Handler(void)     { for (;;); }
void Default_Handler(void)    { for (;;); }

extern unsigned int _sidata;
extern unsigned int _sdata;
extern unsigned int _edata;
extern unsigned int _sbss;
extern unsigned int _ebss;

void SysTick_Handler(void);

__attribute__((section(".isr_vector"), used))
const void* vectors[] = {
  &__stack_top,
  Reset_Handler,
  NMI_Handler,
  HardFault_Handler,
  0, 0, 0, 0, 0, 0, 0,
  SVC_Handler,
  0,
  0,
  PendSV_Handler,
  SysTick_Handler,
};

void Reset_Handler(void)
{
  unsigned int* src = &_sidata;
  unsigned int* dst = &_sdata;
  while (dst < &_edata) *dst++ = *src++;
  unsigned int* bss = &_sbss;
  while (bss < &_ebss) *bss++ = 0;
  main();
  for (;;);
}

void SysTick_Handler(void)
{
  sys_tick_isr();
}
