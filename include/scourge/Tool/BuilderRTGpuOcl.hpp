// ============================================================================
//  Scourge 编译器 — AOT C 运行时 · GPU OpenCL C kernel（Builder 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：--gpu 时经 cStringLiteral 内嵌进 _rt.c（clBuildProgram 运行时
//  JIT）。与 verify/cuda/sg_kernels.ll 逐 kernel 镜像——.ll 为语义权威，
//  两文件改动必须同步：确定性纪律同 .ll 头注（浮点/整数运算顺序与 CPU 逐位
//  一致——mm/sum 串行升序归约，逐元素每元素恰一次 IEEE RN 运算；int 溢出
//  判定与 CPU __builtin_*_overflow 逐位一致，回绕值照常写回，panic 由运行时
//  回读状态后以同一 sg_panic 终止）。FP_CONTRACT OFF + -cl-fmad=false 禁止
//  fma 收缩（sg_mm 的 mul+add 必须保持两条 IEEE 运算，与 CPU 逐位一致）。
// ============================================================================
#pragma once

namespace scourge {

inline const char kGpuOpenCLC[] = R"OCL(/* sg_kernels.cl — Scourge OpenCL 后端 kernel（sg_kernels.ll 逐 kernel 镜像；
 * .ll 为语义权威）。覆盖集：sg_ew_tt/ts/ti/tsi、sg_mm、sg_sum/sg_sum_i、
 * sg_tr。索引取全局线性 id（1D）或 (row=gid.y, col=gid.x)（2D 32x32 tile），
 * 与 CUDA launch 的 ctaid*ntid+tid 覆盖同一索引集；越界线程空转。 */
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#pragma OPENCL FP_CONTRACT OFF

/* ---- int 溢出仿真（llvm.sadd/ssub/smul.with.overflow.i64 同构） ----
 * 回绕结果经无符号运算构造（规避有符号溢出 UB，位型与硬件回绕一致）；
 * 判定式：
 *   add: ((x^r)&(y^r))<0（同号相加得异号）
 *   sub: ((x^y)&(x^r))<0（异号相减与 x 异号）
 *   mul: 0/±1 特判（避开 -1 x INT64_MIN 除法陷阱）后 r/y != x——若 r/y==x
 *        则 r=x*y+s、|s|<|y|，而 r≡x*y (mod 2^64) 强制 s=0 即无溢出；
 *        逆否：溢出必 r/y != x。除法操作数排除 0/-1 后无陷阱。 */
static inline long sg_wadd(long x, long y) {
    return (long)((unsigned long)x + (unsigned long)y);
}
static inline long sg_wsub(long x, long y) {
    return (long)((unsigned long)x - (unsigned long)y);
}
static inline long sg_wmul(long x, long y) {
    return (long)((unsigned long)x * (unsigned long)y);
}
static inline int sg_ovf_add(long x, long y, long r) {
    return (int)(((x ^ r) & (y ^ r)) < 0);
}
static inline int sg_ovf_sub(long x, long y, long r) {
    return (int)(((x ^ y) & (x ^ r)) < 0);
}
static inline int sg_ovf_mul(long x, long y, long r) {
    if (x == 0 || y == 0) return 0;
    if (x == -1) return y == (-0x7fffffffffffffffL - 1);
    if (y == -1) return x == (-0x7fffffffffffffffL - 1);
    return r / y != x;
}

/* ---- sg_ew_tt：out[i] = a[i] op b[i]，op 0=+ 1=- 2=* ---- */
__kernel void sg_ew_tt(__global double *out, __global const double *a,
                       __global const double *b, long n, int op) {
    long idx = (long)get_global_id(0);
    if (idx >= n) return;
    double va = a[idx], vb = b[idx], r;
    if (op == 0)
        r = va + vb;
    else if (op == 1)
        r = va - vb;
    else if (op == 2)
        r = va * vb;
    else
        return;
    out[idx] = r;
}

/* ---- sg_ew_ts：sleft=0 -> a[i] op s；1 -> s op a[i] ---- */
__kernel void sg_ew_ts(__global double *out, __global const double *a,
                       double s, long n, int op, int sleft) {
    long idx = (long)get_global_id(0);
    if (idx >= n) return;
    double va = a[idx];
    double x = sleft ? s : va;
    double y = sleft ? va : s;
    double r;
    if (op == 0)
        r = x + y;
    else if (op == 1)
        r = x - y;
    else if (op == 2)
        r = x * y;
    else
        return;
    out[idx] = r;
}

/* ---- sg_ew_ti：int tt，溢出 atomic_or 上报（回绕值照常写回） ---- */
__kernel void sg_ew_ti(__global long *out, __global const long *a,
                       __global const long *b, long n, int op,
                       __global int *status) {
    long idx = (long)get_global_id(0);
    if (idx >= n) return;
    long va = a[idx], vb = b[idx], r;
    int f;
    if (op == 0) {
        r = sg_wadd(va, vb);
        f = sg_ovf_add(va, vb, r);
    } else if (op == 1) {
        r = sg_wsub(va, vb);
        f = sg_ovf_sub(va, vb, r);
    } else if (op == 2) {
        r = sg_wmul(va, vb);
        f = sg_ovf_mul(va, vb, r);
    } else
        return;
    if (f) atomic_or(status, 1);
    out[idx] = r;
}

/* ---- sg_ew_tsi：int ts/st，标量 i64；溢出上报同 sg_ew_ti ---- */
__kernel void sg_ew_tsi(__global long *out, __global const long *a, long s,
                        long n, int op, int sleft, __global int *status) {
    long idx = (long)get_global_id(0);
    if (idx >= n) return;
    long va = a[idx];
    long x = sleft ? s : va;
    long y = sleft ? va : s;
    long r;
    int f;
    if (op == 0) {
        r = sg_wadd(x, y);
        f = sg_ovf_add(x, y, r);
    } else if (op == 1) {
        r = sg_wsub(x, y);
        f = sg_ovf_sub(x, y, r);
    } else if (op == 2) {
        r = sg_wmul(x, y);
        f = sg_ovf_mul(x, y, r);
    } else
        return;
    if (f) atomic_or(status, 1);
    out[idx] = r;
}

/* ---- sg_mm：shared-memory tiling 32x32（local 必须由 launch 固定 32x32）。
 * 每输出元素仍按 p=0..k-1 升序 acc += a[i][p]*b[p][j]（mul/add 两条 IEEE
 * 运算，FP_CONTRACT OFF 保证不收缩 fma）；p>=k 的 padding 乘加经 if 丢弃
 * （保 -0.0 符号）；tile 装载越界格读 (0,0) 安全地址、写 0.0。 ---- */
__kernel void sg_mm(__global double *out, __global const double *a,
                    __global const double *b, long m, long n, long k) {
    __local double sg_mm_at[1024], sg_mm_bt[1024];
    long tx = (long)get_local_id(0);
    long ty = (long)get_local_id(1);
    long col = (long)get_global_id(0);
    long row = (long)get_global_id(1);
    int ok = (row < m) && (col < n);
    long tidx2 = ty * 32 + tx;
    double acc = 0.0;
    for (long p0 = 0; p0 < k; p0 += 32) {
        long ac = p0 + tx;
        int al = (row < m) && (ac < k);
        long asafe = al ? (row * k + ac) : (long)0;
        double va = a[asafe];
        sg_mm_at[tidx2] = al ? va : 0.0;
        long brw = p0 + ty;
        int bl = (brw < k) && (col < n);
        long bsafe = bl ? (brw * n + col) : (long)0;
        double vb = b[bsafe];
        sg_mm_bt[tidx2] = bl ? vb : 0.0;
        barrier(CLK_LOCAL_MEM_FENCE);
        for (long p2 = 0; p2 < 32; ++p2) {
            double va2 = sg_mm_at[ty * 32 + p2];
            double vb2 = sg_mm_bt[p2 * 32 + tx];
            double pr = va2 * vb2;
            double f = acc + pr;
            if (p0 + p2 < k) acc = f;
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (ok) out[row * n + col] = acc;
}

/* ---- sg_sum：单线程 k 升序 fadd 累加（与 CPU 逐位一致） ---- */
__kernel void sg_sum(__global double *out, __global const double *a, long n) {
    if (get_global_id(0) != 0) return;
    double acc = 0.0;
    for (long kk = 0; kk < n; ++kk) acc = acc + a[kk];
    out[0] = acc;
}

/* ---- sg_sum_i：单线程升序 sadd 溢出仿真累加；溢出普通 store（单写者） ---- */
__kernel void sg_sum_i(__global long *out, __global int *status,
                       __global const long *a, long n) {
    if (get_global_id(0) != 0) return;
    long acc = 0;
    for (long kk = 0; kk < n; ++kk) {
        long v = a[kk];
        long r = sg_wadd(acc, v);
        if (sg_ovf_add(acc, v, r)) *status = 1;
        acc = r;
    }
    *out = acc;
}

/* ---- sg_tr：out[i*m+j] = a[j*n+i]（a 为 m x n，out 为 n x m） ---- */
__kernel void sg_tr(__global double *out, __global const double *a, long m,
                    long n) {
    long idx = (long)get_global_id(0);
    long cnt = m * n;
    if (idx >= cnt) return;
    long i = idx / m;
    long j = idx % m;
    out[idx] = a[j * n + i];
}

/* ---- 0.9.3 扩展覆盖集镜像（语义权威 sg_kernels.ll：sg_red_f/red_i/un_f/
 * un_ai/un_si/cpy；比较/转换逐位一致红线与 .ll 注释一致） ---- */
__kernel void sg_red_f(__global double *out, __global const double *a, long n,
                       int isMax) {
    if (get_global_id(0) != 0) return;
    double best = a[0];
    for (long k = 1; k < n; ++k) {
        double x = a[k];
        best = isMax ? (x > best ? x : best) : (x < best ? x : best);
    }
    out[0] = best;
}

__kernel void sg_red_i(__global long *out, __global const long *a, long n,
                       int isMax) {
    if (get_global_id(0) != 0) return;
    long best = a[0];
    for (long k = 1; k < n; ++k) {
        long x = a[k];
        best = isMax ? (x > best ? x : best) : (x < best ? x : best);
    }
    out[0] = best;
}

__kernel void sg_un_f(__global double *out, __global const double *a, long n,
                      int fnid) {
    long idx = (long)get_global_id(0);
    if (idx >= n) return;
    /* fnid 0=abs（fabs 清符逐位一致）1=sqrt（OpenCL sqrt 为 IEEE 正确舍入，
     * 与 libm/CPU 一致；-cl-fmad=false 不影响单目函数） */
    out[idx] = fnid == 0 ? fabs(a[idx]) : sqrt(a[idx]);
}

__kernel void sg_un_ai(__global long *out, __global const long *a, long n) {
    long idx = (long)get_global_id(0);
    if (idx >= n) return;
    long x = a[idx];
    out[idx] = x < 0 ? -x : x;
}

__kernel void sg_un_si(__global double *out, __global const long *a, long n) {
    long idx = (long)get_global_id(0);
    if (idx >= n) return;
    out[idx] = sqrt((double)a[idx]);
}

__kernel void sg_cpy(__global long *out, __global const long *a, long off,
                     long n) {
    long idx = (long)get_global_id(0);
    if (idx >= n) return;
    out[idx] = a[off + idx];
}
")OCL";

} // namespace scourge
