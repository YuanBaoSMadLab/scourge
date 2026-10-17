// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
// ============================================================================
//  Scourge 编译器 — 裸机运行时 · 核心段（Builder 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：--freestanding 通道的 sg_* 符号子集（白名单见 BuilderBare.cpp
//  的符号映射表）。从 BuilderRTCore.hpp 移植，三处结构性替换：
//    * libc → freestanding：无 stdio/stdlib/math，print 系列走 UART，
//      floor/sqrt/fmod 走 AArch64 指令（frint*/fsqrt）或精确整数算法；
//    * malloc → 链接脚本 __heap_start 起步的 bump 分配（str/容器本就是
//      "程序生命周期持有" 契约，bump 与宿主 arena 语义同构）；
//    * f64 最短往返格式化自研（宿主走 snprintf %.*g + strtod）：十进制
//      大数（1e9 基 limb）做 P 位舍入与 round-trip 判定，全值域逐位精确，
//      输出与宿主 %g 规则逐字节一致（科学计数 e±NN 两位指数、去尾零、
//      整数浮点补 ".0"）。
//  语义纪律：panic 消息与 VM/宿主 AOT 逐字一致；白名单外的符号不实现，
//  链接期由 BuilderBare.cpp 映射为能力域诊断（io/net/json/gui/…）。
// ============================================================================

namespace scourge {
namespace bare {

inline const char kBareRtC[] = R"BRT(/* Scourge 裸机运行时·核心（--freestanding 生成；勿手工改）
 * 通道纪律：print 系列 → UART，sg_panic → UART + [scourge exit 1] + 停机。 */
#include <stdint.h>

/* crt 段导出（BuilderBareA64.hpp kA64CrtC） */
void sg_uart_putc(char c);
void sg_uart_puts(const char* s);
void sg_uart_puti(long long v);
void a64_exit(long long code);
extern char __heap_start[];

/* ---- panic（消息文本与 VM/宿主 AOT 逐字一致；裸机通道无 stderr） ---- */
void sg_panic(const char* fn, const char* file, int64_t line,
              const char* msg) {
    sg_uart_puts("panic: ");
    sg_uart_puts(msg);
    sg_uart_puts(" at ");
    sg_uart_puts(fn);
    sg_uart_puts(" (");
    sg_uart_puts(file);
    sg_uart_putc(':');
    sg_uart_puti((long long)line);
    sg_uart_putc('\n');
    a64_exit(1);
}

/* ---- bump 分配：__heap_start（栈后）向上，程序生命周期持有，零 free ---- */
static char* sg_heap_cur = 0;

static void* sg_balloc(unsigned long long n) {
    if (!sg_heap_cur) sg_heap_cur = __heap_start;
    unsigned long long p = ((unsigned long long)sg_heap_cur + 15) &
                           ~(unsigned long long)15;
    sg_heap_cur = (char*)(p + n);
    return (void*)p;
}

static char* sg_bdupn(const char* s, unsigned long long n) {
    char* p = (char*)sg_balloc(n + 1);
    for (unsigned long long i = 0; i < n; ++i) p[i] = s[i];
    p[n] = 0;
    return p;
}

static char* sg_bdup(const char* s) {
    unsigned long long n = 0;
    while (s[n]) ++n;
    return sg_bdupn(s, n);
}

/* ---- i64→十进制（panic 消息拼装用；静态缓冲单线程安全） ---- */
static char* sg_i64_str(long long v, char* out) {
    char tmp[24];
    int i = 24;
    unsigned long long u = v < 0 ? (unsigned long long)(-(v + 1)) + 1
                                 : (unsigned long long)v;
    do {
        tmp[--i] = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    if (v < 0) tmp[--i] = '-';
    int k = 0;
    while (i < 24) out[k++] = tmp[i++];
    out[k] = 0;
    return out;
}

/* ---- 整数算术（溢出/除零 panic，消息与 VM doBinOp 一致） ---- */
int64_t sg_add_i64(int64_t a, int64_t b, const char* fn, const char* file,
                   int64_t line) {
    int64_t out;
    if (__builtin_add_overflow(a, b, &out))
        sg_panic(fn, file, line, "整数加法溢出");
    return out;
}

int64_t sg_sub_i64(int64_t a, int64_t b, const char* fn, const char* file,
                   int64_t line) {
    int64_t out;
    if (__builtin_sub_overflow(a, b, &out))
        sg_panic(fn, file, line, "整数减法溢出");
    return out;
}

int64_t sg_mul_i64(int64_t a, int64_t b, const char* fn, const char* file,
                   int64_t line) {
    int64_t out;
    if (__builtin_mul_overflow(a, b, &out))
        sg_panic(fn, file, line, "整数乘法溢出");
    return out;
}

double sg_div_i64(int64_t a, int64_t b, const char* fn, const char* file,
                  int64_t line) {
    if (b == 0) sg_panic(fn, file, line, "整数除零");
    return (double)a / (double)b;
}

int64_t sg_floordiv_i64(int64_t a, int64_t b, const char* fn, const char* file,
                        int64_t line) {
    if (b == 0) sg_panic(fn, file, line, "整数整除零");
    if (a == INT64_MIN && b == -1) sg_panic(fn, file, line, "整数整除溢出");
    int64_t q = a / b;
    if (a % b != 0 && ((a < 0) != (b < 0))) --q; /* Python 向下取整 */
    return q;
}

int64_t sg_mod_i64(int64_t a, int64_t b, const char* fn, const char* file,
                   int64_t line) {
    if (b == 0) sg_panic(fn, file, line, "整数取模零");
    if (a == INT64_MIN && b == -1) return 0;
    int64_t m = a % b;
    if (m != 0 && ((m < 0) != (b < 0))) m += b; /* 与除数同号 */
    return m;
}

int64_t sg_ipow_i64(int64_t base, int64_t exp, const char* fn,
                    const char* file, int64_t line) {
    int64_t result = 1;
    while (exp > 0) {
        if (exp & 1)
            if (__builtin_mul_overflow(result, base, &result))
                sg_panic(fn, file, line, "整数幂运算溢出");
        exp >>= 1;
        if (exp > 0)
            if (__builtin_mul_overflow(base, base, &base))
                sg_panic(fn, file, line, "整数幂运算溢出");
    }
    return result;
}

int64_t sg_neg_i64(int64_t v, const char* fn, const char* file, int64_t line) {
    if (v == INT64_MIN) sg_panic(fn, file, line, "整数取负溢出");
    return -v;
}

/* ---- 浮点位级工具（无 math.h：位型判定/操作） ---- */
static int sg_isnan_d(double d) {
    union { double d; uint64_t u; } x;
    x.d = d;
    return (x.u << 1) > 0xffe0000000000000ull;
}

static int sg_isinf_d(double d) {
    union { double d; uint64_t u; } x;
    x.d = d;
    return (x.u << 1) == 0xffe0000000000000ull;
}

double sg_abs_f64(double v) {
    union { double d; uint64_t u; } x;
    x.d = v;
    x.u &= 0x7fffffffffffffffull;
    return x.d;
}

double sg_sqrt_f64(double v) {
    double r;
    __asm__ volatile("fsqrt %d0, %d1" : "=w"(r) : "w"(v));
    return r;
}

/* IEEE fmod 精确算法：2 的幂缩放对齐后逐次相减（Sterbenz 引理保证每步
 * 减法精确）；指数差 ≤ 2100 → 迭代 ≤ 2100 次收敛，每次 O(1)。 */
static double sg_fmod_impl(double x, double y) {
    if (sg_isnan_d(x) || sg_isnan_d(y) || sg_isinf_d(x)) {
        union { double d; uint64_t u; } n;
        n.u = 0x7ff8000000000000ull;
        return n.d;
    }
    if (sg_isinf_d(y)) return x;
    double ax = sg_abs_f64(x), ay = sg_abs_f64(y);
    if (ax == 0.0 || ay == 0.0) {
        union { double d; uint64_t u; } n;
        n.u = 0x7ff8000000000000ull;
        return n.d;
    }
    while (ax >= ay) {
        double scaled = ay;
        while (scaled * 2.0 <= ax) scaled *= 2.0; /* 精确（幂二缩放） */
        ax -= scaled; /* Sterbenz：scaled ∈ (ax/2, ax] → 差精确 */
    }
    union { double d; uint64_t u; } r;
    r.d = ax;
    r.u |= ((union { double d; uint64_t u; })x).u & 0x8000000000000000ull;
    return r.d;
}

double sg_fmod_f64(double a, double b, const char* fn, const char* file,
                   int64_t line) {
    if (b == 0.0) sg_panic(fn, file, line, "浮点取模零");
    double m = sg_fmod_impl(a, b);
    if (m != 0.0 && ((m < 0.0) != (b < 0.0))) m += b; /* 与除数同号 */
    return m;
}

/* ---- 取整族：frint 指令 + int64 域检查（消息与 VM panicHere 一致） ----
 * frintm=向负无穷(floor) frintp=向正无穷(ceil) frintn=半到偶(round)
 * frintz=向零(trunc)；NaN/±inf/超域 → panic。 */
static int64_t sg_int_range(double r, const char* fn, const char* file,
                            int64_t line, const char* name) {
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) {
        char out[72];
        char* o = out;
        const char* p = name;
        while (*p) *o++ = *p++;
        p = "() 结果超出 int64 范围";
        while (*p) *o++ = *p++;
        *o = 0;
        sg_panic(fn, file, line, out);
    }
    return (int64_t)r;
}

#define SG_FRINT_FN(cname, asmop, name)                                      \
    int64_t cname(double v, const char* fn, const char* file, int64_t line) {\
        double r;                                                            \
        __asm__ volatile(#asmop " %d0, %d1" : "=w"(r) : "w"(v));             \
        return sg_int_range(r, fn, file, line, name);                        \
    }
SG_FRINT_FN(sg_floor_f64, frintm, "floor")
SG_FRINT_FN(sg_ceil_f64, frintp, "ceil")
SG_FRINT_FN(sg_round_f64, frintn, "round")
SG_FRINT_FN(sg_trunc_f64, frintz, "trunc")

double sg_floordiv_f64(double a, double b, const char* fn, const char* file,
                       int64_t line) {
    if (b == 0.0) sg_panic(fn, file, line, "浮点整除零");
    double r;
    __asm__ volatile("frintm %d0, %d1" : "=w"(r) : "w"(a / b));
    return r;
}

int64_t sg_abs_i64(int64_t v) {
    return v < 0 ? (int64_t)(0 - (uint64_t)v) : v;
}

int64_t sg_int_from_f64(double v, const char* fn, const char* file,
                        int64_t line) {
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0))
        sg_panic(fn, file, line, "int() 浮点越界");
    return (int64_t)v;
}

/* ---- random/seed：splitmix64（与 VM::rngNext 逐位一致，确定性红线） ---- */
static uint64_t sg_rng_state = 0;

static uint64_t sg_rng_next(void) {
    sg_rng_state += 0x9E3779B97F4A7C15ull;
    uint64_t z = sg_rng_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double sg_random(void) {
    return (double)(sg_rng_next() >> 11) * (1.0 / 9007199254740992.0);
}

void sg_seed(int64_t v) { sg_rng_state = (uint64_t)v; }

double sg_now(void) {
    uint64_t frq, ct;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ct));
    return (double)ct / (double)frq;
}

void sg_fflush_stdout(void) {} /* UART 无缓冲，直通 */

/* ---- print 系列（UART 通道；与 VM stringify 同格式） ---- */
void sg_print_i64(int64_t v) { sg_uart_puti((long long)v); }
void sg_print_bool(int b) { sg_uart_puts(b ? "True" : "False"); }
void sg_print_cstr(const char* s) { sg_uart_puts(s); }
void sg_print_none(void) { sg_uart_puts("None"); }
void sg_print_space(void) { sg_uart_putc(' '); }
void sg_print_nl(void) { sg_uart_putc('\n'); }

/* ==== f64 最短往返格式化引擎（十进制大数，全值域精确） ====
 * 与宿主 snprintf("%.*g")+strtod 试探法语义逐字节对齐：
 *   1. d = m×2^e 精确展开为十进制大数（e≥0 连乘 2，e<0 连乘 5 + 负规模）；
 *   2. P=15..17 逐级：X = round(bn/10^(D-P))（D = bn 十进制位数，scale 恒
 *      相消），商/余数半到偶舍入得 P 位有效数字；
 *   3. round-trip 判定 = round(X×10^(E-P+1)) 位型 == d 位型（等价 strtod）；
 *   4. %g 排版：E < -4 || E >= P → 科学计数（e±NN 两位指数），否则定点；
 *      去尾零；无 '.'/'e' 补 ".0"。 */
#define SG_BN_LIMBS 96 /* 1e9 基 → 容量 ~10^864（覆盖 2^53×5^1074 ≈ 10^769） */

typedef struct {
    unsigned int d[SG_BN_LIMBS]; /* 低位在前，基 1e9 */
    int n;                       /* 有效 limb 数（0 = 值 0） */
} SgBn;

static void sg_bn_zero(SgBn* x) {
    for (int i = 0; i < SG_BN_LIMBS; ++i) x->d[i] = 0;
    x->n = 0;
}

static void sg_bn_from_u64(SgBn* x, uint64_t v) {
    sg_bn_zero(x);
    while (v) {
        x->d[x->n++] = (unsigned int)(v % 1000000000ull);
        v /= 1000000000ull;
    }
}

static void sg_bn_mul_small(SgBn* x, unsigned int m) { /* m < 2^32 */
    uint64_t carry = 0;
    for (int i = 0; i < x->n; ++i) {
        uint64_t cur = (uint64_t)x->d[i] * m + carry;
        x->d[i] = (unsigned int)(cur % 1000000000ull);
        carry = cur / 1000000000ull;
    }
    while (carry) {
        if (x->n >= SG_BN_LIMBS)
            sg_panic("<fmt>", "<fmt>", 0, "格式化大数溢出");
        x->d[x->n++] = (unsigned int)(carry % 1000000000ull);
        carry /= 1000000000ull;
    }
}

/* 十进制右移 10^k（k ≥ 0）：商写回 x，余数写 rem（= x mod 10^k）。
 * 分两步：整 limb 平移（余数 = 低 take 个 limb 原值）+ 非整 limb 长除
 * （余数 r5 占 limb[take]：R = r5×10^(9·take) + 低 take limb 原值）。 */
static void sg_bn_shr10(SgBn* x, int k, SgBn* rem) {
    int limbs = k / 9, rest = k % 9;
    sg_bn_zero(rem);
    int take = limbs < x->n ? limbs : x->n;
    for (int i = 0; i < take; ++i) rem->d[i] = x->d[i];
    rem->n = take;
    if (limbs >= x->n) {
        x->n = 0;
        while (rem->n > 0 && rem->d[rem->n - 1] == 0) --rem->n;
        return;
    }
    for (int i = 0; i < x->n - limbs; ++i) x->d[i] = x->d[i + limbs];
    x->n -= limbs;
    if (rest) {
        unsigned int div = 1;
        for (int i = 0; i < rest; ++i) div *= 10;
        uint64_t r = 0;
        for (int i = x->n - 1; i >= 0; --i) {
            uint64_t cur = r * 1000000000ull + x->d[i];
            x->d[i] = (unsigned int)(cur / div);
            r = cur % div;
        }
        while (x->n > 0 && x->d[x->n - 1] == 0) --x->n;
        /* r5 < div ≤ 1e8：直接占 limb[take]（rem 前 take 个 limb 未动） */
        if (r) rem->d[rem->n++] = (unsigned int)r;
    }
    while (rem->n > 0 && rem->d[rem->n - 1] == 0) --rem->n;
}

static void sg_bn_shl10(SgBn* x, int k) { /* ×10^k（k ≥ 0，调用方保容量） */
    int limbs = k / 9, rest = k % 9;
    if (limbs) {
        for (int i = x->n - 1; i >= 0; --i) x->d[i + limbs] = x->d[i];
        for (int i = 0; i < limbs; ++i) x->d[i] = 0;
        x->n += limbs;
    }
    if (rest) {
        unsigned int mul = 1;
        for (int i = 0; i < rest; ++i) mul *= 10;
        sg_bn_mul_small(x, mul);
    }
}

static int sg_bn_cmp(const SgBn* a, const SgBn* b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; --i) {
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    }
    return 0;
}

/* 小除数长除（div < 2^32；返回余数）——10^k 逐次除法用 */
static unsigned int sg_bn_div_small(SgBn* x, unsigned int div) {
    uint64_t r = 0;
    for (int i = x->n - 1; i >= 0; --i) {
        uint64_t cur = r * 1000000000ull + x->d[i];
        x->d[i] = (unsigned int)(cur / div);
        r = cur % div;
    }
    while (x->n > 0 && x->d[x->n - 1] == 0) --x->n;
    return (unsigned int)r;
}

static int sg_bn_digits(const SgBn* x) { /* 总十进制位数（x > 0） */
    unsigned int top = x->d[x->n - 1];
    int n = 0;
    while (top) {
        ++n;
        top /= 10;
    }
    return 9 * (x->n - 1) + n;
}

/* 大数 → 最近 double（round-to-nearest-even）：value = x×10^scale，
 * 返回位型（等价宿主 strtod：溢出 → ±inf，下溢 → ±0，subnormal 精确舍入）。
 * 负指数：A = x×2^T 后逐次除 10^k（基 1e9 精确），Q ≥ 2^54 保精度，
 * 余数非零进 sticky。窗口提取前先把基 1e9 精确转成基 2^16（逐次除
 * 2^16 取余）——纯二进制移位，无十进制重标度的精度混杂。 */
static uint64_t sg_bn_to_double_bits(uint64_t x, int scale, int neg) {
    uint64_t mant; /* 最高 ≤64 位窗口 */
    int bl;        /* 窗口内有效位数 */
    int dropped;   /* 窗口之下被折叠进 sticky 的位数 */
    int e2;        /* value = mant×2^(e2+dropped) + 低 bit（sticky） */
    int sticky = 0;
    /* 越界快路（x < 2^60；边界留足裕量，DBL_MAX≈1.8e308 /
     * 最小 subnormal 半值≈2.47e-324） */
    if (scale >= 310) return 0x7ff0000000000000ull | ((uint64_t)neg << 63);
    if (scale <= -342) return (uint64_t)neg << 63;
    SgBn b;
    sg_bn_from_u64(&b, x);
    if (scale >= 0) {
        for (int i = 0; i < scale; ++i) sg_bn_mul_small(&b, 10);
        e2 = 0;
    } else {
        int k = -scale;
        /* T = 128 + ⌈log2(10^k)⌉ 裕量 → Q = x×2^T/10^k ≥ 2^54 */
        int T = 128 + (10 * k) / 3 + 1;
        for (int i = 0; i < T; ++i) sg_bn_mul_small(&b, 2);
        int remnz = 0;
        for (int i = 0; i < k; ++i)
            if (sg_bn_div_small(&b, 10)) remnz = 1;
        e2 = -T;
        sticky = remnz;
    }
    /* 基 1e9 → 基 2^16（LSB 先；最大 ~1326 位 → ≤ 84 limb） */
    unsigned short g[SG_BN_LIMBS * 2 + 8];
    int m = 0;
    while (b.n > 0) g[m++] = (unsigned short)sg_bn_div_small(&b, 0x10000u);
    if (m == 0) return (uint64_t)neg << 63;
    int H = 16 * (m - 1);
    {
        unsigned short t = g[m - 1];
        while (t) {
            ++H;
            t >>= 1;
        }
    }
    /* 顶部 ≤8 个 limb（≤128 位）装进窗口 w；更低 limb 非零 → sticky。
     * w = value >> 16·(m-m8)，实际位宽 wb ∈ [H-16·(m-8)]；mant64 =
     * value >> (H-64) = w >> (wb-64)。 */
    unsigned __int128 w = 0;
    int m8 = m < 8 ? m : 8;
    for (int i = 0; i < m8; ++i) w = (w << 16) | (unsigned __int128)g[m - 1 - i];
    for (int i = 0; i < m - m8; ++i)
        if (g[i]) sticky = 1;
    int wb = 0;
    {
        unsigned __int128 t = w;
        while (t) {
            ++wb;
            t >>= 1;
        }
    }
    if (H > 64) {
        int sh = wb - 64; /* m ≥ 8 → [49,64]；m < 8 → H-64 ∈ [1,64] */
        mant = (uint64_t)(w >> sh);
        dropped = H - 64;
        if (w & (((unsigned __int128)1 << sh) - 1)) sticky = 1;
    } else {
        mant = (uint64_t)w; /* H ≤ 64 → m ≤ 4，w 即全值 */
        dropped = 0;
    }
    bl = 0;
    {
        uint64_t t = mant;
        while (t) {
            ++bl;
            t >>= 1;
        }
    }
    if (mant == 0) return (uint64_t)neg << 63; /* 下溢为 0 */
    /* value = mant×2^(e2+dropped)（+sticky）。目标 53 位：
     * m53 = mant >> (bl-53)，guard/sticky 取 mant 低 (bl-53) 位。 */
    int sh = bl - 53; /* -52..11 */
    uint64_t m53;
    int guard = 0;
    if (sh > 0) {
        guard = (int)((mant >> (sh - 1)) & 1);
        for (int i = 0; i < sh - 1; ++i) sticky |= (int)((mant >> i) & 1);
        m53 = mant >> sh;
    } else {
        m53 = mant << (-sh);
    }
    int exp = e2 + dropped + bl - 1; /* 未偏置指数（归一前） */
    if (guard && (sticky || (m53 & 1))) ++m53; /* 半到偶 */
    if (m53 >> 53) { /* 舍入进位 */
        m53 >>= 1;
        ++exp;
    }
    uint64_t bits;
    if (exp > 1023) {
        bits = 0x7ff0000000000000ull; /* 溢出 → inf（strtod 同语义） */
    } else if (exp >= -1022) {
        bits = ((uint64_t)(exp + 1023) << 52) | (m53 & 0x000fffffffffffffull);
    } else {
        int sub = -1022 - exp; /* subnormal：整体右移 sub 位 */
        if (sub >= 54) {
            bits = 0;
        } else {
            uint64_t frac = (m53 & 0x000fffffffffffffull) | (1ull << 52);
            uint64_t sh2 = (uint64_t)sub;
            uint64_t lost_mask = (1ull << sh2) - 1;
            int lost_any = (frac & lost_mask) != 0;
            int half = (int)((frac >> (sh2 - 1)) & 1);
            uint64_t q2 = frac >> sh2;
            if (half && (lost_any || (q2 & 1))) ++q2;
            bits = q2; /* subnormal：指数域 0 */
        }
    }
    if (neg) bits |= 0x8000000000000000ull;
    return bits;
}

/* P 位有效数字舍入：d（有限、非零）→ X（10^(P-1) ≤ X < 10^P）+ 首位
 * 指数 E。十进制大数精确展开；scale 在商式里恒相消：X = round(bn/10^(D-P))。 */
static void sg_f64_round_sig(double d, int P, uint64_t* outX, int* outE) {
    union { double d; uint64_t u; } ua;
    ua.d = sg_abs_f64(d);
    int rawExp = (int)((ua.u >> 52) & 0x7ff) - 1023;
    uint64_t frac = ua.u & 0x000fffffffffffffull;
    uint64_t m;
    int e;
    if (rawExp == -1023) { /* subnormal */
        m = frac;
        e = -1074;
    } else {
        m = frac | (1ull << 52);
        e = rawExp - 52;
    }
    SgBn bn;
    sg_bn_from_u64(&bn, m);
    if (e >= 0) {
        for (int i = 0; i < e; ++i) sg_bn_mul_small(&bn, 2);
    } else {
        for (int i = 0; i < -e; ++i) sg_bn_mul_small(&bn, 5); /* m×5^(-e) */
    }
    /* scale = min(e,0)：bn×10^scale = |d|（精确） */
    int scale = e < 0 ? e : 0;
    int D = sg_bn_digits(&bn);
    int E = D + scale - 1; /* 首位指数 */
    SgBn q, r;
    q = bn;
    uint64_t X;
    if (D - P >= 0) {
        sg_bn_shr10(&q, D - P, &r);
        X = 0;
        for (int i = q.n - 1; i >= 0; --i) X = X * 1000000000ull + q.d[i];
        /* 半到偶：2r vs 10^(D-P) */
        SgBn r2 = r, div;
        sg_bn_mul_small(&r2, 2);
        sg_bn_from_u64(&div, 1);
        sg_bn_shl10(&div, D - P);
        int cmp = sg_bn_cmp(&r2, &div);
        if (cmp > 0 || (cmp == 0 && (X & 1))) ++X;
    } else {
        sg_bn_shl10(&q, P - D); /* 不足 P 位：左移补齐（≤ 16 位，安全） */
        X = 0;
        for (int i = q.n - 1; i >= 0; --i) X = X * 1000000000ull + q.d[i];
    }
    uint64_t powP = 1; /* P 位满格进位：X == 10^P → X=10^(P-1), E++ */
    for (int i = 0; i < P; ++i) powP *= 10;
    if (X >= powP) {
        X /= 10;
        ++E;
    }
    *outX = X;
    *outE = E;
}

/* round-trip 判定：X×10^j 最近舍入回 d？位型比较（等价宿主 strtod）。 */
static int sg_f64_rt_ok(uint64_t X, int j, double d) {
    union { double d; uint64_t u; } bits;
    bits.d = d;
    int neg = (int)(bits.u >> 63);
    return sg_bn_to_double_bits(X, j, neg) == bits.u;
}

static const char* sg_fmt_f64(double d) {
    static char buf[64];
    if (sg_isnan_d(d)) return "nan";
    if (sg_isinf_d(d)) return d > 0 ? "inf" : "-inf";
    union { double d; uint64_t u; } bits;
    bits.d = d;
    int neg = (int)(bits.u >> 63);
    double a = sg_abs_f64(d);
    uint64_t X = 0;
    int E = 0;
    int P = 15;
    if (a == 0.0) {
        P = 1; /* 0 排版走定点整路 */
    } else {
        for (P = 15; P <= 17; ++P) {
            sg_f64_round_sig(a, P, &X, &E);
            if (sg_f64_rt_ok(X, E - P + 1, a)) break;
        }
    }
    /* X → digits（P 位；X=0 时 1 位） */
    char digits[24];
    int nd = 0;
    {
        uint64_t v = X;
        char tmp[24];
        int i = 24;
        if (v == 0) tmp[--i] = '0';
        while (v) {
            tmp[--i] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (i < 24) digits[nd++] = tmp[i++];
    }
    /* %g 排版（去尾零；E < -4 || E >= P → 科学计数） */
    char body[32];
    int nb = 0;
    if (a == 0.0) {
        body[nb++] = '0';
    } else if (E < -4 || E >= P) {
        body[nb++] = digits[0];
        int last = nd;
        while (last > 1 && digits[last - 1] == '0') --last;
        if (last > 1) {
            body[nb++] = '.';
            for (int i = 1; i < last; ++i) body[nb++] = digits[i];
        }
        body[nb++] = 'e';
        body[nb++] = E < 0 ? '-' : '+';
        int ae = E < 0 ? -E : E;
        if (ae >= 100) body[nb++] = (char)('0' + (ae / 100) % 10);
        body[nb++] = (char)('0' + (ae / 10) % 10);
        body[nb++] = (char)('0' + ae % 10);
    } else {
        /* 定点：小数点在 E+1 位后 */
        if (E >= nd - 1) {
            for (int i = 0; i < nd; ++i) body[nb++] = digits[i];
            for (int i = nd; i <= E; ++i) body[nb++] = '0';
        } else if (E >= 0) {
            int intLen = E + 1;
            for (int i = 0; i < intLen; ++i) body[nb++] = digits[i];
            int last = nd;
            while (last > intLen && digits[last - 1] == '0') --last;
            if (last > intLen) {
                body[nb++] = '.';
                for (int i = intLen; i < last; ++i) body[nb++] = digits[i];
            }
        } else {
            body[nb++] = '0';
            body[nb++] = '.';
            for (int i = 0; i < -E - 1; ++i) body[nb++] = '0';
            int last = nd;
            while (last > 1 && digits[last - 1] == '0') --last;
            for (int i = 0; i < last; ++i) body[nb++] = digits[i];
        }
    }
    body[nb] = 0;
    char* w = buf;
    if (neg) *w++ = '-'; /* 含 -0.0（与 printf %g 一致） */
    const char* p = body;
    while (*p) *w++ = *p++;
    *w = 0; /* 先终止本轮字符串——hasDot 严禁扫到上轮残留 */
    /* 宿主纪律：无 '.'/'e' → 补 ".0"（整数浮点） */
    int hasDot = 0;
    for (const char* q = buf; *q; ++q)
        if (*q == '.' || *q == 'e') hasDot = 1;
    if (!hasDot) {
        *w++ = '.';
        *w++ = '0';
    }
    *w = 0;
    return buf;
}

const char* sg_str_from_f64(double d) { return sg_bdup(sg_fmt_f64(d)); }

void sg_print_f64(double v) { sg_uart_puts(sg_fmt_f64(v)); }

/* ---- str 基础（拼接/重复/别名登记/累加器；语义同宿主 kRuntimeC） ---- */
const char* sg_str_concat(const char* a, const char* b) {
    unsigned long long na = 0, nb = 0;
    while (a[na]) ++na;
    while (b[nb]) ++nb;
    char* p = (char*)sg_balloc(na + nb + 1);
    for (unsigned long long i = 0; i < na; ++i) p[i] = a[i];
    for (unsigned long long i = 0; i < nb; ++i) p[na + i] = b[i];
    p[na + nb] = 0;
    return p;
}

const char* sg_str_repeat(const char* s, int64_t n, const char* fn,
                          const char* file, int64_t line) {
    if (n < 0) n = 0; /* Python 语义：负次数 → 空串 */
    const int64_t kMaxBytes = (int64_t)64 << 20;
    unsigned long long sl = 0;
    while (s[sl]) ++sl;
    if (n != 0 && (int64_t)sl > kMaxBytes / n)
        sg_panic(fn, file, line, "字符串重复结果过长（上限 64MB）");
    char* p = (char*)sg_balloc(sl * (unsigned long long)n + 1);
    for (int64_t k = 0; k < n; ++k)
        for (unsigned long long i = 0; i < sl; ++i) p[sl * k + i] = s[i];
    p[sl * (unsigned long long)n] = 0;
    return p;
}

/* str 别名登记（开放寻址哈希集；bump 存储，扩容旧块泄漏即 arena 纪律） */
static const char** sg_str_esc_set = 0;
static unsigned long long sg_str_esc_cap = 0;
static unsigned long long sg_str_esc_len = 0;

static uint64_t sg_str_hash_ptr(const void* p) {
    uint64_t x = (uint64_t)(unsigned long long)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

static int sg_str_escaped(const char* p) {
    if (!p || sg_str_esc_cap == 0) return 0;
    unsigned long long mask = sg_str_esc_cap - 1;
    unsigned long long i = sg_str_hash_ptr(p) & mask;
    while (sg_str_esc_set[i]) {
        if (sg_str_esc_set[i] == p) return 1;
        i = (i + 1) & mask;
    }
    return 0;
}

static void sg_str_esc_put(const char* p) {
    unsigned long long mask = sg_str_esc_cap - 1;
    unsigned long long i = sg_str_hash_ptr(p) & mask;
    while (sg_str_esc_set[i]) i = (i + 1) & mask;
    sg_str_esc_set[i] = p;
    sg_str_esc_len++;
}

void sg_str_mark(const char* p) {
    if (!p || sg_str_escaped(p)) return;
    if ((sg_str_esc_len + 1) * 4 > sg_str_esc_cap * 3) {
        unsigned long long ncap = sg_str_esc_cap ? sg_str_esc_cap * 2 : 64;
        const char** neu = (const char**)sg_balloc(ncap * sizeof(char*));
        for (unsigned long long k = 0; k < ncap; ++k) neu[k] = 0;
        const char** old = sg_str_esc_set;
        unsigned long long oldcap = sg_str_esc_cap;
        sg_str_esc_set = neu;
        sg_str_esc_cap = ncap;
        sg_str_esc_len = 0;
        for (unsigned long long k = 0; k < oldcap; ++k)
            if (old[k]) sg_str_esc_put(old[k]);
    }
    sg_str_esc_put(p);
}

/* 累加器登记表（原地扩容快路径；语义同宿主 sg_str_concat_grow） */
typedef struct {
    char* ptr;
    unsigned long long len, cap;
} SgAcc;

static SgAcc* sg_accs = 0;
static int sg_accs_n = 0;

const char* sg_str_concat_grow(int64_t acc, const char* a, const char* b) {
    unsigned long long nb = 0;
    while (b[nb]) ++nb;
    if (acc >= 0 && acc < sg_accs_n) {
        SgAcc* s = &sg_accs[acc];
        if (a == s->ptr && s->len + nb + 1 <= s->cap && !sg_str_escaped(a)) {
            for (unsigned long long i = 0; i <= nb; ++i)
                s->ptr[s->len + i] = b[i];
            s->len += nb;
            return s->ptr;
        }
    }
    unsigned long long na = 0;
    while (a[na]) ++na;
    unsigned long long need = na + nb + 1;
    unsigned long long cp = 32;
    while (cp < need) cp <<= 1;
    char* p = (char*)sg_balloc(cp);
    for (unsigned long long i = 0; i < na; ++i) p[i] = a[i];
    for (unsigned long long i = 0; i <= nb; ++i) p[na + i] = b[i];
    if (acc >= 0) {
        if (acc >= sg_accs_n) {
            int n = sg_accs_n ? sg_accs_n : 8;
            while (n <= acc) n *= 2;
            SgAcc* na2 = (SgAcc*)sg_balloc(sizeof(SgAcc) * (unsigned)n);
            for (int i = 0; i < n; ++i) {
                na2[i].ptr = 0;
                na2[i].len = 0;
                na2[i].cap = 0;
            }
            for (int i = 0; i < sg_accs_n; ++i) na2[i] = sg_accs[i];
            sg_accs = na2;
            sg_accs_n = n;
        }
        sg_accs[acc].ptr = p;
        sg_accs[acc].len = na + nb;
        sg_accs[acc].cap = cp;
    }
    return p;
}

int64_t sg_str_cmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    int c = (unsigned char)*a - (unsigned char)*b;
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

int sg_str_contains(const char* hay, const char* needle) {
    if (!*needle) return 1;
    for (; *hay; ++hay) {
        const char* h = hay;
        const char* n = needle;
        while (*h && *n && *h == *n) {
            ++h;
            ++n;
        }
        if (!*n) return 1;
    }
    return 0;
}

int64_t sg_len_str(const char* s) {
    int64_t n = 0;
    while (s[n]) ++n;
    return n;
}

/* ---- str() 构造 ---- */
const char* sg_str_from_i64(int64_t v) {
    char buf[32];
    sg_i64_str((long long)v, buf);
    return sg_bdup(buf);
}

const char* sg_str_from_bool(int b) { return b ? "True" : "False"; }
)BRT";

} // namespace bare
} // namespace scourge
