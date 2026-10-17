// 裸汇编模拟驱动：LoadLibrary/GetProcAddress 取 msvcrt printf，
// 经 sim.s（AOT 精确寄存器状态）调用。
#include <windows.h>
#include <stdio.h>

typedef void (__attribute__((ms_abi)) *sim_fn)(void *target, const char *fmt);
typedef int (__attribute__((ms_abi)) *fflush_fn)(void*);

int main(void) {
    HMODULE h = LoadLibraryA("msvcrt.dll");
    if (!h) { fprintf(stderr, "LoadLibrary failed\n"); return 1; }
    void *p = (void *)GetProcAddress(h, "printf");
    fflush_fn f = (fflush_fn)GetProcAddress(h, "fflush");
    if (!p) { fprintf(stderr, "printf not found\n"); return 1; }
    sim_fn sim = (sim_fn)(void *)sim;
    sim(p, "i=%d f=%.2f\n");
    if (f) f(0);
    return 0;
}
