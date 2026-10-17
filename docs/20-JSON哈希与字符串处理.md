# JSON、哈希与字符串处理

## 20.1 JSON（M 专项，零能力）

`json_decode(str) -> Json` 把 JSON 文本解析成动态 `Json` 值，取值用只读成员方法（VM/AOT 双模一致）：

```scourge
def main():
    j = json_decode("{\"a\": 1, \"b\": [true, 2.5]}")
    print(j.get("a").as_int())        # 1
    print(j.get("b").at(1).as_float())# 2.5
    print(j.get("b").len())           # 2
    print(j.dump())                   # {"a":1,"b":[true,2.5]}
    return 0
```

`Json` 值七形态与取值面：

| 方法 | 返回 | 说明 |
|---|---|---|
| `is_null` `is_bool` `is_int` `is_float` `is_str` `is_list` `is_obj` | `bool` | 类型判别 |
| `as_int` `as_float` `as_bool` `as_str` | 标量 | 类型不符则 panic（消息逐字一致） |
| `at(i)` | `Json` | 数组按 `int` 下标（越界 panic） |
| `get(k)` | `Json` | 对象按 `str` 键（**缺键返回 null，不 panic**） |
| `len` | `int` | 数组 / 对象长度（标量 panic） |
| `keys` | `List[str]` | 对象键列表（非对象 panic） |
| `dump` | `str` | 重编码为紧凑 JSON 文本 |

> `Json` 是只读视图：没有变更方法，对象按键的插入序输出，浮点用 `%.17g` 往返精度。

## 20.2 确定性哈希（零能力）

```scourge
def main():
    print(sha256("abc"))
    # ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    print(crc32("abc"))              # 891568578
    return 0
```

`sha256` / `crc32` 是纯计算，零能力声明即可用，输出确定，适合做数据指纹。

## 20.3 字符串方法（C-D 专项 P0-1，已落地）

字符串是不可变值，方法一律纯函数、不改原串。全套 12 个：

```scourge
def main():
    s = "  Hello World  "
    print(s.upper())                  # "  HELLO WORLD  "
    print(s.trim())                   # "Hello World"（去两端空白）
    print(s.split(" "))               # ["", "", "Hello", "World", "", ""]
    print("-".join(["a", "b", "c"]))  # "a-b-c"
    print(s.replace("World", "Scourge"))  # "  Hello Scourge  "
    print(s.find("World"))            # 8（字节下标；无则 -1）
    print(s.contains("Hello"))        # True
    return 0
```

| 方法 | 语义 |
|---|---|
| `upper` / `lower` | 仅 ASCII 字母映射，非 ASCII 字节原样返回 |
| `trim` / `trim_start` / `trim_end` | 去空白（空格 / `\t` / `\r` / `\n`），按字节两端 |
| `split(sep)` | 按子串分割；`sep` 为空串 panic；无匹配返回单元素列表 |
| `join(parts)` | 以 self 为连接符拼接 `List[str]` |
| `replace(old, new)` | 全部替换；`old` 为空串 panic |
| `find(sub)` | 首次字节下标；无则 -1（不 panic，便于探测） |
| `index(sub)` | 同 `find`，但无则 panic |
| `contains` / `startswith` / `endswith` | 子串判定 |

> **核心约定**：字符串按 UTF-8 **字节**处理。`len` / 下标 / `find` / `split` 一律按字节；`upper` / `lower` 仅 ASCII。需要按"字符"数预期时应自行按码点拆分——这是确定性语言的有意选择，务必写进代码注释避免误用。

## 20.4 集合算法：sorted / reversed

`sorted` 与 `reversed` 作用在 `List` 上，返回新列表（不改原列表）：

```scourge
def main():
    xs = [3, 1, 2]
    print(sorted(xs))                 # [1, 2, 3]
    print(reversed(xs))               # [2, 1, 3]
    print(sorted(["c", "a", "b"]))    # ["a", "b", "c"]
    return 0
```

- `List[int]` / `List[float]` / `List[bool]` / `List[str]` 均可构造与排序。
- 含 `NaN` 的浮点列表运行时 panic（`sorted 不支持含 NaN 的浮点元素`）——NaN 破坏全序，显式拒绝而非未定义行为。
- 空列表返回空列表，不 panic。

> 一维数值字面量 `[3, 1, 2]` 的类型是 `List[int]`；要得到张量需显式注解 `t: Tensor[int, (3,)] = [...]`。

## 20.5 归约：min / max

`min` / `max` 同时接受 `Tensor` 与数值 `List`，返回元素类型的标量：

```scourge
def main():
    print(min([3.0, 1.0, 2.0]))       # 1.0，一维浮点字面量是 List[float]
    print(max([[1.0, 2.0], [3.0, 4.0]]))  # 4.0，二维字面量是 Tensor
    xs = [3, 1, 2]
    print(min(xs), max(xs))           # 1 3，List[int] 同样支持
    return 0
```

支持 `int` / `float` 元素；空序列运行时 panic。不提供 `min(a, b, ...)` 多参标量形态。

## 20.6 避坑

- `sorted` / `reversed` 只接受 `List`。一维字面量 `[3, 1, 2]` 是 `List[int]`，可直接用；显式注解的张量（如 `Tensor[int, (3,)]`）会报 E2001，需先转成列表。
- `split` 的空 `sep` 和 `replace` 的空 `old` 都会 panic，属"语义有歧义一律 panic"纪律。
- 字符串按字节，中文等一个码点可能占多字节，`find` 返回的是字节下标。
