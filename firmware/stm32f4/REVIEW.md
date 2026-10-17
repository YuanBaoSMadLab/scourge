# 硬件模块（firmware/stm32f4）审查报告

审查对象：`D:\Scourge\firmware\stm32f4\`（完整裸机固件 + Renode 烧录 harness）
审查方式：逐文件代码审读 + 宿主编译实证（`test_stats_host.c`）+ 链接符号分析（`llvm-nm -u`）。
结论先行：**功能与数据通路已验证正确，但有 8 个 P0 级输入/边界缺陷、6 个 P1 级可诊断性缺口、一批 P2 冗余；本次"考古十几轮才跑通"的根因不在代码逻辑，而在三个隐性知识缺口 + 无脚手架 + 无诊断，全部可工程化消除。**

---

## 一、P0 正确性问题（必须修，均为输入处理/边界）

| # | 位置 | 问题 | 后果 | 修法 |
|---|---|---|---|---|
| 1 | `shell.c:222` `run <n>` | n 只 clamp 到 ≥0，无上限 | `run 2000000000` 永久阻塞 shell（期间其他命令全不响应） | clamp 到 ≤10000，超出报 `usage: run <0..10000>` |
| 2 | `shell.c:250` `peek/poke` | 任意地址可读写 | 误 poke 到 NVIC/SysTick/向量表区直接破坏运行环境且无法恢复 | 限定可访问区（RAM 0x20000000–0x2002FFFF + 外设），flash/NVIC 拒绝并提示 |
| 3 | `shell.c:39` `parse_int` | 无溢出检查 `val*10+d` | `sample 99999999999` 静默回绕成垃圾值污染滤波器 | 累加前判 `val > (INT_MAX-d)/10` 则拒绝 |
| 4 | `uart.c:77` `uart_puthex` | `v >> (i*4)`，width>8 时移位量 ≥32 是 UB（ARM 上计数 mod 32，静默输出错位 hex） | 当前只以 8 调用，属潜伏 UB | `if (width > 8) width = 8;` |
| 5 | `sys.c:39` `systick_init` | `16000000u / hz`，hz=0 除零 | reload 变垃圾，SysTick 行为未定义（后续 delay_ms 死等） | `if (hz < 1) hz = 1;` |
| 6 | `uart.c:41` `uart_getline` | 缓冲满（63 字符）后静默丢字符直到回车 | 用户无感知，命令被截断 | 满时输出 `\a` 或 `[truncated]` 提示 |
| 7 | `shell.c:118` `cmd_crc` | 输入超 64 字节静默截断 | CRC 结果对应的是截断数据，用户以为算的是全文 | 截断时打印 `warn: truncated to 64 bytes` |
| 8 | `shell.c:26` `tokenize` | argv[8] 超长命令静默截断 | 同上，静默丢参 | 达上限时回显提示 |

另有两处"依赖实现定义"的次要项：`uart_puti(INT_MIN)`（`-(long)v` 在 32 位 long 上是有符号溢出 UB，当前两种 ABI 下恰好都输出正确，建议改写成无符号运算消除 UB）；`stats.c` 的 `min/max/variance` 线性扫描 `ring[0..count-1]` **当前是正确的**（已用 host 测试实证：满环时恰好覆盖全环，未满时 head 不会回绕），但正确性 silently 依赖"count 到 CAP 前 head 不回绕"这个隐式不变量——一旦将来加窗口化统计就会静默出错。建议遍历显式写成 `(head+i)%STATS_CAP` 并把不变量写进注释。

## 二、P1 健壮性 / 可诊断性（本次耗时十几轮 debug 的主因）

1. **`HardFault_Handler` 是 `for(;;)`，零诊断**（`startup.c:9`）。`__aeabi_memclr` 自递归爆栈后，如果 HardFault 打印 CFSR/HFSR/MMFAR/BFAR + 故障时的 SP，配合 `-g` 的 addr2line，几秒钟定位；实际是逐轮反汇编考古。**这是硬件模块最该补的基础设施**：一个会报死亡的固件，调试成本差一个数量级。
2. **无 host 侧单测**。filter/stats/crc32/sensor 全是纯逻辑，本可在 x86 上毫秒级回归；现在每次迭代 = build.sh + Renode 冷启动 + telnet，几十秒起。已附 `test_stats_host.c` 示范（`gcc -O1 test_stats_host.c stats.c`）。
3. **CFLAGS 缺 `-Wall -Wextra`**。上面多个潜伏问题（移位 UB、未用宏）本可被警告提前暴露。
4. **向量表不严谨**（`startup.c:23`）：`const void* vectors[]` 混用数据指针与函数指针，且无对齐属性。当前从 0x0 启动恰好能跑；一旦要重定位向量表（SCB_VTOR，bootloader 场景），类型不对 + 对齐不足（Cortex-M4 要求表按 ≥128 字节对齐）就是坑。应改为 `typedef void (*isr_handler)(void)` + `__attribute__((aligned(128)))`。
5. **向量表保留槽位填 0 而非 `Default_Handler`**（`startup.c:28-32`）。目前只使能 SysTick 中断所以不炸；一旦使能其他中断，未实现的中断会跳地址 0 → 极难查的 fault。应统一用弱符号 `Default_Handler` 填满。
6. **FPU 全程未用**（`-mfloat-abi=soft`）。Cortex-M4 有硬件 FPU，软浮点白扔约一个数量级的浮点性能。当前固件纯整数无影响，但作为"硬件模块"应预留开关：`-mfpu=fpv4-sp-d16 -mfloat-abi=hard` + Reset_Handler 里设 CPACR。
7. **无看门狗**。`uart_putc` 等 TXE 是死循环轮询，外设一旦挂死 + 无 IWDG = 永久变砖。
8. **BRR=139 硬编码**（`uart.c:14`）。应按 `HSI_HZ/115200` 宏计算，换波特率/时钟源时不靠心算。

## 三、P2 简洁性（死代码 / 冗余）

- `sys.c:11` `(void)RCC_CR; (void)RCC_CFGR;` —— 考古残留死代码，删。
- `hw.h` 未用宏：`GPIOA_ODR`、`GPIOD_ODR`、`USART_SR_TC`（`SYSTICK_VAL` 有用在 init 清计数器，保留）。
- `shell.c:190` `filter_reset(); filter_set_window(8);` —— `filter_reset` 内部即 `filter_init`（window=8），后半句冗余。
- `filter_init` 与 `filter_reset` 同体，语义重叠，应注释区分"首次初始化"与"运行中复位"。
- `runtime.c` 6 个 `*4/*8` 薄包装可留（`--gc-sections` 会清未用的），但应注释说明"哪些是实测需要的、为什么需要 `-fno-builtin`"。
- `renode_run.py:59` 固定 `sleep(4)` 等 Renode 冷启动（dotnet JIT 时间不稳）；单次 `recv` 可能丢 TCP 分片响应；路径全硬编码。应改为：轮询端口直到可连 + 循环读到空闲 + `--renode/--out` 参数化。

## 四、为什么没能"第一回就写对"（根因，按耗时排序）

1. **Renode 平台坑**：`platforms/cpus/stm32f4.repl` 只映射 flash 到 0x08000000，**无 0x0 boot 别名**（真实 STM32 靠 BOOT0 引脚 + 别名机制）。Cortex-M 复位固定从 0x0 取 SP/PC → fault（PC 落 0xEFFFFFFE）。官方示例走 board repl 看不出来，等于每个使用者重新考古一遍。
2. **编译器组合坑**：clang `-O2` 把 `while(n--)*d++=0` builtin 成 `memset` 调用，而 `-nostdlib` **不阻止 builtin 化**，该调用又解析回自己的 `__aeabi_memclr` → 无限递归爆栈。这个组合违反直觉（注意：项目自己的 `toolchain/build_builtins.sh` 已经在用 `-ffreestanding -fno-builtin`，说明知识在仓库里已有，只是没同步到 firmware 模板）。
3. **Renode 语义坑**：`LoadELF` 对该核既不设 SP 也不设 PC，且 CPU 进 fault lockup 后手动 `SetRegisterUlong` 救不回来。正确姿势是 `LoadBinary` + boot 别名，让 CPU 自己走复位流程（也正好符合"烧录"的真实语义）。
4. **无诊断**（见 P1-1）：HardFault 静默，把本可几秒钟的问题拖成十几轮。
5. **无测试分层 + 无脚手架**（见 P1-2 和方案 6）。

共性：四个坑全是"环境/工具链/流程"知识，没有一个是代码逻辑错误。**消除它们靠模板和文档，不靠更小心。**

## 五、改进方案

### A. firmware 侧（改 C，半天工作量）

1. P0 全部修掉（上表 8 项，每项都是几行）。
2. HardFault 诊断：`startup.c` 里打印 CFSR/HFSR/MMFAR/BFAR + 故障 SP；README 给出 `llvm-symbolizer`/`addr2line` 一条龙命令。
3. 向量表重做：`isr_handler` 类型 + `aligned(128)` + 全槽位弱符号 `Default_Handler`。
4. `CFLAGS` 加 `-Wall -Wextra` 并清零警告；BRR 改为 `HSI_HZ/BAUD` 宏；时钟/波特率/LED 引脚集中到 `board.h`。
5. host 单测入口：`build.sh` 加 `test` 子命令（x86 编译 filter/stats/crc32/sensor + 断言，**必须含环形缓冲回绕边界用例**），Renode 只做集成验证。
6. FPU/看门狗作为 `board.h` 里的可选开关预留；`peek/poke` 加地址区校验。
7. 删 P2 死代码，hw.h 瘦身。

### B. Scourge 源码侧（让硬件模块被"直接支持"，这是根治）

把本次验证过的 C 固件起点收编为语言级裸机后端，新板子不再从零考古：

1. **裸机目标通道**：`scourge build --target=armv7m-none-eabi --freestanding`（或 `--board=stm32f4-discovery`）。AOT 后端（`src/AOT/LangToLLVM.cpp` 一线）对裸机目标**强制** `-ffreestanding -fno-builtin`（与 `toolchain/build_builtins.sh` 已有做法对齐），无 crt、ld.lld 直链。
2. **内置 crt 模板**：startup/runtime/linker 三件套（即本次的 `startup.c` + `runtime.c` + `stm32f4.ld`，按 A 方案修订后固化为模板）+ 最小 `__aeabi_mem*` 集（复用 `build_builtins.sh` 的 compiler-rt 流程或直接内置 C 版）。
3. **板级一键产物**：`--board=stm32f4-discovery` 直接产出 elf + bin + Renode `.resc`/boot repl + harness（把 `stm32f4_boot.repl` 和 `renode_run.py` 收编进仓库），"烧录测试"变成 `scourge flash`/`scourge test --renode` 一条命令。
4. **std capability 裁剪**：bare-metal 下排除 stdio/dirent/malloc，提供 `sg_bare` API（putc/panic/bump-alloc），与现有 capability 域机制一致。
5. **文档进 book**：两个坑从编译器和链接器原理层面解释（builtin 化与符号解析的顺序、Cortex-M 复位取向量与存储映射的关系），而不是只记"要加哪个 flag"。
6. **可选**：给 Renode 上游提 issue/PR（`stm32f4.repl` 缺 boot alias），社区受益，我们也不再需要自定义 boot repl。

## 六、验收标准（改进后）

- [ ] `bash build.sh`（或 `scourge build --board=...`）零警告产出 elf/bin
- [ ] `bash build.sh test` 宿主单测全过（含回绕边界）
- [ ] Renode 里 `selftest` 5/5 + 新增 fault 诊断用例（故意触发一次 HardFault，能看到 CFSR/BFAR 输出）
- [ ] 恶意输入（`run 999999999` / `sample 99999999999` / `peek 00000000` / `poke` 向量表）全部被安全拒绝，固件不卡不死
