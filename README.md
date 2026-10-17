<div align="center">

<img src="scourge.png" alt="Scourge" width="140"/>

# Scourge

**为 AI 代理时代设计的确定性验证型语言**

一份源码，两种执行：VM 直跑与 AOT 原生编译，行为逐字节一致——从云端的 AI 代理，到口袋里的单片机。

[![Version](https://img.shields.io/badge/version-0.1.0-2563eb)](#版本)
[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C)](CMakeLists.txt)
[![MLIR](https://img.shields.io/badge/codegen-LLVM%20%2B%20MLIR-8A2BE2)](#架构)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-64748b)](#30-秒上手)
[![MCP](https://img.shields.io/badge/MCP-native-16a34a)](#mcp-为-ai-代理而生)
[![Embedded](https://img.shields.io/badge/bare_metal-STM32F4-4a7c59)](#仓库结构)

**简体中文** ｜ [English](README_EN.md)

</div>

---

## 这是一门什么语言

Scourge 把"确定性"作为第一原则：同一段 `.sg` 源码，经 `sg run` 在字节码虚拟机上执行，与经 `sg build` 降级为 MLIR → LLVM IR → 原生可执行文件，**运行结果逐字节一致，panic 消息逐字相同**。所有程序在编译期完成类型、形状与契约的全量验证，把运行时事故拦在执行之前。

它同时为机器可消费而设计：静态检查结果可输出结构化 JSON 诊断，内置 MCP（Model Context Protocol）服务器，AI 代理可以像使用人类工具一样使用整套工具链。同一套确定性原则也延伸到裸机世界：仓库里带着 STM32F4 裸机运行时、链接脚本与 Renode 仿真配置——AOT 产物可以不依赖任何操作系统，直接落在 Cortex-M 上。

| 特性 | 说明 |
|---|---|
| 双模一致 | `run`（VM）与 `build`（AOT）行为逐字节一致，panic 逐字相同 |
| 编译期全量验证 | 类型、张量形状、契约（docstring 即契约载体）、错误消费强制检查 |
| 确定性错误模型 | 错误即值（`Result[T, E]`）+ 可恢复 panic（`try/except`）双轨 |
| 张量一等公民 | `Tensor[float, (2, 3)]` 形状进类型系统；张量运算降级到 linalg |
| 进程内 MLIR | 无外部 `mlir-opt` / `mlir-translate` 依赖，一条命令产出原生可执行 |
| 裸机嵌入式 | STM32F4 裸机运行时 + Renode 仿真；AOT 产物无 OS 依赖落地 Cortex-M |
| 中文诊断 | 面向中文开发者的编译期诊断与运行时 panic 消息 |
| MCP 原生 | 独立 `sgmcp` 入口，stdio JSON-RPC，AI 代理开箱即用 |
| 自举包管理器 | `sget` 由 Scourge 自身 AOT 编译产出——构建成功即全链冒烟 |

## 三件套

构建产物为三个可执行文件（`build-rel/`）：

| 可执行 | 定位 | 典型用法 |
|---|---|---|
| `sg` | 主编译器 | `sg run x.sg` 直跑 ｜ `sg build x.sg` 产出独立可执行 ｜ `sg check` 静态验证 ｜ `sg repl` 交互会话 |
| `sgmcp` | MCP 独立入口 | stdio JSON-RPC 服务器，与 `sg mcp` 共用实现、零分叉 |
| `sget` | 包管理器 | `sget list` 查看本地包；由 `sg build` 自举而来 |

构建产物纪律：`sg build` 产物落 `<源目录>/out/`，中间产物与缓存落 `<源目录>/.sgbuild/`——源码树永不污染。

## 安装包

Windows 提供 Inno Setup 安装包（`Scourge-0.1.0-win-x64-x86-setup.exe` 与 `Scourge-0.1.0-win-arm64-setup.exe`，后者为 x64 二进制经 Windows on ARM 兼容层运行），安装时可勾选**加入系统 PATH**；安装后 `sg run` / `sg check` / `sg repl` / `sgmcp` / `sget` 零配置可用——`std/` 标准库随编译器安装布局解析，无需任何环境变量。Linux 提供 `scourge-0.1.0-linux-x86_64.tar.gz` 源码发行包，解压后 `scripts/bootstrap.sh` 一键构建。

## 30 秒上手

依赖：`cmake ≥ 3.28`、`ninja`、`clang`；MLIR 库取自发行版 `llvm-dev`（Linux 自动探测）或 Release 预编译库包（自动下载）。

**Linux / macOS**

```bash
git clone https://github.com/YuanBaoSMadLab/scourge.git scourge && cd scourge
scripts/bootstrap.sh                     # 探测工具链 → 取 MLIR → 构建 → 冒烟
build-rel/sg run examples/hello.sg
```

**Windows（PowerShell）**

```powershell
git clone https://github.com/YuanBaoSMadLab/scourge.git scourge; cd scourge
scripts\bootstrap.ps1                    # 探测工具链 → 取 MLIR → 构建 → 冒烟
build-rel\sg.exe run examples\hello.sg
```

引导脚本完成后即可使用三件套；`sget` 的自举成功本身就是 AOT 全链路的端到端冒烟。

## 语言一览

递归、错误即值、panic 恢复——摘自 [examples/hello.sg](examples/hello.sg)：

```python
def safeDiv(a: int, b: int) -> Result[int, str]:
    """错误即值：除零显式进 Result，is_ok 强制消费。"""
    if b == 0:
        return err("除数为零")
    return ok(a // b)

def panicGuard() -> str:
    """panic 恢复：try/except 捕获运行时 panic（整除零）。"""
    x: int = 0
    try:
        x = boom(10, 0)
    except:
        x = -2
    return "panic 已捕获"
```

跨模块自动微分训练循环——摘自 [demos/04_ml_regression](demos/04_ml_regression/train.sg)，标准库 `std.nn` / `std.autodiff` 驱动：

```python
import std.autodiff as ad
import std.nn as nn

def run_epoch(W1: List[float], b1: List[float], ..., lr: float, t: int) -> Tuple[int, float]:
    T = ad.tape()
    ids1 = nn.params(T, W1, b1, 1, nhid)
    hn = nn.linear_nodes(T, ids1, xn, 1, nhid)
    loss = nn.mse(T, on, yt, 1)
    g = nn.gather_grads(T, loss, ids, npar)
    nn.adam(W1, g[0:p1], mW1, vW1, lr, t, p1)
    return (t, loss_sum / float(ntr))
```

五个端到端应用见 [demos/](demos/README.md)：系统巡检工具、零依赖 Web 服务、数据清洗管道、MLP 回归（GUI 实时训练）、MLP 分类。

## 性能：实测数据说话

以下数字全部来自单机实测（Windows 11 x64 · 16 核 · RTX 5060 Ti），与 CPython 3.14 / numpy 2.5.1 同机对拍、数值逐位交叉验证；方法学与全部探针见 [docs/15-性能实测与客观对比.md](docs/15-性能实测与客观对比.md)——**慢的数字原样收录**。

**标量代码：AOT vs CPython 3.14**

| 负载 | Scourge AOT | CPython 3.14 | 加速比 |
|---|---|---|---|
| fib(30) 递归 | **6.78 ms** | 159 ms | **≈23×** |
| 1000 万次循环累加 | **11.87 ms** | 882 ms | **≈74×** |

**矩阵乘 128×128（CPU）**

| 实现 | 耗时 | 相对 Scourge AOT |
|---|---|---|
| **Scourge AOT（build）** | **0.32–0.35 ms** | 基准 |
| numpy 2.5.1（OpenBLAS） | 1.61 ms | ≈5× 慢 |
| Scourge VM（run） | 1.23–1.38 ms | ≈4.1× 慢 |
| 纯 Python 三重循环 | 115.2 ms | **≈340× 慢** |

**双模一致不是口号**：同一份源码，`run` 与 `build` 结果逐字节一致（含 panic 消息），GPU 路径数值与 CPU 逐位一致——性能可以放心换挡：**调试用 VM，发布用 AOT**。

## 诚实的不足

- **大尺寸矩阵乘落后于 numpy**：单线程朴素实现对阵 OpenBLAS 多线程分块，1024² 约 18× 差距（7.72 ms vs 139 ms）——多线程分块是下一步优化方向；
- **VM 标量弱于 CPython 1.4–5.7×**：直译路线的解释器，调试够用，不是性能上限（张量运算仍走原生运行时）；
- **GPU 仅极大矩阵有收益**：≤512² 因驱动固定开销反而更慢，≥1024² 约 1.15× 微弱领先；
- **生态从零起步**：没有第三方张量库与包仓库，`std.nn` / `std.autodiff` / `sget` 是全部家底；
- **语义仍在收紧**：`@` 声明形态与文档存在差异等已知缺口见 [docs/22-已知缺口与避坑.md](docs/22-已知缺口与避坑.md)；
- **AOT 产物构建依赖外部工具链**：需系统 clang 与 MSVC/SDK（Windows）；`sg run`（VM）才是零依赖开箱即用的路径。

## 架构

```mermaid
flowchart LR
    A[".sg 源码"] --> B["前端<br/>Lexer → Parser → Sema<br/>类型 · 形状 · 契约 · 单态化"]
    B --> C["SG-BC 字节码"]
    C --> D["栈式 VM<br/>sg run"]
    C --> E["进程内 MLIR<br/>Lang 方言 → linalg / scf / affine"]
    E --> F["LLVM IR → 原生可执行<br/>sg build"]
    D -. 行为逐字节一致 .-> F
    B --> G[".sgi 接口摘要<br/>JSON 诊断 · MCP"]
```

- **src/Sema** — 类型检查、形状统一、隐式单态化、契约检查
- **src/BC** — 字节码编译器 + 栈式虚拟机（含网络 / 系统 / JSON 内建）
- **src/AOT** — Lang 自定义方言 → linalg/affine/scf → LLVM IR，全程进程内
- **src/Tool** — `sg` / `sgmcp` / `sget` 三入口驱动

## 文档

完整语言手册 26 章，位于 [docs/](docs/)：

| 分册 | 章节 |
|---|---|
| 工具链 | [01 命令总览与工具链](docs/01-命令总览与工具链.md) · [14 AOT 构建与双模一致性](docs/14-AOT构建与双模一致性.md) · [25 测试套件索引](docs/25-测试套件索引.md) |
| 语言核心 | [02 类型系统](docs/02-类型系统.md) · [04 运算符与表达式](docs/04-运算符与表达式.md) · [05 控制流](docs/05-控制流.md) · [06 函数](docs/06-函数.md) · [08 变量与赋值](docs/08-变量与赋值.md) · [10 模块系统](docs/10-模块系统.md) |
| 错误与契约 | [03 错误处理](docs/03-错误处理.md) · [07 契约式编程](docs/07-契约式编程.md) · [21 defer 确定性清理](docs/21-defer确定性清理.md) · [24 错误码速查](docs/24-错误码速查.md) |
| 张量与数值 | [11 张量构造与形状](docs/11-张量构造与形状.md) · [12 张量运算与归约](docs/12-张量运算与归约.md) |
| 系统能力 | [13 并发基座](docs/13-并发基座.md) · [17 系统编程内建](docs/17-系统编程内建.md) · [18 文件 IO 与网络](docs/18-文件IO与网络.md) · [19 外部函数接口 FFI](docs/19-外部函数接口FFI.md) · [20 JSON 哈希与字符串处理](docs/20-JSON哈希与字符串处理.md) |
| 进阶与参考 | [09 装饰器与声明边界](docs/09-装饰器与声明边界.md) · [15 性能实测](docs/15-性能实测与客观对比.md) · [16 能力模型](docs/16-能力模型.md) · [22 已知缺口与避坑](docs/22-已知缺口与避坑.md) · [23 内建函数速查](docs/23-内建函数速查.md) · [26 与 Python 语法对照](docs/26-与Python语法对照.md) |

## MCP：为 AI 代理而生

`sgmcp` 以 stdio JSON-RPC 2.0 提供 MCP 服务器，任何 MCP 客户端即可接入：

```json
{
  "mcpServers": {
    "scourge": {
      "command": "/path/to/build-rel/sgmcp",
      "args": []
    }
  }
}
```

代理可以调用 `check` / `run` / `build` 等工具并获得结构化 JSON 诊断——编译器输出的每一条错误都可被机器解析、定位、修复。

## 仓库结构

```
include/scourge   公开头文件（Common / Syntax / Sema / BC / AOT / Tool）
src/              编译器实现（Sema · BC · AOT · Tool 四域）
std/              标准库（std.nn / std.autodiff …）
examples/         语言示例（hello · blog · bindgen · sget 包工作区 · 裸机）
demos/            五个端到端应用（各为多模块完整工程）
docs/             语言手册（26 章）
verify/           双模回归与探针（40+ 脚本，VM/AOT 对拍）
scripts/          bootstrap 双平台一键构建 · vsenv 链接环境
firmware/         裸机嵌入式目标（STM32F4：运行时 + Renode 仿真）
```

## 愿景：AI 时代的红利，属于确定性

两件事正在同时发生：**AI 代理正在成为代码的主要作者**；而 **AI 的下一站是物理世界**——机器人、智能家居、工厂与田野里的每一块芯片。前者需要语言本身成为验证器：代理写的每一行代码都过同一把尺子，结构化诊断让错误可被机器直接定位与修复。后者需要确定性、可控的体积与裸机可用性：设备端的每一字节行为都不该靠运气。

主流语言很少同时认真服务这两个世界。Scourge 想做这个交叉地带的答案：**对 AI，它是代理可直接驱动的验证型语言（MCP 原生）；对嵌入式，它是能落进 Cortex-M 的确定性 AOT**。我们身处的，正是"AI 能力红利"与"万物互联"两条曲线的交汇点——把工具提前造好，等潮水来。

0.1.0 只是起点。我们不宣称替代任何语言，也清楚自己生态尚小、语义仍在收紧（见"诚实的不足"）；但方向是对的，路是可复现的。项目完全开放共同研发——issue、PR、手册修订、示例贡献都欢迎，无论你是语言爱好者、AI 工具链工程师，还是想让高级语言跑进单片机的嵌入式开发者。

如果这个项目对你有价值，欢迎在爱发电支持持续开发：**[https://afdian.com/a/YuanBaoSMadLab](https://afdian.com/a/YuanBaoSMadLab)**

## 版本

**0.1.0** —— 首个公开版本。三件套交付形态、双平台一键构建、五 demos 端到端工程、26 章语言手册自此起稳定迭代。

## License

Copyright © 2026 Scourge Project. All rights reserved. See [LICENSE.txt](LICENSE.txt).
