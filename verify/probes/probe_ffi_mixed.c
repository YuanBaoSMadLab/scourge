// 混合签名定参探针的被调方 DLL（MS x64 位置分配规则）：
//   第 p 个参数（0 起）占 GPR/XMM 序第 p 槽——整型类进 rcx/rdx/r8/r9，
//   浮点类进 xmm0-3，第 5+ 参数按位置序入栈。切勿按 SysV AMD64 的
//   "两类独立计数"理解：混合签名 (int, double) 的 double 在 xmm1
//   （参数位置 1），不是 xmm0。
// 本文件由 verify/verify_ffi_mixed.ps1 以 clang 编为 probe_ffi_mixed.dll；
// 若调用方误用独立计数，下列函数全部返回错值（第二参数恒 0）。
#include <stddef.h>

__declspec(dllexport) double m_if(int a, double b) {
    return (double)a + b; /* a: rcx(位置0)  b: xmm1(位置1) */
}

__declspec(dllexport) double m_fi(double a, long long b) {
    return a + (double)b; /* a: xmm0(位置0)  b: rdx(位置1) */
}

__declspec(dllexport) double m_idd(int a, double b, double c) {
    return (double)a + b * 10.0 + c * 100.0; /* rcx, xmm1, xmm2 */
}

__declspec(dllexport) double m_6(int a, double b, int c, double d, int e,
                                 double f) {
    /* 前 4 参：rcx, xmm1, rdx, xmm3；第 5/6 参按位置入栈 */
    return (double)a + b + (double)c + d + (double)e + f;
}
