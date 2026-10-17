# 03 · 销售流水 ETL（数据管道方向）

读取外置数据 `data/sales.json`（16 条原始流水），清洗校验（类型守卫丢弃脏行），
按 类别/区域 两维聚合报表，Z-score 离群检测，内建统计（sum/mean/std/median），
最后落盘 Markdown 报告与 JSON 摘要（原样 json_encode 语义自建）。

## 构建与运行

```powershell
# 方式一：一键构建 + 运行（推荐；报告落在 demo 目录）
sg  # 编译器需先在仓库根构建
powershell -File build.ps1

# 方式二：手动（在仓库根执行，产物落 out/、中间产物落 .sgbuild/）
sg build demos\03_data_pipeline\main.sg
demos\03_data_pipeline\out\main.exe   # 需在 demo 目录下运行（data/ 相对路径）

# VM 直跑（字节码缓存落 .sgbuild/）
sg run demos\03_data_pipeline\main.sg
```

## 目录结构

```
03_data_pipeline/
├── main.sg          # 装配入口：编排 + 落盘
├── clean.sg         # 清洗层
├── aggregate.sg     # 聚合/报表层
├── build.ps1        # 一键构建运行脚本
├── README.md
├── data/
│   └── sales.json   # 外置数据（16 条流水，2 条脏数据）
├── out/             # 最终产物（main.exe）
└── .sgbuild/        # 中间产物/缓存（.mlir/.ll/.sgbc/obj）
```

## 模块结构（多文件 import 协作）

| 文件 | 职责 | 关键能力 |
|---|---|---|
| `main.sg` | 装配入口：读数据 → 清洗 → 双维报表 → 落盘 | `capability io`、write_text、三引号报告模板 |
| `clean.sg` | 清洗校验：json_decode → 逐条类型守卫（5 类脏行丢弃） | `capability io`、json_decode、List[Tuple] |
| `aggregate.sg` | 聚合报表：类别/区域双维 + 六表累加器 + Z-score 离群 | Dict.get 累加、定宽键降序、sqrt |
| `data/sales.json` | 外置数据：字符串价格、缺字段、负数量等脏数据 | 运行时读取，不硬编码 |

## 实测输出（VM/AOT 双模逐字节一致）

```
== by category (kept 14, dropped 2, revenue 342.5) ==
cat       n  revenue  qty    mean    std   price  share%
tool      3    145.0    4   48.33   5.31   36.25   42.34
book      4    103.0    5   25.75   3.77   20.60   30.07
food      5     54.0    9   10.80   2.66    6.00   15.77
toy       2     40.5    5   20.25   2.25    8.10   11.82
== by region ==
north   n  6  revenue   163.0  mean  27.17  min  12.5  max  55.0
south   n  4  revenue    93.5  mean  23.38  min   7.5  max  48.0
east    n  4  revenue    86.0  mean  21.50  min   9.0  max  42.0
== global ==
orders 14  revenue 342.5  mean 24.46  std 14.32  median 21.25  avg_order 24.46
== outliers (z > 1.5) ==
outlier tool/north amt 55.0 z 2.13
outlier tool/south amt 48.0 z 1.64
outlier_count 2
report -> sales-report.md / sales-summary.json
```

## 语言要点

- **跨模块可变 Dict 传参**：`accum` 九参数全显式注解（跨模块调用禁止隐式
  单态化，E2001）；同模块内部调用保持无注解推断。
- **数据外置**：`data/sales.json` 运行时读取——改数据不改代码，脏数据
  （字符串价格/缺字段/负数量）由 clean 层统一丢弃（kept 14 / dropped 2）。
- **元组装不了 Dict**（E1007）：报表函数返回 `(table, top_cat, top_rev)`
  全标量元组；六表累加器走 Dict 原地更新而非元组回收。
- **JSON 摘要手搓**：f-string 拼 JSON 数组（字段序确定），不经 json_encode。
- 报告写到**运行时 CWD**（约定从 demo 目录运行，见 build.ps1）。
