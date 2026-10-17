# defer 确定性清理

## 21.1 基本形态

`defer:` 注册一个**函数级清理块**，在函数返回（显式 `return` 或函数尾隐式返回）前，按 **LIFO**（后注册先执行）执行该 return 之前已声明的全部 defer 块：

```scourge
def main():
    defer:
        print("cleanup-1")
    defer:
        print("cleanup-2")
    print("body")
    return 7
```

输出顺序：

```
body
cleanup-2
cleanup-1
```

返回 7。注意 `cleanup-2` 后于 `cleanup-1` 注册，却先于它执行——这就是 LIFO。

## 21.2 语义要点

- **仅函数体顶层**：不能写在 `if` / `while` / `for` / `try` 体内（E1007）。展开点静态可枚举，是 VM/AOT 双模一致的根基。
- **返回值先求值**：`return x` 先求值 `x`，defer 块内对局部变量的修改**不影响本次返回值**。
- **defer 块内禁止** `return` / `break` / `continue` / 嵌套 `defer`（E1007）；允许任意其他语句（含 `try/except`）。
- **panic 不触发 defer**：panic 传播不经过 return 点；panic 路径的资源清理请用 `try/except`。这保证每个 return 点执行的清理序列完全静态确定。

## 21.3 为什么 panic 不触发 defer

确定性是 Scourge 的第一红线。如果 panic 也触发 defer，那么"哪些清理会跑"就取决于 panic 发生的具体位置，清理序列不再是编译期可枚举的静态结构——这会破坏 VM/AOT 双模一致。因此规则很干脆：**defer 只在正常 return 路径执行，panic 走 `try/except`**。

## 21.4 双模一致

`defer` 在 VM（`run`）与 AOT（`build`）下行为逐字节一致：清理顺序、返回值、panic 不触发，三路同源。

## 21.5 避坑

- 不要在 `defer` 里 `return`——编译期就拒绝，不是运行期陷阱。
- 需要"无论成功失败都清理"，用 `try/except` 包住可能 panic 的部分，把清理放进 `except` 之后的正常路径或另一个 `defer` 的不 panic 分支。
- defer 块里可以安全地打印、写文件（需对应能力）、调用纯函数。
