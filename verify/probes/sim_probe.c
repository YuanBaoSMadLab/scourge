// AOT vararg 发射状态模拟（inline asm）：rcx=fmt、rdx=42、r8=3.5 位模式、
// xmm2=3.5、xmm0/1/3=0、AL=函数地址低字节。二分变量：xmm0 内容。
#include <windows.h>
#include <stdio.h>

typedef int (__attribute__((ms_abi)) *fflush_fn)(void *);

int main(int argc, char **argv) {
    HMODULE h = LoadLibraryA("msvcrt.dll");
    if (!h) { fprintf(stderr, "LoadLibrary failed\n"); return 1; }
    void *target = (void *)GetProcAddress(h, "printf");
    fflush_fn f = (fflush_fn)GetProcAddress(h, "fflush");
    if (!target) { fprintf(stderr, "printf not found\n"); return 1; }
    volatile double d35 = 3.5;
    const char *fmt = "i=%d f=%.2f\n";
    void *t = target;
    __asm__ volatile(
        "movq %0, %%rax\n\t"
        "movq %1, %%rcx\n\t"
        "movl $42, %%edx\n\t"
        "movsd %2, %%xmm2\n\t"
        "movq %%xmm2, %%r8\n\t"
        "xorps %%xmm0, %%xmm0\n\t"
        "xorps %%xmm1, %%xmm1\n\t"
        "xorps %%xmm3, %%xmm3\n\t"
        "callq *%%rax\n\t"
        :
        : "r"(t), "r"(fmt), "m"(d35)
        : "rax", "rcx", "rdx", "r8", "xmm0", "xmm1", "xmm2", "xmm3", "memory");
    if (f) f(0);
    return 0;
}
