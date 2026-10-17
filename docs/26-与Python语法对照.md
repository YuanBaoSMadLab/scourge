# 附录 D 与 Python 的语法对照

> 面向有 Python 基础的读者：左列是 Python 习惯，右列是 Scourge 现实。标注 ⚠️ 的是"写法相似但语义/可用性不同"的高危项。

## D.1 基础语法

| Python | Scourge | 备注 |
|---|---|---|
| `x = 42` | `x = 42` | 一致 |
| `x: int = 42` | `x: int = 42` | 一致；注解可选，写了会触发运行期转换（`f: float = 3` → `3.0`） |
| `# 注释` | `# 注释` | 一致 |
| `//` 注释 | `//` 是**地板除** | ⚠️ |
| `def f(a, b=1):` | `def f(a, b = 1):` | 注解可选；默认参数生效 |
| 省略返回注解 | `def f(...):` 自动推断 | 递归/前向引用、混返、空容器除外 |
| `lambda` / 闭包 / 嵌套 def | 均无 | 函数只能模块顶层 |
| `True/False/None` | `True/False/None` | 一致 |
| `if __name__ == "__main__":` | 入口固定 `def main():` | 返回值即退出码；`-> int` 可省略 |

## D.2 数据结构

| Python | Scourge | 备注 |
|---|---|---|
| `[1, 2, 3]`（list） | `List[int]` | 一维数值字面量即列表；`List[float]` 由混入浮点推出 |
| `["a", "b"]`（list of str） | `List[str]` | 一致 |
| `[True, False]` | `List[bool]` | 一致 |
| `[[1.0, 2.0], [3.0, 4.0]]` | `Tensor[float, (2, 2)]` | ⚠️ 嵌套数值字面量是张量，非嵌套 list |
| `{"a": 1}` | `{"a": 1}` | Dict 字面量可用 |
| `{1, 2, 3}`（set） | `{1, 2, 3}` | Set 字面量可用 |
| `None` 哨兵 | `Result[T, E]` / `Option[T]` | 均完整可用（`some/none`） |
| `xs.append(v)` | `xs.append(v)` | 可用；元素域 int/float/bool/str/Tuple |
| `s.add(v)` / `v in s` | `s.add(v)` / `v in s` | 可用 |
| `len(xs)` | `len(xs)` | List/Dict/Set/str 可用；张量不支持 |
| `xs[0]` / `xs[-1]` | 一致 | 负下标合法 |
| `d["k"]` | 一致 | 读写都可用；`d.get(k, 缺省值)` 也可用 |

## D.3 控制流

| Python | Scourge | 备注 |
|---|---|---|
| `if/elif/else` | 一致 | 条件必须 bool |
| `while` | 一致 | |
| `for i in range(n)` | 一致 | |
| `for x in xs` | 一致 | List / range / Dict（键）/ Set / str 可遍历 |
| `for ch in "abc"` | 一致 | 按字节迭代 |
| `break/continue` | 一致 | 作用域检查 E2009 |
| `pass` | 一致 | |
| `1 < x < 10` | E1007 | ⚠️ 用 `and` |
| `a if c else b` | 一致 | 两分支类型须一致 |
| 循环变量遮蔽 | 允许 | 作用域限于循环体；内外层同名合法 |
| 语句间分号 | E1001 | ⚠️ 每行一条语句 |

## D.4 运算

| Python | Scourge | 备注 |
|---|---|---|
| `+ - * / // % **` | 一致 | 语义对齐（同号修正等） |
| `/` 真除 | 恒 float | 一致 |
| `@` 矩阵乘 | 一致 | 张量专用 |
| `and/or/not` | 一致 | |
| `in` | str 子串 / List 元素 / Dict 键 / Set 元素 | 张量不支持 |
| `f"{x}"` | 一致 | 简单插值可用；空插值 `f"{}"` 与格式说明符 `f"{x:spec}"` 在 AOT 报 E1007 |
| `"a" * 3` | 一致 | |
| `s.split(",")` / `s.upper()` | 一致 | 字符串方法可调用（注意带括号） |
| `s.strip()` | `s.trim()` | 名称不同 |
| `s[1:4]` | `s.slice(1, 4)` | ⚠️ 字符串切片按方法调用 |

字符串方法清单：`trim`、`upper`、`lower`、`split`、`join`、`replace`、`slice`、`find`、`index`、`contains`、`startswith`、`endswith`、`count`。

## D.5 错误处理

| Python | Scourge | 备注 |
|---|---|---|
| `try/except Exception as e:` | `try:` + 裸 `except:` | ⚠️ 无类型、无对象 |
| `raise ValueError(...)` | 无 raise；用 `err(...)` / `assert` / panic | 错误即值优先 |
| 任意精度 int | 64 位 int，**溢出 panic** | ⚠️ |
| `assert x, "msg"` | 一致 | 失败 panic 可捕获 |

## D.6 模块与工具

| Python | Scourge | 备注 |
|---|---|---|
| `import a` / `from a import b` / `import a as c` | 仅 `import a` | ⚠️ 另两种拒绝 |
| 包/点分路径 | 拒绝 | |
| `pip` / 包管理器 | 无 | 生态早期 |
| `python -c` / REPL | `sg repl`（同语义） | 一致体验 |
| 类型注解（可选） | 可选；跨模块被导入的函数须注解参数 | 返回类型可推断 |
| docstring | docstring + **契约**（requires/ensures） | ⚠️ 功能扩展 |
| 装饰器（任意） | 七个白名单 | ⚠️ |

## D.7 一句话迁移指南

> 把每个**嵌套**数值字面量念成"张量"，一维数值字面量则是 `List`；函数签名可以只写参数、返回类型交给推断（递归与被导入函数除外）；语句之间换行而不是分号；把链式比较换成 `and`；把 `s.strip()` 换成 `s.trim()`、`s[1:4]` 换成 `s.slice(1, 4)`——剩下的，Python 直觉基本够用。
