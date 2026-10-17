/* Scourge 裸机链接脚本（--freestanding 生成；勿手工改）
 * 恒等映射（VMA=LMA=0x42000000）；bin 首字节即 _start（复位向量）。 */
ENTRY(_start)
SECTIONS
{
  . = 0x42000000;
  .text.boot : { KEEP(*(.text.boot)) }
  .vectors ALIGN(2048) : { KEEP(*(.vectors)) }
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) }
  .data : { *(.data .data.*) *(.sdata .sdata.*) }
  .bss (NOLOAD) : {
    . = ALIGN(16);
    __bss_start = .;
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  }
  .pgtbl (NOLOAD) : { . = ALIGN(4096); __pgtbl = .; . += 4096; }
  .heap_ctrl (NOLOAD) : { . = ALIGN(16); __heap_start = .; }
  .stack (NOLOAD) : { . = ALIGN(16); . += 1048576; __stack_top = .; }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) }
}
