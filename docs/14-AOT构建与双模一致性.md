# AOT 构建、双模一致性与 GPU

## 14.1 双模是什么

同一个 `.sg` 文件，两种执行方式：

| 模式 | 命令 | 管线 | 适用 |
|---|---|---|---|
| 字节码 VM | `scg run` | 检查 → SG-BC 字节码 → 虚拟机 | 调试、保存即跑、REPL |
| AOT 原生 | `scg build` | 检查 → MLIR → LLVM IR → clang 链接 | 发布、性能、分发 |

**双模一致（在 AOT 覆盖集内）**：同一程序的 stdout、panic 消息（stderr）、退出码逐字节一致。这不是"尽量一致"而是被测试固化的纪律——`examples\` 全部示例（含 hello.sg）都经过双模逐字节比对；本书附录 C 的 12 个 AOT 可构建测试（`t03`/`t06`/`t07`/`t08`/`t13`/`t16`/`t19` 等）输出均与 VM 字节级一致。

> **边界（重要）**：双模一致仅对 AOT 后端**已实现**的特性成立。当前有 4 类写法在 `build` 下被 `E1007` 显式拒绝（张量全局变量、负指数幂、`print(Result)`、模块顶层 `async def`，见 14.4）。这些不是崩溃、不是回归，而是 AOT 后端尚未覆盖——请按需用 `run` 或等待实现。

## 14.2 AOT 覆盖集

build 时，张量 op 分两路：

**降级为结构化循环/linalg（编译期内联）**：f64/int 全静态形状逐元素 `+ - * / // % **`（int `/` 产出 f64）、同 dtype 与混合 dtype 的标量双向广播、`sum`、二维 `transpose`、`@` 矩阵乘（linalg 结构化 op）。

**走 `@sg_*` 运行时回退**：动态形状张量、覆盖集外的 op、Result/List/try-except 的句柄世界（`lang.result_*` / `lang.list_*` 降级为 `@sg_*`）。try/except 用 CRT `setjmp/longjmp` 蹦床实现，与 VM 的 `catches_` 同构；含 `setjmp` 的函数中槽位读写一律 volatile，保住"槽值跨 panic 保持"语义。

**FFI 调用（ O 节起进入覆盖集）**：`@extern` 声明（标量域 + 末位 `...` 变参，至多 20 实参）在 AOT 下发射 LLVM 真变参间接调用（`var_callee_type`）——变参被调方按**参数位置**读寄存器/影子槽（MS x64 变参 ABI），与 VM 手写调用桩同规则，`printf`/`memset`/`strlen` 实测双模输出逐字节一致（`verify\verify_o_wide.ps1`，含 0/20 实参双边界）。`data_ptr(x)` 地址透传（`Tensor[int]`/`Tensor[float]`/`str` → `int`）在 AOT 侧降级为 `@sg_data_ptr` 运行时调用。两者均需模块顶层 `capability ffi`（未声明 E5004）。

> **实测备注**：本书实测生成的 `.ll` 中，128×128 与 224×224 的 `@` 矩阵乘均出现 `@sg_tensor_matmul` 运行时调用（未见 linalg.matmul 残迹）——文档描述的"linalg 结构化 op"降级与产物之间存在差异。矩阵乘已采用行流式 i-k-j + `__restrict` + 4-wide 向量化内联，AOT 比 VM 快 4–23×（见 17.2.1）。产物是否仍含 `@sg_tensor_matmul` 调用，建议开发团队复核。

## 14.3 验证双模一致：diff 三件套

```powershell
scg run t16_dual_mode.sg > run.txt
scg build t16_dual_mode.sg -o t16.exe
.\t16.exe > build.txt
diff run.txt build.txt        # 期望无差异
```

> **实测记录**（`t16_dual_mode.sg`，run 与 build 输出完全一致）：
> ```
> 7
> 3.5
> 3
> 1024
> abcd
> Tensor(2, 2) [[7.0, 10.0], [15.0, 22.0]]
> Tensor(2, 2) [[2.0, 4.0], [6.0, 8.0]]
> 10.0
> 9
> abc
> -1
> ```

该测试覆盖标量路径（算术/真除/地板除/幂/字符串拼接）、张量路径（`@`、广播、`sum`）、Result/List/try-except 路径（含除零 panic 捕获）——三类路径双模逐字节一致。

## 14.4 AOT 当前不支持的构造（显式报 E1007）

以下构造进入 `build` 时显式报 `E1007`，与"张量静默降级禁止"同一纪律——不会退化为错误代码，这些程序请用 `scg run`。**当前仍拦 4 类**：

| 构造 | 触发示例 | E1007 消息片段 | 当前状态 |
|---|---|---|---|
| 张量全局变量 | 模块顶层 `A = [[...]]` | AOT 暂不支持：全局变量 'A' 的类型 | **仍拦**（复测确认） |
| Result 整体打印 | `print(r)` | AOT 暂不支持：print 该参数类型 | **仍拦**（复测确认） |
| 负整数字面量指数 | `2 ** -1` | AOT 暂不支持：整数幂需要非负整数字面量指数 | **仍拦**（复测确认） |
| str 上的 `in` | `"a" in words` | AOT 暂不支持：'in' 仅支持 str 右操作数 | **解锁**（双模一致） |
| Result/List 整体打印中的 List | `print(xs)`（List[int]/List[str]） | AOT 暂不支持：print 该参数类型 | **解锁**（仅 Result 仍拦） |
| 读取 int 列表下标 | `xs[i]`（xs: List[int]） | AOT 暂不支持：该元素类型的列表下标读取 | **解锁**（双模一致） |
| `async def` | `async def f: ...`（模块顶层） | AOT 暂不支持：async def | **顶层仍拦**；函数内嵌套形态  可 build（同步语义） |

> 上表以 `scg` 双模实测为准（探针见附录 C）。`Dict`/`Option` 本就无运行时，不在上表之列。

## 14.5 坑一：张量全局变量

```python
A = [[1.0, 2.0], [3.0, 4.0]]     # 模块顶层张量
def main():
    print(A @ A)
    return 0
```

`run` 正常，`build` 报 E1007「AOT 后端暂不支持：全局变量 'A' 的类型」。**绕行：把张量搬进函数局部**。本书 `t17_bench.sg`、`t18_bench_*.sg` 都是局部矩阵写法。

> **诊断截断**：超长矩阵行的诊断会截断为约 200 字节 + 截断标记 + 插入符对准，不会刷屏。

## 14.6 大矩阵字面量的构建成本

大矩阵字面量的常量数据被编译进产物的全局常量区（如 `@sg_tc0 = internal constant [1048576 x double]`），不占用运行时栈。因此 256²/512²/1024² 矩阵全部可正常构建与运行；代价是构建时间随字面量体积线性增长（1024² 约 9s）。需要频繁改数据做基准时，建议用固定公式在程序内生成（本书第 17 章的新基准即此写法）。

## 14.7 GPU 加速

官方文档（04-工具链参考 §11）描述的 `build --gpu` 路径已验证可用。本书在 RTX 5060 Ti 16GB 上实测：

```powershell
scg build t18_bench_512.sg -o t18_cpu512.exe        # CPU 路径
scg build --gpu t18_bench_512.sg -o t18_gpu512.exe   # GPU 路径
```

**实测 crossover（同一公式、同机、数值逐位一致；CPU 列为当前 AOT 实测）：**

| 尺寸 | CPU AOT | GPU AOT | 对比 |
|---|---|---|---|
| 224×224 | ~6 ms | ~113 ms | GPU 更慢 |
| 512×512 | ~21 ms | ~88 ms | GPU 慢约 4× |
| 1024×1024 | ~139 ms | ~121 ms | GPU 约 1.15× 微弱领先 |

读法：

- **GPU 有固定启动开销**（驱动惰性加载、kernel 调度，~100ms 级）：CPU AOT 在矩阵乘上已极快（行流式 i-k-j + 向量化，见 17.2.1），小矩阵 GPU 反而显著更慢。这是 CUDA 程序的普遍规律，不是 Scourge 的缺陷。
- **只有 ≥1024² 且追求极限吞吐时 GPU 才有微弱优势**；日常场景用 CPU AOT 即可（详见 17.3）。
- **回退机制**：`SCOURGE_NO_GPU=1` 永久回退 CPU、任意失败自动回退，已验证生效（GPU 二进制在 `SCOURGE_NO_GPU=1` 下回落到 CPU 路径，数值不变）。

> **与官方文档的核对**：官方文档给出 1024² GPU 299ms vs CPU 4618ms（15.4×）。本书实测 GPU ~121ms；CPU 列随矩阵乘优化大幅下降后，官方的加速比数字不再成立，以本书 17.3 节实测为准。
