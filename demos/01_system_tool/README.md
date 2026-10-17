# 01 · 代码库度量 CLI（系统工具方向）

扫描目录下 `.sg/.md/.txt` 文件，逐文件统计 行/词/字节/空行/注释行/代码行，
按行数降序输出 TopN 对齐表格、摘要统计、按扩展名聚合与行数直方图，
最后以三引号 f-string 模板落盘 Markdown 报告。

## 构建与运行

```powershell
# 方式一：一键构建 + 运行（推荐；可透传程序参数）
sg  # 编译器需先在仓库根构建
powershell -File build.ps1 --top=3

# 方式二：手动（在仓库根执行，产物落 out/、中间产物落 .sgbuild/）
sg build demos\01_system_tool\main.sg
demos\01_system_tool\out\main.exe --path=. --top=5

# VM 直跑（字节码缓存落 .sgbuild/）
sg run demos\01_system_tool\main.sg
```

## 模块结构（多文件 import 协作）

| 文件 | 职责 | 关键能力 |
|---|---|---|
| `main.sg` | 装配入口：CLI 解析（`args()`）+ 三层数据流编排 | `capability io, env` |
| `scan.sg` | 数据采集：目录扫描、单文件 7 元组度量 | `capability io`、`list_dir/read_text`、try/except |
| `stats.sg` | 统计聚合：扩展名两张 Dict 表 + 行数直方图 | `Dict.get` 默认值累加 |
| `report.sg` | 呈现：降序对齐表格 + Markdown 报告模板 | 定宽键降序、三引号 f-string |

## 实测输出（VM/AOT 双模逐字节一致）

```
== top 5 files by lines (.) ==
file                     lines  words   bytes  code
main.sg                     57    198    2158    41
report.sg                   52    219    2062    39
stats.sg                    45    178    1805    29
scan.sg                     42    185    1719    31
== summary ==
files 4  total_lines 196  total_words 780  total_bytes 7744
mean 49.0  median 48.5  std 5.87  min 42  max 57
== by extension ==
sg       files 4  lines 196  avg 49.0
== histogram ==   0-9 0   10-49 2   50-99 2   100+ 0
report -> ./metrics-report.md
```

## 语言要点

- **跨模块调用带显式类型注解**（`def measure_dir(path: str) -> List[Tuple[...]]`）；
  模块内辅助函数走无注解推断。
- **定宽键降序**：`f"{lines:06d}|{name}"` 建排序键 → `sorted` → `reversed` →
  `find("|")` 切回（sorted 无 key 参数的标准替代）。
- **元组不能装 Dict**（E1007）：聚合表拆成两张同键序 Dict 函数配对消费。
- 报告写到**运行时 CWD**（约定从 demo 目录运行，见 build.ps1）。
