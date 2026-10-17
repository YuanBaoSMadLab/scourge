// msvcrt printf 变参对照实验：LoadLibrary/GetProcAddress 拿符号后调用，
// 与 AOT 侧 sg_extern_resolve 同路径。C 编译器按 vararg 发射（AL=1）。
#include <windows.h>
#include <stdio.h>

typedef int (__attribute__((ms_abi)) *printf_fn)(const char*, ...);
typedef int (__attribute__((ms_abi)) *fflush_fn)(void*);

int main(void) {
    HMODULE h = LoadLibraryA("msvcrt.dll");
    if (!h) { fprintf(stderr, "LoadLibrary failed\n"); return 1; }
    printf_fn p = (printf_fn)GetProcAddress(h, "printf");
    fflush_fn f = (fflush_fn)GetProcAddress(h, "fflush");
    if (!p) { fprintf(stderr, "printf not found\n"); return 1; }
    p("i=%d f=%.2f\n", 42, 3.5);
    if (f) f(0);
    return 0;
}
