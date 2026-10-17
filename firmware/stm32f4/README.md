# Scourge STM32F407 裸机固件 + Renode 烧录测试

一个**完整、非玩具**的 Cortex-M4 裸机固件，用 LLVM/Clang 交叉编译，在 **Renode** 里"烧录"并验证行为。不是 blink，而是带真实数据通路与交互外壳的固件。

## 固件能力（main.c + 各模块）

一条完整的数据通路 + 一个交互式 UART 命令Shell：

```
sensor(确定性信号+噪声) → 移动平均滤波 → 运行统计(均值/方差/标准差) ┐
                                                                    ├→ UART Shell
                            CRC32(IEEE 802.3) ───────────────────────┘
```

| 模块 | 文件 | 作用 |
|---|---|---|
| 传感器 | `sensor.c` | xorshift32 PRNG + 三角波 + 有界噪声，输出 0..1023，**确定性可复现** |
| 滤波器 | `filter.c` | 环形缓冲移动平均，运行和 O(1)，窗口 1..64 可调 |
| 统计 | `stats.c` | 环形缓冲，min/max/mean/variance/stddev（整数 + 自写 `isqrt`） |
| 校验 | `crc32.c` | 表驱动 CRC32（多项式 `0xEDB88320`），带已知测试向量 |
| 外壳 | `shell.c` | 13 条命令：`help/info/sensor/sample/run/filter/stats/reset/crc/selftest/led/peek/poke` |
| 启动 | `startup.c` `stm32f4.ld` | 向量表、`.data` 拷贝、`.bss` 清零、`__stack_top` |
| 驱动 | `sys.c` `uart.c` `hw.h` `runtime.c` | 时钟/SysTick/LED、USART2、寄存器映射、EABI 助手 |

`selftest` 内置 5 项断言（CRC 已知向量、空串、移动平均、isqrt、常数方差），全部 PASS 才算通过。

## 构建（无需独立 arm-gcc，用 LLVM）

```bash
cd firmware/stm32f4
bash build.sh      # → firmware.elf / firmware.bin
```

工具链：`/d/LLVM/bin/clang.exe`（`--target=armv7m-none-eabi -mcpu=cortex-m4`）+ `ld.lld` + `llvm-objcopy`。
**关键编译选项 `-fno-builtin`**：否则 clang -O2 会把清零循环优化成 `memset` 调用，在 `-nostdlib` 下又解析回 `__aeabi_memclr` 自身 → 无限递归爆栈。

## 在 Renode 里"烧录"并测试

```bash
# 单次跑：烧录 + 启动 + 捕获 UART
python3 renode_run.py --wait 2 --cmds "selftest;sensor off;reset;filter 2;sample 10;sample 20;sample 30;stats"
```

`renode_run.py` 复现真实"烧录 + 复位"流程：`LoadBinary` 把 `firmware.bin` 写进 flash `0x08000000` **和 boot 别名 `0x00000000`**，CPU 复位从向量表取 SP/PC 启动，用 DR 写钩子抓取 UART 输出，用 `WriteChar` 注入 RX。

## 两个 Renode 致命坑（已解，务必记住）

1. **`0x0` boot 别名**：`platforms/cpus/stm32f4.repl` 只把 flash 映射在 `0x08000000`，没有 `0x0` 别名。Cortex-M 复位固定从 `0x0` 取初始 SP/PC → 取不到就 fault（PC 落 `0xEFFFFFFE`，固件根本不跑）。解法：`stm32f4_boot.repl` 加 `bootAlias: Memory.MappedMemory @ sysbus 0x0`，并把 bin 同时加载到 `0x08000000` 与 `0x0`。
2. **`-fno-builtin`**：见上文，否则 `__aeabi_memclr` 自递归爆栈。

> 注：`sysbus LoadELF` 对该核既不设 SP 也不设 PC；手动 `cpu SetRegisterUlong 13/15` 只在 CPU 未进 fault lockup 前有效。最稳的是让复位流程自己从正确的向量表启动（即坑 1 的解法）。

## 实测（Renode 捕获的 UART）

```
selftest: 5/5 ALL PASS
[crc32 "123456789"] =0xCBF43926 PASS
[ma4 100/200/300/400] =250 PASS
sensor off → reset → filter 2 → sample 10/20/30
stats: n=3 min=10 max=25 mean=16 var=39 std=6
```

## 命令一览

```
help            命令列表
info            固件信息 + uptime
sensor on|off   开关实时采样
run <n>         采 n 个实时样本
sample <v>      注入值 v（0..1023）进 滤波+统计
filter <n>      设移动平均窗口(1..64)
stats           打印 count/min/max/mean/var/std
reset           复位滤波与统计
crc <hex>       CRC32(十六进制字节)，如 crc 313233343536373839
selftest        内置测试
led on|off      GPIOD12 LED
peek/poke <hex> 读写内存字（调试）
```
