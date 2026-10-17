# Scourge 演示项目集合（demos / 演示文件夹）

语言版本：**Scourge 0.9.10**（静态类型、张量原生、AOT 单文件可执行）。
本目录每个演示都是**完整可用的多文件工程**，而非功能片段：每个 demo =
`main.sg` 装配入口 + 按职责拆分的 3~4 个源码模块 + 外置数据（如有）+
`build.ps1` 一键脚本 + README。01 是带 CLI 参数的代码库度量工具，02 是
前后端完整的博客站，03 是含清洗校验与报告落盘的 ETL 管道，04/05 是训练
闭环与评估方法论齐备的 ML 实验。供社区参考与二次开发。

> 主方向为 **AI·ML（张量原生 + autodiff + nn）**，包含 04、05 两个演示；
> 并发（thread/channel）融入 02，桌面 GUI（gui_*）融入 04，不单列 demo。

## 如何构建与运行

每个 demo 目录自带 `build.ps1`（可从任意位置调用）：从仓库根构建 →
在 demo 目录运行。手动方式（在**仓库根**执行，这样 `import std.*` 才能
解析到仓库根的 `std/`）：

```powershell
# AOT 构建 → 产物落 <demo>/out/main.exe（无运行时依赖，随处分发）
sg build demos\01_system_tool\main.sg
demos\01_system_tool\out\main.exe --top=3

# VM 直跑（SG-BC 字节码，缓存落 <demo>/.sgbuild/）
sg run demos\01_system_tool\main.sg
```

**产物目录纪律**（编译器内建约定）：最终产物 → `out/`，中间产物与缓存
（.mlir/.ll/_rt.c/obj/.sgbc）→ `.sgbuild/`——源码目录永远只有源码。

除 04 外，每个演示均通过 **VM/AOT 双模逐字节一致**验证（stdout/stderr/
落盘文件全部哈希比对）；04/05 的输出另与拆分前的单文件版本逐字节比对
通过。04 的 gui 域仅 AOT/Windows 可跑（VM 运行到 `gui_*` 即固定 panic，
属双模契约）。

## 目录与模块结构

| 目录 | 方向 | 源码模块（main 之外的职责拆分） | 展示的语言能力 |
|---|---|---|---|
| `01_system_tool` | 系统工具 | `scan.sg`（目录扫描/7 元组度量）· `stats.sg`（扩展名聚合+直方图）· `report.sg`（表格+Markdown 模板） | `capability io, env`、`args()`、for-in、Tuple 记录、Dict 聚合、`mean/median/std/min/max/sorted/reversed`、**定宽 fmt 键降序**、三引号 f-string 模板、`write_text` |
| `02_web_server` | 网络 | `views_html.sg`（HTML/CSS 呈现）· `json_api.sg`（JSON 响应构造）· `stats_hub.sg`（TopN 定宽键统计） | `capability net, thread`、`thread_spawn`/`channel`（SYNC 栅栏 + QUIT 哨兵）、跨请求有状态（状态集中 main）、`json_decode`、f-string HTML/JSON 模板、定宽键 TopN |
| `03_data_pipeline` | 数据处理 | `clean.sg`（json_decode+类型守卫清洗）· `aggregate.sg`（双维聚合+Z-score 离群）· `data/sales.json`（外置数据 16 条，含 2 条脏数据） | `json_decode`、`is_str/is_int/is_float/as_*` 类型守卫、Dict.get 默认值累加、跨模块可变 Dict（全注解 `accum`）、try/except 兜底、三引号报告模板、`write_text` |
| `04_ml_regression` | **AI·ML（主方向）** | `dataset.sg`（40 点 sin 纯函数数据源）· `model.sg`（初始化+推理前向）· `train.sg`（单 epoch Adam 训练步） | `capability gui`、`gui_window/label/poll/set_text/close`、`std.autodiff/nn` 全套、**跨模块可变参数原地更新**（12 表进 `run_epoch`）、List 切片分段 Adam、`nn.rand_uniform` |
| `05_ml_classification` | **AI·ML（主方向）** | `dataset.sg`（16 点棋盘坐标/标签）· `model.sg`（双输入初始化+推理）· `train.sg`（单 epoch Adam 训练步） | 同 04 的模块化训练闭环（输入 2 维）、**三目表达式**、f-string 正负号对齐表、分类阈值与分类别统计、双模确定性 |

各 demo 的模块职责、构建方式与实测输出详见各自 README.md。

## 实测输出（双模验证摘录）

**01_system_tool**（在 demo 自身目录运行，扫描自身 4 模块 + 文档；`--top`/`--path` 可调）
```
== top 4 files by lines (.) ==
file                     lines  words   bytes  code
main.sg                     57    198    2158    41
README.md                   57    222    2474    37
report.sg                   52    219    2062    39
stats.sg                    45    178    1805    29
== summary ==
files 6  total_lines 268  total_words 1076  total_bytes 10517
mean 44.7  median 48.5  std 14.41  min 15  max 57
== by extension ==
md       files 2  lines 72  avg 36.0
sg       files 4  lines 196  avg 49.0
== histogram ==   0-9 0   10-49 3   50-99 3   100+ 0
report -> ./metrics-report.md
```

**02_web_server**（启动后 curl 打 8 个请求，服务处理完自动退出）
```
blog-up port=18099
blog-down requests=8 paths=6 hits=8
top /api/posts = 3
top /style.css = 1
top /post/99 = 1
```
响应体（节选）：
```
GET  /              → 200  首页 HTML（fetch('/api/posts') 动态渲染文章列表）
GET  /style.css     → 200  内嵌样式表
GET  /post/1        → 200  SSR 详情页：<h1>hello scourge</h1>…<p>1 views - <a href="/">back</a></p>
GET  /post/99       → 404  <h1>404</h1>
GET  /api/posts     → 200  [{"id":1,"title":"hello scourge","views":1}, …]
POST /api/posts     → 201  {"id":4,"title":"fourth post"}          # JSON 发文入库
GET  /api/posts     → 200  列表含第 4 篇（POST 即时可见）
GET  /api/stats     → 200  {"served":8,"paths":6,"hits":8,"top":[["/api/posts",3],…]}
```

**03_data_pipeline**（数据外置 `data/sales.json`，自动丢弃 2 条脏数据）
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

**04_ml_regression**（AOT 运行，GUI 窗口实时刷新 loss，训完自动关闭）
```
epoch 0 train_loss 0.20663496224661024
epoch 8000 train_loss 0.00015340460568226028
epoch 15999 train_loss 6.672013298628752e-05
train_rmse 0.0071   val_rmse 0.0069        # 训练/验证误差几乎一致 → 泛化良好
x -1.00 true 0.000 pred +0.018 abserr 0.018
x -0.50 true 0.707 pred +0.714 abserr 0.006
x +0.00 true 1.000 pred +0.997 abserr 0.003
x +0.50 true 0.707 pred +0.707 abserr 0.000
x +1.00 true 0.000 pred -0.017 abserr 0.017
grid_mae 0.0062   grid_max_abserr 0.0183
```

**05_ml_classification**（4×4 棋盘 16 点，双模逐字节一致）
```
epoch 0 loss 2.0772728452502958
epoch 3000 loss 0.7961434274925739
epoch 5999 loss 0.011113316786093789
--- checkerboard evaluation (16 points) ---
in  -1.5  -1.5  pred -0.052  class 0  target 0  ok
in  +0.5  +0.5  pred 0.009  class 0  target 0  ok
in  +1.5  -1.5  pred 1.045  class 1  target 1  ok
…（16 点全部 ok）
accuracy 16 / 16
class0 8 / 8   class1 8 / 8
```

## 风格与技巧说明

这批演示刻意展示"高阶写法"而非堆砌语法点，以下技巧在多个 demo 中复用：

- **跨模块 API 全显式注解**：模块边界上的函数一律显式标注参数与返回类型
  （`def measure_dir(path: str) -> List[Tuple[...]]`）——隐式单态化仅限
  定义模块内部调用；同模块辅助函数仍可走无注解推断。
- **状态归属纪律**：可变状态（02 的 posts/views、03 的六表）集中在持有
  编排职责的文件；跨模块传容器要么只读（02 呈现层），要么全注解原地更新
  （03 `accum`、04/05 `run_epoch` 收 12 张参数表）。
- **定宽键降序**（sorted 无 key 参数的标准替代）：把数值格式化进字符串
  前缀建排序键，`f"{v:06d}|{name}"` → `sorted` → `reversed` →
  `find("|")` 切回，01/02/03 三处复用。
- **SYNC 栅栏模式**（02）：后台线程与主线程通过两条 channel 通信——业务
  消息单向流入，`"SYNC"` 应答栅栏对齐后才读共享 Dict（保证双模输出确定），
  `"QUIT"` 哨兵 + `thread_join` 收尾。
- **f-string 即模板引擎**：`{x:>8.2f}` 对齐列、三引号 f-string 做
  HTML/JSON/Markdown 多行模板、`{{` `}}` 转义字面花括号；模块内可直接
  引用跨模块调用（`f"... {model.predict(...):+.3f} ..."`）。
- **三目表达式**：`"ok " if ok == 1 else "MISS"`（05）、`1 if p0 != p1v else 0`。
- **切片分段投喂**（04/05）：`g[0:p1]`、`g[p1:p1+nhid]`… 把扁平梯度切给
  各自 Adam 状态，免手写拷贝循环。
- **内置统计全家桶**：`sum/mean/std/median/min/max` 直接作用于 List（01/03）。
- **数据即模块**：04/05 的 dataset.sg 只回答"第 s 个样本是什么"，训练/
  评估共用同一真值/标签定义，杜绝平行实现；03 的流水数据外置 `data/`，
  改数据不改代码。

## 构建注意事项（社区踩坑提示）

1. **模块解析**：`import std.autodiff as ad` 需从仓库根目录构建/运行，否则编译器找不到 `std/`（报 E5001）；本地模块（`import clean`）相对入口文件目录解析，多文件工程无需额外配置。
2. **同包多子模块导入**要用 `as` 别名（`import std.autodiff as ad` + `import std.nn as nn`），否则顶层名 `std` 重复（E2007）。
3. **能力门禁**：文件读写需 `capability io`，`args()` 需 `capability env`，网络需 `capability net`，线程需 `capability thread`，GUI 需 `capability gui`；缺则报 E5004。多个能力可逗号并列：`capability io, env`。
4. **内建名不可重定义**：`epoch`/`params` 等是内建保留名，自定义函数撞名报 E2007（04/05 的训练函数因此叫 `run_epoch`）。
5. **变量复用与类型**：同一作用域（含 if/elif/else 共享链、for 循环变量）不能把同名变量声明成不同类型——demo03 清洗循环因此把 Json 临时变量命名为 `jc/jr/ja/jq` 与后续 str 变量区分。
6. **`/` 是真除法**：`int / int` 得 `float`；整数下标用 `int()` 或保持 int 运算。
7. **元组不可变**：更新 `List[Tuple]` 元素须整体重建元组赋值（02 的 posts 追加即此写法）；元组元素类型不支持 Dict/List（E1007）——聚合多表用 Dict 原地更新或多函数配对返回。
8. **ML 初始化**：tanh 隐层必须用 `nn.rand_uniform` 随机初始化打破对称（常值/交替符号会塌缩成常数输出）；04/05 均以 `seed()` 固定种子保证可复现与双模一致。
9. **Adam 学习率**：逐样本（batch=1）训练时 lr=0.02 量级会震荡发散（04 实测 loss 先爆后回落），0.005 量级稳定收敛；分类任务（05）0.05 可用。
10. **gui 域仅 AOT/Windows**：VM 运行到 `gui_*` 即固定 panic（双模契约）；GUI 程序用 `gui_poll` 周期泵消息，训完 `gui_close` 自动退出便于脚本验证。
11. **网络服务为单线程 accept 循环**（官方契约），02 以「固定 8 请求后退出」便于自动化验证；PV 统计由后台线程异步完成，不阻塞响应。
12. **build.ps1 编码**：含中文注释的 .ps1 必须带 UTF-8 BOM（PowerShell 5.1 无 BOM 时按 GBK 解析会报语法错）。

## 一句话总结

01 可直接当代码库度量器用，02 是前后端完整的博客站（SSR + JSON API + fetch 前端 + 异步统计），03 是数据外置、带清洗校验与双格式报告落盘的 ETL，**AI·ML 主方向具备模块化的 autodiff + nn 真实训练闭环**（sin 回归 RMSE≈0.007、棋盘分类 16/16）。所有演示多文件 import 协作、固定种子、双模逐字节可复现。
