# 04 · sin(x) 回归（AI·ML 主方向，GUI 实时训练）

autodiff + nn(Adam) 拟合 sin(x)，MLP 1-32-1，GUI 窗口实时刷新 epoch/loss
（点 X 提前中止，训练完自动关闭），随后输出训练/验证 RMSE 与 21 点密集
网格误差。随机种子固定 → 输出逐字节可复现。

## 构建与运行

```powershell
# 方式一：一键构建 + 运行（推荐）
scg  # 编译器需先在仓库根构建
powershell -File build.ps1

# 方式二：手动（在仓库根执行；import std.* 由仓库根 std/ 解析）
scg build demos\04_ml_regression\main.sg
demos\04_ml_regression\out\main.exe
```

> gui 域仅 AOT/Windows 可跑：VM（`scg run`）运行到 `gui_*` 即固定 panic，
> 属双模契约的一部分（与 verify_b6 探针同口径）。

## 模块结构（多文件 import 协作）

| 文件 | 职责 | 关键能力 |
|---|---|---|
| `main.sg` | 装配入口：GUI 编排 + epoch 循环 + RMSE/网格评估 | `capability gui`、gui_window/label/poll/set_text/close |
| `dataset.sg` | 数据层：40 点 sin(x) 纯函数数据源（x/y/归属/真值） | 纯函数无 IO，偶训练奇验证 |
| `model.sg` | 模型层：参数初始化（rand_uniform）+ 推理前向 | std.autodiff/nn、显式注解 API |
| `train.sg` | 训练层：单 epoch forward+backward+Adam | List 切片分段 Adam、跨模块原地更新 |

## 实测输出（与重构前单文件版逐字节一致）

```
epoch 0 train_loss 0.20663496224661024
epoch 8000 train_loss 0.00015340460568226028
epoch 15999 train_loss 6.672013298628752e-05
train_rmse 0.0071   val_rmse 0.0069
x -1.00 true 0.000 pred +0.018 abserr 0.018
x -0.50 true 0.707 pred +0.714 abserr 0.006
x +0.00 true 1.000 pred +0.997 abserr 0.003
x +0.50 true 0.707 pred +0.707 abserr 0.000
x +1.00 true 0.000 pred -0.017 abserr 0.017
grid_mae 0.0062   grid_max_abserr 0.0183
```

## 语言要点

- **跨模块可变参数原地更新**：`train.run_epoch` 收 12 张参数/动量表原地
  更新（std.nn.adam 同款模式），前提是参数全显式注解；Adam 时间步 `t`
  以值语义进出（返回元组 `(t, avg_loss)` 推进）。
- **epoch 是内建保留名**：训练函数用 `run_epoch` 规避（E2007）。
- **数据即模块**：dataset.sg 只回答"第 s 个样本是什么"，训练/评估/网格
  三处共用同一真值定义 `truth()`，杜绝平行实现。
- **import std.***：从仓库根构建即可解析（std/ 在仓库根），无需设
  SCOURGE_PACKAGES；`scg run` 同理。
