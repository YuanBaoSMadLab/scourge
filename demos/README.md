# Scourge 演示项目集合（demos / 演示文件夹）

语言版本：**Scourge 0.9.10**（静态类型、张量原生、AOT 原生可执行）。
本目录用这门语言在多个应用方向写了一批**可直接构建运行**的演示，源码即产物、单文件 AOT 可执行，供社区参考与二次开发。

> 主方向为 **AI·ML（张量原生 + autodiff + nn）**，故该方向包含两个演示（04、05）；其余为语言已验证可用的方向：系统工具、网络、数据处理。

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

| 目录 | 方向 | 演示内容 | 展示的语言能力 |
|---|---|---|---|
| `01_system_tool` | 系统工具 | 扫描目录下的 `.sg/.md/.txt`，统计每个文件的行数/词数并汇总 | `capability io`、`list_dir`/`read_text`/`exists`、`str.split`/`endswith`、`while` 循环、文件读写 |
| `02_web_server` | 网络 | 多路由 HTTP JSON API（`/`、`/health`、`/echo`、`/sum`、404 兜底） | `capability net`、`net_bind`/`net_accept`/`net_http_read_request`/`net_http_write_response`、`json_decode` |
| `03_data_pipeline` | 数据处理 | 解析 JSON 数据集，按分类聚合求和/计数/均值 | `json_decode`、JSON 取值（`get`/`as_*`）、`List` 聚合、`float()` 转换 |
| `04_ml_regression` | **AI·ML（主方向）** | 用 `std.autodiff` + `std.nn`（Adam）训练 MLP 拟合 `sin(x)` | `import std.autodiff/nn as …`、标量 tape 自动微分、线性层/`mse`/`gather_grads`/`adam`、张量无关纯 `.sg` 源码库 |
| `05_ml_classification` | **AI·ML（主方向）** | 用 autodiff + nn 训练 MLP 解决 **XOR** 非线性分类 | 同上 + 对称性打破、训练循环、`try/except` 之外的纯数值收敛 |

## 实测输出（已验证）

**01_system_tool**（在自身目录运行，扫描到 `main.sg` 47 行 / 172 词）
```
main.sg 47 172
TOTAL files 1 lines 47 words 172
avg_lines_per_file 47.0
```

**02_web_server**（后台启动，curl 4 个请求后自动退出）
```
GET  /        → {"service":"scourge-demo","version":"0.9.10","routes":["/","/health","/echo","/sum"]}
POST /sum [1,2,3,4] → {"sum":10.0}
GET  /nope    → {"error":"not-found"}
GET  /health  → {"status":"ok"}
```

**03_data_pipeline**
```
food 30.0 3 10.0
book 50.0 2 25.0
tool 55.0 1 55.0
grand_total 135.0 records 6
```

**04_ml_regression**（Adam，loss 持续下降，网络学到 sin 的正负半周与过零）
```
epoch 0    loss 94.07
epoch 7999 loss 0.47  (更长训练后进一步下降)
x=-0.478 true 0.998 pred 0.534   x=0.000 true 0.0   pred 0.043
x=0.478  true -0.998 pred -0.539  x=1.000 true 0.0 pred -1.056
```

**05_ml_classification**（XOR，精确收敛）
```
epoch 5999 loss 4.3e-32
in 0 0 pred -0.0 class 0 target 0
in 0 1 pred 1.0  class 1 target 1
in 1 0 pred 1.0  class 1 target 1
in 1 1 pred -0.0 class 0 target 0
accuracy 4 / 4
```

## 构建注意事项（社区踩坑提示）

1. **模块解析**：`import std.autodiff as ad` 这类标准库导入，需从仓库根目录构建，否则编译器找不到 `std/`。
2. **同包多子模块导入**要用 `as` 别名（如 `import std.autodiff as ad; import std.nn as nn`），否则顶层名 `std` 重复（E2007）。
3. **能力门禁**：涉及文件读写需 `capability io`，涉及网络需 `capability net`；缺则报 E5004。
4. **列表字面量**：`[1,2,3]` 默认是 **Tensor** 而非 List；做列表请用 `xs: List[int] = []` 后 `append(...)`。
5. **Tuple 暂不支持 AOT**（VM 可编译）：跨 AOT 请用平行 `List` 而非 `Tuple`，否则报 E2008。
6. 网络服务为单线程串行（官方契约），演示中处理固定数量请求后退出，便于验证。

## 一句话总结

系统工具 / 网络 / 数据处理可直接当正经小工具用；**AI·ML 主方向已具备 autodiff + nn 的真实训练闭环**（XOR 4/4、sin 拟合），是这门语言"张量原生、为 AI 协同编程设计"定位的直接体现。并发原语、桌面 GUI、跨平台开箱仍非当前强项，社区可据此选择落地方向。
