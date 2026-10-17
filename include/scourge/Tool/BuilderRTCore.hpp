// ============================================================================
//  Scourge 编译器 — AOT C 运行时 · 基础设施段（Builder 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：kRuntimeC 第 1 段（原 Builder.cpp 内嵌单体按功能域拆分）——
//  try 蹦床 / panic / 字符串分配与去重登记 / 累加器 / 浮点格式化 / print /
//  RNG。后续段：kTensorRuntimeC（张量）→ kCollRuntimeC（Result/List/Dict/
//  Set）→ kIoRuntimeC（sorted 与文件 IO）→ 能力门禁 → 各专项片段
//  （BuilderRTStr/Net/Sys/Std/Json/Http）。build 时按序拼接写入 <out>_rt.c，
//  拼接顺序 = C 依赖顺序（后续段复用本段 sg_panic/sg_dup/sg_fmt_f64）；
//  缓存键 rtObjKey 逐段计入文本。
//  语义纪律：与 SG-BC VM（src/BC/VM.cpp）逐字对齐，双模差分由 verify 探针锁定。
// ============================================================================
#pragma once

namespace scourge {

inline const char kRuntimeC[] = R"RT(/* Scourge AOT 运行时（scourge build 内嵌生成；与 SG-BC VM 语义逐一对齐）。
 * 通道纪律：print 系列写 stdout，sg_panic 写 stderr 并 exit(1)。 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h> /* int(str) 全消费解析的 ERANGE 判定 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/* 控制台编码初始化（__attribute__((constructor)) 先于 main 执行）：
 * print 系列输出 UTF-8 字节，将输出代码页切至 65001，中文等多字节文本
 * 在任意区域设置的控制台原样显示；重定向到管道/文件时写原始字节流，
 * 不受代码页影响。 */
__attribute__((constructor)) static void sg_console_init(void) {
    SetConsoleOutputCP(CP_UTF8);
}
#else
/* POSIX（Linux/macOS 与 BSD）：终端与管道原生字节流（UTF-8 环境），无需
 * 控制台代码页初始化；目录/文件/时钟 API 用 POSIX 对应物（跨平台支持）。 */
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#endif

/* ---- try/except 蹦床 ----
 * setjmp 由生成代码直调 CRT _setjmp：SgTry.jb 为首成员，句柄即 jmp_buf*，
 * 返回点落在生成函数自身活跃帧（包装函数方案失效——longjmp 恢复的包装帧
 * 会被 try body 内同调用深度的兄弟调用覆盖，恢复到死帧）。returns_twice
 * 调用点属性由 InProcess 在 translate 后程序化补齐。处理器栈 sg_try_top
 * 与 VM catches_ 同构：EnterTry=push、ExitTry=pop、panic=recover（弹到
 * 最近 handler）。 */
#include <setjmp.h>

typedef struct SgTry {
    jmp_buf jb;
    struct SgTry* prev;
} SgTry;

static SgTry* sg_try_top = NULL;

SgTry* sg_try_push(void) {
    SgTry* h = (SgTry*)malloc(sizeof(SgTry));
    if (!h) exit(1);
    h->prev = sg_try_top;
    sg_try_top = h;
    return h;
}

void sg_try_pop(SgTry* h) {
    sg_try_top = h->prev;
    free(h);
}

_Noreturn void sg_panic(const char* fn, const char* file, int64_t line,
                        const char* msg) {
    if (sg_try_top) /* try/except 激活：跳到最近 handler（与 VM recover 一致） */
        longjmp(sg_try_top->jb, 1);
    fprintf(stderr, "panic: %s at %s (%s:%lld)\n", msg, fn, file,
            (long long)line);
    exit(1);
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

/* ---- 浮点算术 ---- */
double sg_fdiv_f64(double a, double b, const char* fn, const char* file,
                   int64_t line) {
    if (b == 0.0) sg_panic(fn, file, line, "浮点除零");
    return a / b;
}

double sg_floordiv_f64(double a, double b, const char* fn, const char* file,
                       int64_t line) {
    if (b == 0.0) sg_panic(fn, file, line, "浮点整除零");
    return floor(a / b);
}

double sg_fmod_f64(double a, double b, const char* fn, const char* file,
                   int64_t line) {
    if (b == 0.0) sg_panic(fn, file, line, "浮点取模零");
    double m = fmod(a, b);
    if (m != 0.0 && ((m < 0.0) != (b < 0.0))) m += b; /* 与除数同号 */
    return m;
}

double sg_pow_f64(double a, double b, const char* fn, const char* file,
                  int64_t line) {
    if (a == 0.0 && b < 0.0) sg_panic(fn, file, line, "零的负次幂");
    return pow(a, b);
}

/* ---- 字符串（NUL 结尾 UTF-8；拼接/重复结果 malloc，程序生命周期持有） ---- */
/* 拼接/重复/复制结果走 arena 顺序 bump 分配：契约本就是"程序生命周期持有"
 * （运行时零 free），大循环拼接时通用 malloc 的 live set 线性膨胀，free 链
 * 翻找 + 页错误使单次拼接远贵于 memcpy 本身（实测比 VM 慢 ~7x——VM 侧
 * 引用计数即时释放、块复用全在缓存）。bump 把分配降回 O(1)、访问顺序化。
 * 语言无线程语义，静态游标单线程安全；超 1MB 的大块直连 malloc（护栏：
 * repeat 上限 64MB，避免 arena 内部碎片放大）。 */
static char* sg_str_alloc(size_t n) {
    static char* cur = 0;
    static size_t left = 0;
    if (n > (size_t)1 << 20) {
        char* p = (char*)malloc(n);
        if (!p) exit(1);
        return p;
    }
    if (left < n) {
        cur = (char*)malloc((size_t)1 << 20);
        if (!cur) exit(1);
        left = (size_t)1 << 20;
    }
    char* p = cur;
    cur += n;
    left -= n;
    return p;
}

static char* sg_dup(const char* s) {
    size_t n = strlen(s);
    char* p = sg_str_alloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

const char* sg_str_concat(const char* a, const char* b) {
    size_t na = strlen(a), nb = strlen(b);
    char* p = sg_str_alloc(na + nb + 1);
    memcpy(p, a, na);
    memcpy(p + na, b, nb + 1);
    return p;
}

const char* sg_str_repeat(const char* s, int64_t n, const char* fn,
                          const char* file, int64_t line) {
    if (n < 0) n = 0; /* Python 语义：负次数 → 空串 */
    const int64_t kMaxBytes = (int64_t)64 << 20; /* 64MB 护栏 */
    if (n != 0 && (int64_t)strlen(s) > kMaxBytes / n)
        sg_panic(fn, file, line, "字符串重复结果过长（上限 64MB）");
    size_t sl = strlen(s);
    char* p = sg_str_alloc(sl * (size_t)n + 1);
    for (int64_t k = 0; k < n; ++k) memcpy(p + sl * (size_t)k, s, sl);
    p[sl * (size_t)n] = 0;
    return p;
}

/* ---- str 别名登记与自拼接原地扩容（循环拼接 O(n²) live set 治理） ----
 * 契约（编译器 MLIRGen 侧同步维持，详见 docs/02 内存模型章）：
 *  1. str 缓冲泄漏语义不变（程序生命周期持有、零 free）；但"独占"缓冲允许
 *     经 sg_str_concat_grow 原地追加。一切使缓冲产生第二引用的操作必须先
 *     sg_str_mark 登记——登记后的指针永远走慢路径（整串新缓冲），别名侧
 *     内容永不变化，与 VM 值语义逐字一致。
 *  2. 第二引用的全部创建通道：槽位存入非独占值（storeTo/storeGlobal 发射
 *     lang.str_mark）、形参绑定（callee 入口发射）、容器捕获（list_new_s /
 *     dict_set / set_new / result_ok_s / result_err / 元组字面量）。
 *  3. 其余运行时函数不得滞留 str 实参（print/len/find 等只读）；str 方法
 *     族一律整串新缓冲返回，无内部指针通道。
 * 失误容错：登记遗漏只会让 grow 误走慢路径（行为仍正确），登记/快路径
 * 永不影响输出内容——性能优化与可观测行为解耦。 */
static char** sg_str_esc_set = NULL; /* 开放寻址哈希集（指针键，NULL=空槽） */
static size_t sg_str_esc_cap = 0;
static size_t sg_str_esc_len = 0;

static uint64_t sg_str_hash_ptr(const void* p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static int sg_str_escaped(const char* p) {
    if (!p || sg_str_esc_cap == 0) return 0;
    size_t mask = sg_str_esc_cap - 1;
    size_t i = (size_t)sg_str_hash_ptr(p) & mask;
    while (sg_str_esc_set[i]) {
        if (sg_str_esc_set[i] == p) return 1;
        i = (i + 1) & mask;
    }
    return 0;
}

static void sg_str_esc_put(const char* p) { /* 容量充足且未去重时调用 */
    size_t mask = sg_str_esc_cap - 1;
    size_t i = (size_t)sg_str_hash_ptr(p) & mask;
    while (sg_str_esc_set[i]) i = (i + 1) & mask;
    sg_str_esc_set[i] = (char*)p;
    sg_str_esc_len++;
}

void sg_str_mark(const char* p) {
    if (!p || sg_str_escaped(p)) return;
    if ((sg_str_esc_len + 1) * 4 > sg_str_esc_cap * 3) { /* 75% 负载 ×2 扩容 */
        size_t ncap = sg_str_esc_cap ? sg_str_esc_cap * 2 : 64;
        char** old = sg_str_esc_set;
        size_t oldcap = sg_str_esc_cap;
        sg_str_esc_set = (char**)calloc(ncap, sizeof(char*));
        if (!sg_str_esc_set) exit(1);
        sg_str_esc_cap = ncap;
        sg_str_esc_len = 0;
        for (size_t k = 0; k < oldcap; ++k)
            if (old[k]) sg_str_esc_put(old[k]);
        free(old);
    }
    sg_str_esc_put(p);
}

/* 累积器登记表：acc（编译期发射点编号）→ 当前缓冲/长度/容量。state[acc]
 * 只由本函数写出，故 a==state[acc].ptr 蕴含 len/cap 对该缓冲精确有效。 */
typedef struct {
    char* ptr;
    size_t len, cap;
} SgAcc;

static SgAcc* sg_accs = NULL;
static size_t sg_accs_n = 0;

const char* sg_str_concat_grow(int64_t acc, const char* a, const char* b) {
    size_t nb = strlen(b);
    if (acc >= 0 && (size_t)acc < sg_accs_n) {
        SgAcc* s = &sg_accs[acc];
        /* 快路径：a 是本发射点登记缓冲、容量足够且从未逃逸 → 原地追加。
         * 恒 memmove：b==a（s+=s 自拼接）时区间重叠。 */
        if (a == s->ptr && s->len + nb + 1 <= s->cap && !sg_str_escaped(a)) {
            memmove(s->ptr + s->len, b, nb + 1);
            s->len += nb;
            return s->ptr;
        }
    }
    /* 慢路径：内容与 sg_str_concat 逐字节同构；容量按 2 的幂预留（几何
     * 增长，后续追加落回快路径——bump 下不预留则每次拼接都整串重拷）。 */
    size_t na = strlen(a);
    size_t need = na + nb + 1;
    size_t cp = 32;
    while (cp < need) cp <<= 1;
    char* p = sg_str_alloc(cp);
    memcpy(p, a, na);
    memcpy(p + na, b, nb + 1);
    if (acc >= 0) {
        if ((size_t)acc >= sg_accs_n) {
            size_t n = sg_accs_n ? sg_accs_n : 8;
            while (n <= (size_t)acc) n *= 2;
            SgAcc* na2 = (SgAcc*)realloc(sg_accs, sizeof(SgAcc) * n);
            if (!na2) exit(1);
            sg_accs = na2;
            memset(sg_accs + sg_accs_n, 0,
                   sizeof(SgAcc) * (n - sg_accs_n));
            sg_accs_n = n;
        }
        sg_accs[acc].ptr = p;
        sg_accs[acc].len = na + nb;
        sg_accs[acc].cap = cp;
    }
    return p;
}

int64_t sg_str_cmp(const char* a, const char* b) {
    int c = strcmp(a, b);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

int sg_str_contains(const char* hay, const char* needle) {
    return strstr(hay, needle) != NULL;
}

int64_t sg_len_str(const char* s) { return (int64_t)strlen(s); }

/* str 单字节取字符（0.9.7：for 遍历 / 下标读共用）。负下标归一 + 越界
 * panic，消息与 VM normIndex 逐字一致（"字符串索引越界：<orig>（长度 <len>）"）。
 * P1 按字节索引（UTF-8 多字节字符返回单字节切片，非码点感知，文档化）。
 * 结果 malloc 持有（程序生命周期，与 str 方法返回同约定）。 */
const char* sg_str_chr_at(const char* s, int64_t idx, const char* fn,
                          const char* file, int64_t line) {
    int64_t len = (int64_t)strlen(s);
    int64_t orig = idx;
    if (idx < 0) idx += len; /* Python 语义：负下标从尾部计 */
    if (idx < 0 || idx >= len) {
        char buf[96];
        snprintf(buf, sizeof buf, "字符串索引越界：%lld（长度 %lld）",
                 (long long)orig, (long long)len);
        sg_panic(fn, file, line, buf);
    }
    char* p = (char*)malloc(2);
    if (!p) exit(1);
    p[0] = s[idx];
    p[1] = 0;
    return p;
}

/* ---- str() 构造 ---- */
const char* sg_str_from_i64(int64_t v) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    return sg_dup(tmp);
}

/* 最短往返浮点格式化（15→16→17 位逐级尝试；整数浮点补 ".0"）。
 * 静态缓冲仅供 print 系列立即消费；str() 经 sg_dup 复制返回。 */
static const char* sg_fmt_f64(double d) {
    static char buf[64];
    if (isnan(d)) return "nan";
    if (isinf(d)) return d > 0 ? "inf" : "-inf";
    for (int prec = 15; prec <= 17; ++prec) {
        snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (strtod(buf, NULL) == d) break;
    }
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E'))
        strcat(buf, ".0");
    return buf;
}

const char* sg_str_from_f64(double d) { return sg_dup(sg_fmt_f64(d)); }

const char* sg_str_from_bool(int b) { return b ? "True" : "False"; }

int64_t sg_abs_i64(int64_t v) {
    /* INT64_MIN 经无符号取负回包，与 VM 实践行为一致且无 UB */
    return v < 0 ? (int64_t)(0 - (uint64_t)v) : v;
}

/* int(f)：NaN/±inf/超 int64 域 panic（与 VM doBuiltin 逐字一致）；域内
 * 向零截断（C 转换即语义，(int64_t)v 对 inf 外已拦截无 UB）。 */
int64_t sg_int_from_f64(double v, const char* fn, const char* file,
                        int64_t line) {
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0))
        sg_panic(fn, file, line, "int() 浮点越界");
    return (int64_t)v;
}
double sg_abs_f64(double v) { return fabs(v); }
double sg_sqrt_f64(double v) { return sqrt(v); }
double sg_exp_f64(double v) { return exp(v); }
double sg_log_f64(double v) { return log(v); }

/* ---- v0.9.3 标量数学/随机（与 VM::doBuiltin 语义逐字一致） ----
 * 三角/双曲依 IEEE 754（asin(2) → nan），不 panic。 */
double sg_sin_f64(double v) { return sin(v); }
double sg_cos_f64(double v) { return cos(v); }
double sg_tan_f64(double v) { return tan(v); }
double sg_asin_f64(double v) { return asin(v); }
double sg_acos_f64(double v) { return acos(v); }
double sg_atan_f64(double v) { return atan(v); }
double sg_sinh_f64(double v) { return sinh(v); }
double sg_cosh_f64(double v) { return cosh(v); }
double sg_tanh_f64(double v) { return tanh(v); }
double sg_atan2_f64(double y, double x) { return atan2(y, x); }

/* 取整族公共尾：NaN/±inf/结果超 int64 范围 panic（消息与 VM panicHere
 * 逐字一致），不回绕。round = 半到偶（nearbyint，默认舍入模式）。 */
static int64_t sg_int_range(double r, const char* fn, const char* file,
                            int64_t line, const char* name) {
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s() 结果超出 int64 范围", name);
        sg_panic(fn, file, line, buf);
    }
    return (int64_t)r;
}
int64_t sg_floor_f64(double v, const char* fn, const char* file, int64_t line) {
    return sg_int_range(floor(v), fn, file, line, "floor");
}
int64_t sg_ceil_f64(double v, const char* fn, const char* file, int64_t line) {
    return sg_int_range(ceil(v), fn, file, line, "ceil");
}
int64_t sg_round_f64(double v, const char* fn, const char* file, int64_t line) {
    return sg_int_range(nearbyint(v), fn, file, line, "round");
}
int64_t sg_trunc_f64(double v, const char* fn, const char* file, int64_t line) {
    return sg_int_range(trunc(v), fn, file, line, "trunc");
}

/* ---- random/seed：splitmix64（与 VM::rngNext 逐位一致；默认状态 0，
 * 每次进程运行序列确定——确定性红线，禁用时间/地址熵）。 ---- */
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
#ifdef _WIN32
    /* 单调时钟秒数（QPC，与 MSVC steady_clock 同源） */
    static double inv = 0.0;
    LARGE_INTEGER f, t;
    if (inv == 0.0) {
        QueryPerformanceFrequency(&f);
        inv = 1.0 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * inv;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); /* 单调墙钟，与 QPC 同语义 */
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

/* ---- print 系列（stdout 通道；与 VM stringify 同格式） ---- */
void sg_print_i64(int64_t v) { printf("%lld", (long long)v); }
void sg_print_f64(double v) { fputs(sg_fmt_f64(v), stdout); }
void sg_print_bool(int b) { fputs(b ? "True" : "False", stdout); }
void sg_print_cstr(const char* s) { fputs(s, stdout); }
void sg_print_none(void) { fputs("None", stdout); }
void sg_print_space(void) { fputc(' ', stdout); }
void sg_print_nl(void) { fputc('\n', stdout); }

)RT";

} // namespace scourge
