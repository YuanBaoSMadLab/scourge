# 外部函数接口 FFI

## 19.1 基本形态（标量域）

先声明 `capability ffi`，再用 `@extern("dll名")` 声明外部函数，函数体只允许一条 `pass`：

```scourge
capability ffi

@extern("kernel32.dll")
def lstrlenA(s: str) -> int:        # 参数与返回类型必须显式注解
    pass

@extern("msvcrt.dll")
def fabs(x: float) -> float:
    pass

def main():
    print(lstrlenA("hello"))        # 5
    print(fabs(-3.5))               # 3.5
    return 0
```

约束（Sema，VM/AOT 共用）：

- 返回类型必须显式注解；参数与返回仅限标量域：`int` / `float` / `bool` / `str` / `none`。
- 不支持缺省参数；函数体仅允许一条 `pass`。
- **本语言函数一律定参**；只有 `@extern` 声明允许末位 `...`。

> 避坑：`@extern` 声明的 Scourge 函数名 = 要查的 C 符号名。想调 `msvcrt.dll` 的 `pow`，函数就要命名为 `pow`，写成 `def mypow(...)` 会报"符号未找到"。

## 19.2 变参 `...`（O 专项）

变参只允许出现在 `@extern` 声明上，且必须是末位：

```scourge
capability ffi

@extern("msvcrt.dll")
def printf(fmt: str, ...) -> int:
    pass

@extern("msvcrt.dll")
def fflush(stream: int) -> int:
    pass

def main():
    printf("hi %d %.2f\n", 42, 3.5)    # 输出：hi 42 3.50
    fflush(0)                           # 刷缓冲，保证上面的输出落地
    return 0
```

MS x64 双 ABI 规则（VM/AOT 双模一致实现）：

- **定参被调方**：浮点走 xmm、整型走 GP，两流独立计数。
- **变参被调方**：全部实参按**参数位置**单流占槽；浮点位模式同时放入对应位置的 GP 影子槽（与 clang/msvcrt 的 `printf` 实现一致）。
- 实参数下限 = 命名参数个数，**上限 20**（与 VM 变参调用桩栈槽容量对齐，越界编译期 E2001）。

## 19.3 地址透传 `data_ptr`（O 专项）

`data_ptr(x)` 返回宿主地址 `int`，可传给期望指针的 C 函数：

```scourge
capability ffi

@extern("msvcrt.dll")
def strlen(p: int) -> int:
    pass

def main():
    s = "hello"
    print(strlen(data_ptr(s)))          # 5：str 地址透传，C 侧直接读 Scourge 内存
    return 0
```

契约：

- 接受 `Tensor[int]` / `Tensor[float]` / `str`；`List` 等编译期拒绝 E2001。
- `str` 地址指向进程期稳定副本（只读透传契约）；张量返回宿主侧连续缓冲地址。
- 与 `@extern` 同受 `ffi` 能力门禁。

> 注意：`data_ptr` 返回的是字节地址，`strlen` 按 C 字节串解释。例如 `data_ptr([1.0, 2.0, 3.0])` 中 `double 1.0` 的低字节是 `0x00`，`strlen` 会立即得到 0——这符合 C 语义，不是缺陷。要做字节偏移运算请用普通 `int` 算术（无类型化指针 / 解引用语法）。

## 19.4 实践约定：AOT 的输出缓冲

AOT 可执行文件直接链接 C 运行时，`printf` 输出进入其自有缓冲，与 VM 管道不同源。跨模式要可复现输出，请在 `printf` 后配同源的 `fflush(0)`（同样经 `@extern` 声明）。DLL 不存在 / 符号缺失 / 调用异常都会 panic，且消息双模逐字一致，可被 `try/except` 捕获。

## 19.5 确定性红线与残量

- `@extern` / `data_ptr` 在 VM（`run`）与 AOT（`build`）下行为逐字节一致（含 panic 消息）。
- **尚未覆盖**（P1/P2）：指针算术 / 结构体布局 / 回调函数指针；变参上限 20 为语言级常量，改造调用桩后可放宽。
