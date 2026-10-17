// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
// ============================================================================
//  Scourge 编译器 — 裸机 crt 模板 · aarch64（Builder 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：--freestanding 通道的内置三件套源码模板——入口汇编（EL3/EL2/EL1
//  统一降到 EL1、栈、bss 清零、FPEN、向量表）、C 运行时（早期 MMU 按 1GB 块
//  属性映射、16550/PL011 双 UART 早期控制台、异常向量打印、libc freestanding
//  shim、退出/停机）与 GNU ld 链接脚本。板级差异（UART 型号/基址/步进/时钟、
//  载入基址、栈深、MMU 块属性）经 @TOKEN@ 由 BuilderBare.cpp 展开后写入生成
//  文件——模板只此一份，板级数据文件（boards/）与预设最终都落到同一 token 集。
//
//  契约（与 firmware/stm32f4 考古结论一致，务必保持）：
//    * -ffreestanding -fno-builtin 强制开启（AOT 后端注入）——否则 -O2 把
//      清零/拷贝循环 builtin 成 memset/memcpy 调用，freestanding 下解析回
//      自身或缺失符号（踩过：__aeabi_memclr 自递归爆栈）。
//    * Cortex-A 复位后进入 EL3/EL2/EL1 之一（U-Boot booti 常为 EL2，QEMU
//      virt 默认 EL1）——模板三级都处理，永远 ERET 到 EL1 再继续。
//    * MMU 开启前 UART 等外设地址裸访问可用，开启后经 device 属性块恒等
//      映射继续可用——映射恒等，无需切换地址。
// ============================================================================
#pragma once

#include <string>

namespace scourge {
namespace bare {

/// 模板 token 替换（@NAME@ → value；缺失 token 原样保留便于排查）。
inline std::string detok(const char* tmpl,
                         const std::string& name,
                         const std::string& value) {
    std::string s(tmpl);
    std::string key = "@" + name + "@";
    for (size_t pos = s.find(key); pos != std::string::npos;
         pos = s.find(key, pos)) {
        s.replace(pos, key.size(), value);
    }
    return s;
}

// ----------------------------------------------------------------------------
// 入口汇编：_start → 判 EL → 降 EL1 → 栈/bss/FPEN/向量表 → a64_mmu_init →
// a64_uart_init → main（LangToLLVM 生成的无参 main）→ a64_exit(返回码)。
// ----------------------------------------------------------------------------
inline const char kA64BootS[] = R"ASM(/* Scourge 裸机入口（--freestanding 生成；勿手工改）
 * 复位流程：EL3/EL2/EL1 统一 ERET 到 EL1 → 栈 → bss 清零 → FPEN →
 * 向量表 → MMU 恒等映射 → UART 早期控制台 → main。 */
    .section .text.boot, "ax"
    .global _start
_start:
    msr     daifset, #0xf           /* 早期无可用中断，全屏蔽 */
    mrs     x0, CurrentEL
    cmp     x0, #0xc
    b.eq    L_el3
    cmp     x0, #0x8
    b.eq    L_el2

L_el1:                              /* EL1 直入：向量表 + 栈 */
    msr     spsel, #1
    ldr     x0, =__vectors
    msr     vbar_el1, x0
    ldr     x19, =__stack_top
    mov     sp, x19
    /* FPEN=0b11：AArch64 基础 ISA 浮点默认可用，但部分 EL3 固件会留
     * 陷阱位——显式开启，浮点/NEON 指令不落异常 */
    mrs     x0, cpacr_el1
    orr     x0, x0, #(0x3 << 20)
    msr     cpacr_el1, x0
    isb
    /* .bss 清零（链接脚本保证 16 字节对齐；-fno-builtin 下不会被
     * builtin 成 memset——freestanding 纪律） */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
L_bss_loop:
    cmp     x0, x1
    b.hs    L_bss_done
    str     xzr, [x0], #8
    b       L_bss_loop
L_bss_done:
    bl      a64_uart_init
    bl      a64_mmu_init
    bl      main
    bl      a64_exit
L_hang:
    wfe
    b       L_hang

L_el3:                              /* EL3 → EL1（RW=1, NS=1） */
    ldr     x0, =0x433
    msr     scr_el3, x0
    ldr     x0, =0x3c5              /* SPSR: EL1h + DAIF 全屏蔽 */
    msr     spsr_el3, x0
    adr     x0, L_el1
    msr     elr_el3, x0
    eret

L_el2:                              /* EL2 → EL1（U-Boot booti 常见入口） */
    ldr     x0, =0x3c5
    msr     spsr_el2, x0
    adr     x0, L_el1
    msr     elr_el2, x0
    eret

/* ---- 异常向量表：16 槽（VBAR_EL1 要求 2KB 对齐，链接脚本保证） ----
 * 四类异常 × 四种来源全部汇入 a64_exc（打印 ESR/FAR/ELR 后停机）。 */
    .section .vectors, "ax"
    .balign 2048
    .global __vectors
__vectors:
    /* VBAR_EL1 每槽 128 字节步距（16 槽共 2KB），不足部分补零 */
    .rept 16
    b       a64_exc
    .space  124
    .endr
)ASM";

// ----------------------------------------------------------------------------
// C 运行时：MMU 恒等映射（MAIR + TTBR0 1GB 块，属性按 @GBn@）、
// 16550/PL011 UART、异常向量打印、libc freestanding shim、退出停机。
// ----------------------------------------------------------------------------
inline const char kA64CrtC[] = R"CRT(/* Scourge 裸机 C 运行时（--freestanding 生成；勿手工改）
 * 早期控制台与最小基础设施；print 系列在裸机运行时段（UART 通道）。 */
#include <stdint.h>

/* ---- 板级参数（BuilderBare.cpp 按 --board/--uart-* 展开注入） ---- */
#define SG_UART_KIND  @UART_KIND@   /* 0 = 16550（DW APB，RK 全系） 1 = PL011 */
#define SG_UART_BASE  @UART_BASE@ULL
#define SG_UART_SHIFT @UART_SHIFT@
#define SG_UART_CLK   @UART_CLK@ULL
#define SG_UART_BAUD  @UART_BAUD@

#define SG_GB0 @GB0@   /* MMU 1GB 块属性：1 = device nGnRE，0 = normal WB */
#define SG_GB1 @GB1@
#define SG_GB2 @GB2@
#define SG_GB3 @GB3@

/* ---- 最简同步原语 ---- */
static inline void a64_dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void a64_isb(void) { __asm__ volatile("isb" ::: "memory"); }
static inline void a64_tlbi_vmalle1(void) {
    __asm__ volatile("tlbi vmalle1" ::: "memory");
}
#define SG_MSR(reg, v) __asm__ volatile("msr " #reg ", %0" :: "r"(v))
#define SG_MRS(reg, out) __asm__ volatile("mrs %0, " #reg : "=r"(out))

/* 前置声明（mmu/uart init 内的调试输出先于定义处） */
void sg_uart_putc(char c);
void sg_uart_puts(const char* s);
void sg_uart_puti(long long v);

/* ---- MMU 早期映射：TTBR0/1 同表恒等映射低 4GB ----
 * T0SZ=32 → 起查 level 1 → 4 个 1GB block 描述符（属性逐 GB 注入）。
 * TTBR1 同表：T1SZ=16 的高半区走同一张表（表内容全 0 高位槽），高地址
 * 访问确定性 fault，而非走未初始化表的不确定行为。 */
extern char __pgtbl[];

void a64_mmu_init(void) {
    sg_uart_puts("[m0]");
    unsigned long long* tt = (unsigned long long*)__pgtbl;
    for (int i = 0; i < 512; ++i) tt[i] = 0;
    const int dev[4] = {SG_GB0, SG_GB1, SG_GB2, SG_GB3};
    for (int gb = 0; gb < 4; ++gb) {
        unsigned long long d = (unsigned long long)gb << 30;
        d |= (dev[gb] ? 1ull : 0ull) << 2; /* attridx：1 = device nGnRE */
        d |= 1ull;                         /* bits[1:0]=01：level1 block */
        d |= 1ull << 10;                   /* AF */
        if (dev[gb]) d |= 1ull << 54;      /* XN：设备页不可执行 */
        tt[gb] = d;
    }
    /* MAIR：attr0 = normal WB（0xff），attr1 = device nGnRE（0x04） */
    SG_MSR(mair_el1, 0x04ffull);
    /* TCR：T0SZ=32（4GB）、TG0=4KB、walk WB（IRGN0/ORGN0=01）、
     * 内共享（SH0=11）、T1SZ=16（bits[21:16]）+ TTBR1 同表 */
    SG_MSR(tcr_el1, 32ull | (1ull << 8) | (1ull << 10) | (3ull << 12) |
                        (16ull << 16));
    SG_MSR(ttbr0_el1, (unsigned long long)tt);
    SG_MSR(ttbr1_el1, (unsigned long long)tt);
    a64_dsb();
    a64_isb();
    unsigned long long s;
    SG_MRS(sctlr_el1, s);
    s |= (1ull << 0) | (1ull << 2) | (1ull << 12); /* M | C | I */
    SG_MSR(sctlr_el1, s);
    a64_tlbi_vmalle1();
    a64_dsb();
    a64_isb();
    sg_uart_puts("[m2]");
}

/* ---- UART 早期控制台（轮询，无中断/DMA） ---- */
static inline void sg_uart_wr(unsigned off, unsigned v) {
    *(volatile unsigned*)(SG_UART_BASE + ((unsigned)off << SG_UART_SHIFT)) = v;
}
static inline unsigned sg_uart_rd(unsigned off) {
    return *(volatile unsigned*)(SG_UART_BASE + ((unsigned)off << SG_UART_SHIFT));
}

void a64_uart_init(void) {
#if SG_UART_KIND == 0
    /* 16550 / DW APB：除数锁存 + 8N1 + FIFO */
    unsigned div = (unsigned)((SG_UART_CLK + 8ull * SG_UART_BAUD) /
                              (16ull * SG_UART_BAUD));
    sg_uart_wr(1, 0);          /* IER：关中断 */
    sg_uart_wr(3, 0x80);       /* LCR：DLAB=1 */
    sg_uart_wr(0, div & 0xff); /* DLL */
    sg_uart_wr(1, (div >> 8) & 0xff); /* DLM */
    sg_uart_wr(3, 0x03);       /* LCR：8N1，DLAB=0 */
    sg_uart_wr(2, 0x07);       /* FCR：使能并复位 FIFO */
#else
    /* PL011（QEMU virt / 树莓派 PL011） */
    sg_uart_wr(0x30, 0);       /* CR：先关 */
    sg_uart_wr(0x38, 0);       /* IMSC：关中断 */
    sg_uart_wr(0x44, 0x7ff);   /* ICR：清挂起 */
    unsigned div = (unsigned)(SG_UART_CLK / (16ull * SG_UART_BAUD));
    unsigned rem = (unsigned)(SG_UART_CLK % (16ull * SG_UART_BAUD));
    sg_uart_wr(0x24, div);     /* IBRD */
    sg_uart_wr(0x28, (rem * 4 + SG_UART_BAUD / 8) / (SG_UART_BAUD / 4)); /* FBRD */
    sg_uart_wr(0x2c, 0x70);    /* LCR_H：8 位 + FIFO */
    sg_uart_wr(0x30, 0x301);   /* CR：UARTEN|TXE|RXE */
#endif
    sg_uart_puts("[u]");
}

void sg_uart_putc(char c) {
#if SG_UART_KIND == 0
    while (!(sg_uart_rd(5) & 0x20)) { /* LSR.THRE */
    }
    sg_uart_wr(0, (unsigned char)c);
#else
    while (sg_uart_rd(0x18) & (1u << 5)) { /* FR.TXFF */
    }
    sg_uart_wr(0, (unsigned char)c);
#endif
}

void sg_uart_puts(const char* s) {
    for (; *s; ++s) sg_uart_putc(*s);
}

/* i64 十进制（退出码/panic 行号；负数补 '-'） */
void sg_uart_puti(long long v) {
    char buf[24];
    int i = 24;
    unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1
                                 : (unsigned long long)v;
    do {
        buf[--i] = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    if (v < 0) buf[--i] = '-';
    sg_uart_puts(buf + i);
}

void a64_exit(long long code) {
    sg_uart_puts("\n[scourge exit ");
    sg_uart_puti(code);
    sg_uart_puts("]\n");
    for (;;) __asm__ volatile("wfe");
}

/* ---- 异常向量：16 槽 × 128B，全部汇入打印 ESR/FAR/ELR 后停机 ---- */
void a64_exc(void) {
    unsigned long long esr, far, elr;
    SG_MRS(esr_el1, esr);
    SG_MRS(far_el1, far);
    SG_MRS(elr_el1, elr);
    sg_uart_puts("\nexception: esr=");
    sg_uart_puti((long long)esr);
    sg_uart_puts(" far=");
    sg_uart_puti((long long)far);
    sg_uart_puts(" elr=");
    sg_uart_puti((long long)elr);
    sg_uart_puts("\n");
    for (;;) __asm__ volatile("wfe");
}

/* ---- libc freestanding shim（编译器可能从生成 IR 直接发射调用） ----
 * 全部朴素字节循环：-fno-builtin 保证不被反优化成对自身的调用。 */
void* memset(void* d, int c, unsigned long long n) {
    unsigned char* p = (unsigned char*)d;
    for (unsigned long long i = 0; i < n; ++i) p[i] = (unsigned char)c;
    return d;
}

void* memcpy(void* d, const void* s, unsigned long long n) {
    unsigned char* p = (unsigned char*)d;
    const unsigned char* q = (const unsigned char*)s;
    for (unsigned long long i = 0; i < n; ++i) p[i] = q[i];
    return d;
}

void* memmove(void* d, const void* s, unsigned long long n) {
    unsigned char* p = (unsigned char*)d;
    const unsigned char* q = (const unsigned char*)s;
    if (p < q) {
        for (unsigned long long i = 0; i < n; ++i) p[i] = q[i];
    } else if (p > q) {
        for (unsigned long long i = n; i > 0; --i) p[i - 1] = q[i - 1];
    }
    return d;
}

int memcmp(const void* a, const void* b, unsigned long long n) {
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    for (unsigned long long i = 0; i < n; ++i) {
        if (p[i] != q[i]) return p[i] < q[i] ? -1 : 1;
    }
    return 0;
}

unsigned long long strlen(const char* s) {
    unsigned long long n = 0;
    while (s[n]) ++n;
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, unsigned long long n) {
    for (unsigned long long i = 0; i < n; ++i) {
        if (a[i] != b[i]) return (unsigned char)a[i] < (unsigned char)b[i]
                                      ? -1 : 1;
        if (!a[i]) return 0;
    }
    return 0;
}

char* strchr(const char* s, int c) {
    for (;; ++s) {
        if (*s == (char)c) return (char*)s;
        if (!*s) return 0;
    }
}

static char* sg_strstr_impl(const char* hay, const char* needle) {
    if (!*needle) return (char*)hay;
    for (; *hay; ++hay) {
        const char* h = hay;
        const char* n = needle;
        while (*h && *n && *h == *n) {
            ++h;
            ++n;
        }
        if (!*n) return (char*)hay;
    }
    return 0;
}

char* strstr(const char* hay, const char* needle) {
    return sg_strstr_impl(hay, needle);
}
)CRT";

// ----------------------------------------------------------------------------
// 链接脚本：恒等映射（VMA=LMA），.text.boot 起于 @LOAD_BASE@（bin 首字节
// 即 _start，配合 loader 直接跳转）；bss/pgtbl/stack 为 NOLOAD 不进 bin。
// ----------------------------------------------------------------------------
inline const char kA64LdT[] = R"LD(/* Scourge 裸机链接脚本（--freestanding 生成；勿手工改）
 * 恒等映射（VMA=LMA=@LOAD_BASE@）；bin 首字节即 _start（复位向量）。 */
ENTRY(_start)
SECTIONS
{
  . = @LOAD_BASE@;
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
  .stack (NOLOAD) : { . = ALIGN(16); . += @STACK_SIZE@; __stack_top = .; }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) }
}
)LD";

} // namespace bare
} // namespace scourge
