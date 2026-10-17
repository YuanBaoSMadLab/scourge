# Scourge 演示项目集合（demos / 演示文件夹）

语言版本：**Scourge 0.9.10**（静态类型、张量原生、AOT 原生可执行）。
本目录用这门语言在多个应用方向写了一批**可直接构建运行**的演示。相比初版，本轮把每个演示的**逻辑量与工程度**都显著加强（多维度统计 / 有状态服务 / 完整训练实验流程），使其成为对语言与运行时有真实检验价值的样例，而非玩具。源码即产物、单文件 AOT 可执行，供社区参考与二次开发。

> 主方向为 **AI·ML（张量原生 + autodiff + nn）**，该方向包含两个演示（04、05）；其余为语言已验证可用的方向：系统工具、网络、数据处理。

## 如何构建与运行

从**仓库根目录**执行（这样 `import std.*` 才能解析到仓库根的 `std/`）：

```bash
# 构建（AOT → 同目录生成 main.exe）
build-rel/scourge.exe build demos/01_system_tool/main.sg
# 运行
demos/01_system_tool/main.exe
```

每个子目录结构一致：`main.sg`（源码）+ `main.exe`（AOT 产物）。

## 目录与方向

| 目录 | 方向 | 演示内容（加强后） | 展示的语言能力 |
|---|---|---|---|
| `01_system_tool` | 系统工具 | **源代码度量分析器**：逐文件统计 行/词/字节/空行/注释行/代码行，按行数降序排序，按扩展名聚合，输出 均值/最小/最大/中位数 与行数分布直方图 | `capability io`、`list_dir`/`read_text`、`str.split`/`trim`/`startswith`、自定义函数、嵌套循环、自写选择排序、`sorted`、`Tuple` 记录、类型自动推断、聚合统计 |
| `02_web_server` | 网络 | **有状态 JSON API**：路由 `/` `/health` `/echo` `/sum` `/fib` `/stats` `/kv`，含服务端请求计数、内存 KV 存储（GET/POST）、迭代斐波那契、查询串解析、按方法分发、完善错误码 | `capability net`、`net_bind`/`net_accept`/`net_http_read_request`/`net_http_write_response`、`json_decode`、字符串切分、跨请求状态维护 |
| `03_data_pipeline` | 数据处理 | **多维销售分析 ETL**：解析 JSON 流水，按「类别」「区域」两维分组，每组算 计数/总额/总量/均值/最小/最大/标准差/均价，按销售额降序，输出占比、Grand Total 与离群点检测 | `json_decode`、多维分组聚合、统计量（方差/标准差/`sqrt`）、自写排序、`float()` 转换、平行数组 |
| `04_ml_regression` | **AI·ML（主方向）** | **MLP 回归完整实验**：autodiff+nn(Adam) 训练拟合 `sin` 单峰；训练/验证集划分、学习曲线、训练/验证 RMSE、密集网格预测的平均/最大绝对误差 | `import std.autodiff/nn as …`、标量 tape 自动微分、`linear_nodes`/`mse`/`gather_grads`/`adam`、权重初始化调优、评估方法论 |
| `05_ml_classification` | **AI·ML（主方向）** | **2D 棋盘非线性分类**：从 XOR 4 点升级为 4×4 网格 16 点，类别 = `(x1>0) XOR (x2>0)`；报告总体准确率 + 分类别准确率 + 逐点预测 | 同上 + 非线性决策边界、分类阈值、混淆统计 |

## 实测输出（已验证）

**01_system_tool**（在自身目录运行，扫描到 `main.sg`）
```
== per-file metrics (sorted by lines desc) ==
name lines words bytes blank comment code
main.sg 183 665 5586 16 13 154
== summary ==
files 1
total_lines 183
total_words 665
total_bytes 5586
mean_lines 183.0  min_lines 183  max_lines 183  median_lines 183
== by extension ==
sg files 1 lines 183 avg 183.0
== line-count histogram ==
0-9 0   10-49 0   50-99 0   100+ 1
```

**02_web_server**（后台启动，curl 打各路由；服务处理 14 个请求后自动退出）
```
GET  /              → {"service":"scourge-demo","version":"0.9.10","routes":[...]}
GET  /health        → {"status":"ok","requests":2}          # 请求计数器（有状态）
POST /sum [1,2,3,4,5]→ {"count":5,"sum":15.0}
POST /fib {"n":10}  → {"n":10,"fib":55}                      # fib(10)=55
POST /kv {"key":"city","value":"beijing"} → {"ok":true,"key":"city","size":1}
GET  /kv?key=city   → {"key":"city","value":"beijing"}       # 内存 KV 读取
GET  /kv?key=missing→ {"error":"no-such-key"}                # 404
GET  /stats         → {"total":8,"by_path":{...},"by_status":{"200":6,"404":1}}
GET  /nope          → {"error":"not-found"}                  # 404
```

**03_data_pipeline**（14 条销售流水，4 类别 × 3 区域）
```
== by category (sorted by revenue desc) ==
tool n 3 revenue 145.0 qty 4 mean 48.33 min 42.0 max 55.0 std 5.31 price 36.25 share% 42.34
book n 4 revenue 103.0 ... share% 30.07
food n 5 revenue 54.0  ... share% 15.77
toy  n 2 revenue 40.5  ... share% 11.82
== by region ==
north n 6 revenue 163.0 ...   south n 4 revenue 93.5 ...   east n 4 revenue 86.0 ...
== grand total ==
records 14 revenue 342.5 qty 23 avg_order 24.46
== outliers (amt > mean+std = 38.79) ==
outlier tool north amt 55.0   outlier tool east amt 42.0   outlier tool south amt 48.0
outlier_count 3
```

**04_ml_regression**（Adam，train/val 划分，损失下降并收敛）
```
epoch 0     train_loss 52.68
epoch 15999 train_loss 0.0052
train_rmse 0.0656   val_rmse 0.0609        # 训练/验证误差接近 → 泛化良好
--- dense grid [-1,1] x sin(pi*(x+1)/2) ---
x -0.5 true 0.707 pred 0.715 abserr 0.007
x  0.0 true 1.000 pred 1.022 abserr 0.022
grid_mae 0.0446   grid_max_abserr 0.203    # 误差集中在两端外推区
```

**05_ml_classification**（4×4 棋盘 16 点，精确收敛）
```
epoch 0     loss 0.831
epoch 11999 loss 5.7e-05
--- checkerboard evaluation (16 points) ---
in -1.5  0.5 pred 1.011 class 1 target 1
in  0.5  0.5 pred 0.004 class 0 target 0
...（对角同色、异对角异色，全部正确）
accuracy 16 / 16
class0 8 / 8   class1 8 / 8
```

## 构建注意事项（社区踩坑提示）

1. **模块解析**：`import std.autodiff as ad` 这类标准库导入，需从仓库根目录构建，否则编译器找不到 `std/`。
2. **同包多子模块导入**要用 `as` 别名（如 `import std.autodiff as ad; import std.nn as nn`），否则顶层名 `std` 重复（E2007）。
3. **能力门禁**：涉及文件读写需 `capability io`，涉及网络需 `capability net`；缺则报 E5004。
4. **列表/张量字面量**：一维数值字面量 `[1,2,3]` 默认推断为 **List**；需要 `Tensor` 请显式注解，如 `v: Tensor[int, (3,)] = [1, 2, 3]`（demo04/05 的数据集即此写法）。
5. **`/` 是真除法**：`int / int` 得 `float`；要整数下标用 `floor(nf / 2)`（demo01 中位数即此写法）。
6. **if/elif/else 共享同一作用域**：同一 `if` 链里不能把同名变量声明成不同类型（demo02 各分支因此用 `sk`/`fn`/`pkey` 等独立命名）。
7. **元组**：`Tuple[K, ...]` 注解、`(a, b, ...)` 字面量、下标读、`append`、`List[Tuple]` 元素整体替换均 VM/AOT 双模一致（demo01 行收集即用 7 元组记录；元组本身不可变，更新槽位须重建元组赋值）。
8. **类型自动推断**：变量初值与函数传参均可不标注类型——无注解参数按调用点实参类型自动生成特化（隐式单态化），标注注解则更严谨；递归函数的返回类型仍须显式注解。
9. 网络服务为单线程串行（官方契约），演示中处理固定数量请求后退出，便于验证。
10. **ML 初始化**：tanh 隐层若用交替符号 + 均匀小偏置，易塌缩成常数输出；demo04/05 采用「陡权重 + 跨输入域铺开的偏置」初始化才稳定收敛。

## 一句话总结

系统工具 / 网络 / 数据处理可直接当正经小工具用（多维统计、有状态服务、ETL 聚合）；**AI·ML 主方向已具备 autodiff + nn 的真实训练闭环与完整评估方法论**（回归 RMSE≈0.06、棋盘分类 16/16）。并发原语、桌面 GUI、跨平台开箱仍非当前强项，社区可据此选择落地方向。
