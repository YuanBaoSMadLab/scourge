# CPython baseline for bench_fib / bench_call / bench_loop (same workloads).
import time

def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)

def inc(x):
    return x + 1

def bench_fib():
    t0 = time.perf_counter()
    r = fib(28)
    t1 = time.perf_counter()
    print(r)
    print("elapsed_s=%.6f" % (t1 - t0))

def bench_call():
    t0 = time.perf_counter()
    s = 0
    i = 0
    while i < 2000000:
        s = inc(s)
        i = i + 1
    t1 = time.perf_counter()
    print(s)
    print("elapsed_s=%.6f" % (t1 - t0))

def bench_loop():
    t0 = time.perf_counter()
    s = 0
    i = 0
    while i < 100000000:
        s = s + i
        i = i + 1
    t1 = time.perf_counter()
    print(s)
    print("elapsed_s=%.6f" % (t1 - t0))

bench_fib()
bench_call()
bench_loop()
