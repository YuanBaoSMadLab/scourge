# 05 · 棋盘非线性分类（AI·ML 主方向）

autodiff + nn(Adam) 学 2D 棋盘非线性边界：4x4 网格 16 点，标签 =
(x0>0) XOR (x1>0)（对角同色）。相比 XOR 4 点，决策边界更复杂，需要足够
隐元 + 随机初始化打破对称。输出逐点预测表、总体与分类别准确率。
随机种子固定 → VM/AOT 双模逐字节一致。

## 构建与运行

```powershell
# 方式一：一键构建 + 运行（推荐）
sg  # 编译器需先在仓库根构建
powershell -File build.ps1

# 方式二：手动（在仓库根执行；import std.* 由仓库根 std/ 解析）
sg build demos\05_ml_classification\main.sg
demos\05_ml_classification\out\main.exe

# VM 直跑（无 gui/IO 依赖，双模均可）
sg run demos\05_ml_classification\main.sg
```

## 模块结构（多文件 import 协作）

| 文件 | 职责 | 关键能力 |
|---|---|---|
| `main.sg` | 装配入口：epoch 循环 + 逐点评估表 + 准确率 | 纯控制台，无 capability 需求 |
| `dataset.sg` | 数据层：16 点棋盘坐标与标签（纯函数） | 笛卡尔积线性扫描、XOR 标签 |
| `model.sg` | 模型层：参数初始化 + 双输入推理前向 | std.autodiff/nn、显式注解 API |
| `train.sg` | 训练层：单 epoch forward+backward+Adam | List 切片分段 Adam、跨模块原地更新 |

## 实测输出（VM/AOT 双模逐字节一致，且与重构前单文件版一致）

```
epoch 0 loss 2.0772728452502958
epoch 3000 loss 0.7961434274925739
epoch 5999 loss 0.011113316786093789
--- checkerboard evaluation (16 points) ---
in  -1.5  -1.5  pred -0.052  class 0  target 0  ok
in  -1.5  +1.5  pred 0.846  class 1  target 1  ok
in  +1.5  -1.5  pred 1.045  class 1  target 1  ok
in  +1.5  +1.5  pred 0.059  class 0  target 0  ok
accuracy 16 / 16
class0 8 / 8   class1 8 / 8
```

## 语言要点

- **跨模块可变参数原地更新**：`train.run_epoch` 收 12 张参数/动量表原地
  更新；Adam 时间步以值语义进出（返回元组 `(t, avg_loss)` 推进）。
- **数据即模块**：dataset.sg 用线性扫描生成笛卡尔积序（免整除语义），
  训练与评估共用同一 `label_at()` 定义。
- **双模确定性**：无 gui/IO 依赖，VM 与 AOT 输出逐字节一致——种子固定 +
  splitmix 确定性初始化的完整闭环。
