# Python 对照：与 t17_bench.sg / t19_scalar.sg 完全同算法
# 用法：python bench_py.py
import time

N = 128

def val(i, j):
    return ((i * 7 + j * 3) % 11) * 0.5

# 与 Scourge t17 相同公式生成矩阵
A = [[val(i, j) for j in range(N)] for i in range(N)]
B = [[val(i + 5, j) for j in range(N)] for i in range(N)]

# 1) 纯 Python 128x128 矩阵乘（三重循环）
def matmul_py(A, B):
    n = len(A)
    C = [[0.0] * n for _ in range(n)]
    for i in range(n):
        Ai = A[i]
        Ci = C[i]
        for k in range(n):
            aik = Ai[k]
            if aik == 0.0:
                continue
            Bk = B[k]
            for j in range(n):
                Ci[j] += aik * Bk[j]
    return C

t0 = time.perf_counter()
C = matmul_py(A, B)
t1 = time.perf_counter()
print("py_c00=" + str(C[0][0]))
print("py_c_last=" + str(C[127][127]))
print("py_sum=" + str(sum(sum(row) for row in C)))
print("py_loop_ms=" + str((t1 - t0) * 1000.0))

# 2) numpy 128x128 矩阵乘（OpenBLAS）
import numpy as np
An = np.array(A)
Bn = np.array(B)
t0 = time.perf_counter()
Cn = An @ Bn
t1 = time.perf_counter()
print("np_c00=" + str(float(Cn[0][0])))
print("np_sum=" + str(float(Cn.sum())))
print("np_ms=" + str((t1 - t0) * 1000.0))

# 3) 标量循环 1000 万次（与 t19_scalar.sg 同算法）
t0 = time.perf_counter()
total = 0
for i in range(10000000):
    total += i
t1 = time.perf_counter()
print("py_loop_total=" + str(total))
print("py_loop10m_ms=" + str((t1 - t0) * 1000.0))

# 4) fib(30) 递归
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

t0 = time.perf_counter()
r = fib(30)
t1 = time.perf_counter()
print("py_fib30=" + str(r))
print("py_fib_ms=" + str((t1 - t0) * 1000.0))

# 5) numpy 1024x1024 矩阵乘（与 t18 对照）
N2 = 1024
def val2(i, j):
    return ((i * 7 + j * 3) % 11) * 0.5
A2 = np.array([[val2(i, j) for j in range(N2)] for i in range(N2)])
B2 = np.array([[val2(i + 5, j) for j in range(N2)] for i in range(N2)])
t0 = time.perf_counter()
C2 = A2 @ B2
t1 = time.perf_counter()
print("np1024_sum=" + str(float(C2.sum())))
print("np1024_ms=" + str((t1 - t0) * 1000.0))

print("python=" + __import__("sys").version.split()[0])
print("numpy=" + np.__version__)
