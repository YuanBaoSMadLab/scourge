# ============================================================================
#  train_linreg.py — train_linreg.sg 的纯 Python 逐语句镜像（无 numpy）
# ----------------------------------------------------------------------------
#  目的：与 Scourge 版本做同逻辑对照——同公式、同数据、同语句序、同浮点
#  操作顺序，双方最终 w/b/loss 输出应逐字符串一致（IEEE 754 双精度确定性），
#  elapsed_ms 反映各自解释器执行同工作负载的耗时。
#
#  运行：   python examples/train_linreg.py
# ============================================================================

import time

N = 24
EPOCHS = 3000
LR = 0.05


def matmul(a, b):
    # 与 Scourge tensorMatMul 同序：k 升序累加，初值 0.0
    out = []
    for i in range(len(a)):
        row = []
        for j in range(len(b[0])):
            acc = 0.0
            for k in range(len(b)):
                acc += a[i][k] * b[k][j]
            row.append(acc)
        out.append(row)
    return out


def sum_t(t):
    # 与 Scourge sum() 同序：行主序扁平累加，初值 0.0
    acc = 0.0
    for row in t:
        for v in row:
            acc += v
    return acc


def add_s(t, s):
    return [[v + s for v in row] for row in t]


def sub(t, u):
    return [[a - b for a, b in zip(r1, r2)] for r1, r2 in zip(t, u)]


def mul_s(t, s):
    return [[v * s for v in row] for row in t]


def mul_tt(t, u):
    return [[a * b for a, b in zip(r1, r2)] for r1, r2 in zip(t, u)]


def main():
    t0 = time.perf_counter()

    X = [[0.0, 0.0] for _ in range(N)]
    Y = [[0.0] for _ in range(N)]
    Xt = [[0.0] * N, [0.0] * N]

    # 数据生成（与 .sg 逐语句镜像）
    for i in range(N):
        x1 = (i % 8) * 0.25 - 1.0
        x2 = (i % 5) * 0.5 - 1.0
        noise = ((i * 7) % 13 - 6) * 0.05
        X[i][0] = x1
        X[i][1] = x2
        Y[i][0] = 3.0 * x1 + 1.5 * x2 - 2.0 + noise
        Xt[0][i] = x1
        Xt[1][i] = x2

    # 全批量梯度下降（向量化）
    w = [[0.0], [0.0]]
    b = 0.0
    for epoch in range(EPOCHS):
        pred = add_s(matmul(X, w), b)
        err = sub(pred, Y)
        gw = mul_s(matmul(Xt, err), (2.0 / N))
        gb = sum_t(err) * (2.0 / N)
        w = sub(w, mul_s(gw, LR))
        b = b - LR * gb

    # 终评 + 报告
    pred = add_s(matmul(X, w), b)
    err = sub(pred, Y)
    loss = sum_t(mul_tt(err, err)) / N

    print("epochs=" + str(EPOCHS) + " N=" + str(N))
    print("w0=" + str(w[0][0]))
    print("w1=" + str(w[1][0]))
    print("b=" + str(b))
    print("loss=" + str(loss))
    print("elapsed_ms=" + str((time.perf_counter() - t0) * 1000.0))


if __name__ == "__main__":
    main()
