// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 工具链 — build 命令实现：AOT 三阶段流水线驱动
// ----------------------------------------------------------------------------
//  Stage A（前端）复用 Session 加载 + 全量检查；Stage C 本文件：
//    generateMLIR → 写 .mlir → 进程内 MLIR→LLVM IR（AOT/InProcess，等价
//    原 mlir-opt --convert-to-llvm + mlir-translate 两步）→ clang 链接
//    内嵌 C 运行时。
//
//  C 运行时（kRuntimeC）与 MLIRGen 的 extern 声明逐一对应（34 张量/标量 +
//  21 Result/List + 3 try 蹦床），语义纪律
//  与 SG-BC VM（src/BC/VM.cpp）逐字对齐：
//    * 整数 add/sub/mul/幂/取负溢出、除零 → panic（消息与 VM 相同）；
//    * '/' 真除法恒 float；'//' Python 向下取整；'%' 与除数同号；
//    * 浮点 print/str 用最短往返表示（15→16→17 位逐级尝试），补 ".0"；
//    * Result 载荷按位编码 / List etag 0-3，方法语义与 VM CallMethod 一致；
//  * try/except 蹦床（sg_try_push/sg_try_pop + 生成代码直调 CRT _setjmp）
//      处理器栈与 VM catches_ 同构，sg_panic 单一钩子：栈顶有 handler 则
//      longjmp（与 VM recover 一致），否则打印 + exit(1)；
//    * panic 输出 "panic: <msg> at <fn> (<file>:<line>)" 到 stderr + exit 1，
//      与 Runner 的 VM panic 文本逐字节一致（构建产物与 scourge run 双模一致）。
// ============================================================================
#define _CRT_SECURE_NO_WARNINGS // std::getenv（MSVC 弃用告警，此处无并发安全诉求）

#include "scourge/Tool/Builder.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#include "scourge/AOT/InProcess.hpp"
#include "scourge/AOT/MLIRGen.hpp"
#include "scourge/BC/Serialize.hpp"
#include "scourge/Tool/Session.hpp"

namespace fs = std::filesystem;

namespace scourge {

namespace {

// ----------------------------------------------------------------------------
// 内嵌 C 运行时（build 时落盘为 <out>_rt.c，与 MLIR extern 签名一一对应）
// ----------------------------------------------------------------------------

const char kRuntimeC[] = R"RT(/* Scourge AOT 运行时（scourge build 内嵌生成；与 SG-BC VM 语义逐一对齐）。
 * 通道纪律：print 系列写 stdout，sg_panic 写 stderr 并 exit(1)。 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static char* sg_dup(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (!p) exit(1);
    memcpy(p, s, n + 1);
    return p;
}

const char* sg_str_concat(const char* a, const char* b) {
    size_t na = strlen(a), nb = strlen(b);
    char* p = (char*)malloc(na + nb + 1);
    if (!p) exit(1);
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
    char* p = (char*)malloc(sl * (size_t)n + 1);
    if (!p) exit(1);
    for (int64_t k = 0; k < n; ++k) memcpy(p + sl * (size_t)k, s, sl);
    p[sl * (size_t)n] = 0;
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
    return (double)clock() / (double)CLOCKS_PER_SEC;
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

/* ===========================================================================
 * ---- 张量运行时（SG-BC VM 的 TensorData 语义移植；panic 消息逐字对齐） ----
 * op 编码：0=+ 1=- 2=* 3=/ 4=// 5=% 6=**；数学函数编码：0=abs 1=sqrt 2=exp 3=log
 * 内存策略：程序生命周期持有（与字符串一致，不回收）。
 * ======================================================================== */
typedef struct {
    int64_t rank;
    int64_t* shape; /* malloc，长度 rank，行主序 */
    int isInt;      /* 1 = i64 元素，0 = f64 */
    int64_t* i64;   /* isInt 时有效 */
    double* f64;    /* !isInt 时有效 */
    void* dev;      /* GPU 设备副本（CUdeviceptr；非 0 = 设备驻留，GPU 构建专用） */
    int hostStale;  /* dev 非 0 时 1 = 设备较新（GPU 输出惰性回拷） */
} SgTensor;

/* ---- 设备缓冲同步钩子：GPU 构建由分派层（同 TU 后段 kGpuRuntimeC）在初始化
 * 成功后安装真实实现；其余构建恒空操作（dev 恒 0，零开销分支）。sg_tsync =
 * host 读入口回拷（设备较新时）；sg_tdirty = host 写入口整体失效设备副本。 ---- */
static void sg_tsync_nop(SgTensor* t) { (void)t; }
static void sg_tdirty_nop(SgTensor* t) { (void)t; }
static void (*sg_tsync_fn)(SgTensor*) = sg_tsync_nop;
static void (*sg_tdirty_fn)(SgTensor*) = sg_tdirty_nop;
static void sg_tsync(SgTensor* t) { sg_tsync_fn(t); }
static void sg_tdirty(SgTensor* t) { sg_tdirty_fn(t); }

static void* sg_xmalloc(size_t n) {
    void* p = malloc(n);
    if (!p) exit(1);
    return p;
}

static int64_t sg_tsize(const SgTensor* t) {
    int64_t n = 1;
    for (int64_t k = 0; k < t->rank; ++k) n *= t->shape[k];
    return n;
}

/* 形状的人类可读形式 "(2, 3)"（panic 消息用，与 VM shapeToString 一致）。
 * 缓冲区由调用方提供：同一 snprintf 传两次 sg_shape_str 时共享 static
 * 缓冲会相互覆盖（求值序未指定 → 两形状打成同一个），必须各自独立。 */
static const char* sg_shape_str(char* buf, size_t cap, const SgTensor* t) {
    char* p = buf;
    char* end = buf + cap;
    *p++ = '(';
    for (int64_t k = 0; k < t->rank && p < end - 24; ++k) {
        if (k) {
            *p++ = ',';
            *p++ = ' ';
        }
        p += snprintf(p, (size_t)(end - p), "%lld", (long long)t->shape[k]);
    }
    *p++ = ')';
    *p = 0;
    return buf;
}

static double sg_tget_f64(const SgTensor* t, int64_t k) {
    return t->isInt ? (double)t->i64[k] : t->f64[k];
}

/* Python 风格负下标归一 + 越界 panic（消息与 VM normIndex 一致） */
static int64_t sg_tnorm(int64_t idx, int64_t len, const char* fn,
                        const char* file, int64_t line) {
    int64_t orig = idx;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) {
        char msg[128];
        snprintf(msg, sizeof msg, "%s索引越界：%lld（长度 %lld）", "张量",
                 (long long)orig, (long long)len);
        sg_panic(fn, file, line, msg);
    }
    return idx;
}

/* 扁平化下标（n <= rank）；越界检查同 VM tensorIndexRead/Store */
static int64_t sg_tflat(const SgTensor* t, int64_t n, const int64_t* idx,
                        const char* fn, const char* file, int64_t line) {
    int64_t flat = 0;
    for (int64_t k = 0; k < n; ++k) {
        int64_t i = sg_tnorm(idx[k], t->shape[k], fn, file, line);
        flat = flat * t->shape[k] + i;
    }
    return flat;
}

SgTensor* sg_tensor_from_i64(int64_t n, const int64_t* data) {
    SgTensor* t = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    t->rank = 1;
    t->shape = (int64_t*)sg_xmalloc(sizeof(int64_t));
    t->shape[0] = n;
    t->isInt = 1;
    t->i64 = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)(n > 0 ? n : 1));
    t->f64 = 0;
    t->dev = 0;
    t->hostStale = 0;
    for (int64_t k = 0; k < n; ++k) t->i64[k] = data[k];
    return t;
}

SgTensor* sg_tensor_from_f64(int64_t n, const double* data) {
    SgTensor* t = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    t->rank = 1;
    t->shape = (int64_t*)sg_xmalloc(sizeof(int64_t));
    t->shape[0] = n;
    t->isInt = 0;
    t->i64 = 0;
    t->f64 = (double*)sg_xmalloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
    t->dev = 0;
    t->hostStale = 0;
    for (int64_t k = 0; k < n; ++k) t->f64[k] = data[k];
    return t;
}

/* 零值槽占位：空一维 i64 张量（防野值；与 VM 槽位零值纪律一致） */
SgTensor* sg_tensor_empty(void) {
    SgTensor* t = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    t->rank = 1;
    t->shape = (int64_t*)sg_xmalloc(sizeof(int64_t));
    t->shape[0] = 0;
    t->isInt = 1;
    t->i64 = 0;
    t->f64 = 0;
    t->dev = 0;
    t->hostStale = 0;
    return t;
}

/* ---- LangToLinalg 桥（AOT 步骤 3：linalg 降级的零拷贝输出/数据直取） ---- */

/* 数据指针直取（按 isInt 分派 i64/f64 存储；LangToLinalg 输入桥用） */
void* sg_tensor_data(SgTensor* t) {
    sg_tsync(t); /* 设备驻留时先回拷（GPU 覆盖集输出喂 linalg 输入桥） */
    return t->isInt ? (void*)t->i64 : (void*)t->f64;
}

/* 静态形状未初始化分配（LangToLinalg 输出侧；arena 策略与 from_* 一致，
 * 数据由调用方接管填充） */
SgTensor* sg_tensor_alloc(int isInt, int64_t rank, const int64_t* shape) {
    SgTensor* t = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    int64_t n = 1;
    t->rank = rank;
    t->shape = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)(rank > 0 ? rank : 1));
    for (int64_t k = 0; k < rank; ++k) {
        t->shape[k] = shape[k];
        n *= shape[k];
    }
    t->isInt = isInt;
    t->dev = 0;
    t->hostStale = 0;
    if (isInt) {
        t->i64 = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)(n > 0 ? n : 1));
        t->f64 = 0;
    } else {
        t->f64 = (double*)sg_xmalloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
        t->i64 = 0;
    }
    return t;
}

/* 外部内存接管（LangToLinalg 输出侧：把 memref.alloc 分配的存储挂进句柄。
 * 该内存同样为 malloc 且程序生命周期持有（AOT 运行时不 free），与
 * sg_xmalloc 的 arena 纪律一致，无双重所有权问题） */
void sg_tensor_adopt(SgTensor* t, void* data) {
    sg_tdirty(t); /* 数据指针被替换：旧设备副本（如有）整体失效 */
    if (t->isInt)
        t->i64 = (int64_t*)data;
    else
        t->f64 = (double*)data;
}

/* 全张量行拼接升维：行形状一致校验（与 VM tensorNew 一致；
 * VM 只比形状不比 dtype，混合 dtype → 提升 f64） */
SgTensor* sg_tensor_concat(int64_t nrows, const SgTensor** rows,
                           const char* fn, const char* file, int64_t line) {
    const SgTensor* r0 = rows[0];
    for (int64_t k = 1; k < nrows; ++k) {
        const SgTensor* r = rows[k];
        int mismatch = r->rank != r0->rank;
        for (int64_t d = 0; !mismatch && d < r0->rank; ++d)
            if (r->shape[d] != r0->shape[d]) mismatch = 1;
        if (mismatch) {
            char msg[512], sa[256], sb[256];
            snprintf(msg, sizeof msg, "NewTensor 行形状不一致：%s 与 %s",
                     sg_shape_str(sa, sizeof sa, r0),
                     sg_shape_str(sb, sizeof sb, r));
            sg_panic(fn, file, line, msg);
        }
    }
    int allInt = 1;
    for (int64_t k = 0; k < nrows; ++k) {
        sg_tsync((SgTensor*)rows[k]); /* 设备驻留行先回拷 */
        allInt &= rows[k]->isInt;
    }

    SgTensor* t = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    t->rank = r0->rank + 1;
    t->shape = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)t->rank);
    t->shape[0] = nrows;
    for (int64_t d = 0; d < r0->rank; ++d) t->shape[d + 1] = r0->shape[d];
    t->isInt = allInt;
    t->dev = 0;
    t->hostStale = 0;
    int64_t rowN = sg_tsize(r0);
    int64_t total = nrows * rowN;
    if (allInt) {
        t->i64 = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)total);
        t->f64 = 0;
        for (int64_t k = 0; k < nrows; ++k)
            memcpy(t->i64 + k * rowN, rows[k]->i64,
                   sizeof(int64_t) * (size_t)rowN);
    } else {
        t->f64 = (double*)sg_xmalloc(sizeof(double) * (size_t)total);
        t->i64 = 0;
        for (int64_t k = 0; k < nrows; ++k) {
            const SgTensor* r = rows[k];
            if (r->isInt) /* 混合 dtype → 提升 f64（对齐 unifyNumeric） */
                for (int64_t j = 0; j < rowN; ++j)
                    t->f64[k * rowN + j] = (double)r->i64[j];
            else
                memcpy(t->f64 + k * rowN, r->f64,
                       sizeof(double) * (size_t)rowN);
        }
    }
    return t;
}

/* 张量逐元素整数运算（溢出/除零 panic 消息与 VM tensorIntOp 一致） */
static int64_t sg_tiop(int op, int64_t x, int64_t y, const char* fn,
                       const char* file, int64_t line) {
    int64_t o = 0;
    switch (op) {
    case 0:
        if (__builtin_add_overflow(x, y, &o)) sg_panic(fn, file, line, "整数加法溢出");
        return o;
    case 1:
        if (__builtin_sub_overflow(x, y, &o)) sg_panic(fn, file, line, "整数减法溢出");
        return o;
    case 2:
        if (__builtin_mul_overflow(x, y, &o)) sg_panic(fn, file, line, "整数乘法溢出");
        return o;
    case 4:
        if (y == 0) sg_panic(fn, file, line, "整数整除零");
        if (x == INT64_MIN && y == -1) sg_panic(fn, file, line, "整数整除溢出");
        o = x / y;
        if (x % y != 0 && ((x < 0) != (y < 0))) --o;
        return o;
    case 5:
        if (y == 0) sg_panic(fn, file, line, "整数取模零");
        if (x == INT64_MIN && y == -1) return 0;
        o = x % y;
        if (o != 0 && ((o < 0) != (y < 0))) o += y;
        return o;
    default: { /* 6 = **（y < 0 与溢出同报"整数幂运算溢出"，对齐 VM） */
        if (y < 0) sg_panic(fn, file, line, "整数幂运算溢出");
        int64_t result = 1, base = x, exp = y;
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
    }
}

/* 张量逐元素浮点运算（除零/负次幂 panic 消息与 VM tensorF64Op 一致） */
static double sg_tfop(int op, double x, double y, const char* fn,
                      const char* file, int64_t line) {
    switch (op) {
    case 0: return x + y;
    case 1: return x - y;
    case 2: return x * y;
    case 3:
        if (y == 0.0) sg_panic(fn, file, line, "浮点除零");
        return x / y;
    case 4:
        if (y == 0.0) sg_panic(fn, file, line, "浮点整除零");
        return floor(x / y);
    case 5: {
        if (y == 0.0) sg_panic(fn, file, line, "浮点取模零");
        double m = fmod(x, y);
        if (m != 0.0 && ((m < 0.0) != (y < 0.0))) m += y;
        return m;
    }
    default:
        if (x == 0.0 && y < 0.0) sg_panic(fn, file, line, "零的负次幂");
        return pow(x, y);
    }
}

/* 通用逐元素：a 张量，b 可为张量（tb!=0）或标量（tb==0：si64=1 → i64 值 vi，
 * 否则 f64 值 vf；scalarLeft=1 表示标量在左操作数——减法/除法方向敏感）。
 * outInt = op != '/' && lInt && rInt（与 VM tensorBinOp 一致）。 */
static SgTensor* sg_tbinop(int op, const SgTensor* a, const SgTensor* tb,
                           int scalarLeft, int si64, int64_t vi, double vf,
                           const char* fn, const char* file, int64_t line) {
    int mismatch = 0;
    if (tb) {
        if (tb->rank != a->rank)
            mismatch = 1;
        else
            for (int64_t d = 0; d < a->rank; ++d)
                if (tb->shape[d] != a->shape[d]) {
                    mismatch = 1;
                    break;
                }
    }
    if (mismatch) {
        char msg[512], sa[256], sb[256];
        snprintf(msg, sizeof msg, "张量形状不兼容：%s 与 %s",
                 sg_shape_str(sa, sizeof sa, a), sg_shape_str(sb, sizeof sb, tb));
        sg_panic(fn, file, line, msg);
    }
    const int lInt = scalarLeft ? si64 : a->isInt;
    const int rInt = tb ? tb->isInt : (scalarLeft ? a->isInt : si64);
    const int outInt = (op != 3) && lInt && rInt;
    const int64_t n = sg_tsize(a);
    sg_tsync((SgTensor*)a); /* 设备驻留输入先回拷（GPU try 失败回退 CPU 时） */
    if (tb) sg_tsync((SgTensor*)tb);
    SgTensor* t = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    t->rank = a->rank;
    t->shape = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)a->rank);
    memcpy(t->shape, a->shape, sizeof(int64_t) * (size_t)a->rank);
    t->isInt = outInt;
    t->dev = 0;
    t->hostStale = 0;
    if (outInt) {
        /* outInt 时双侧必为整型（检查器已保证标量侧非 float） */
        t->i64 = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)n);
        t->f64 = 0;
        for (int64_t k = 0; k < n; ++k) {
            int64_t x, y;
            if (tb) {
                x = a->i64[k];
                y = tb->i64[k];
            } else if (scalarLeft) {
                x = vi;
                y = a->i64[k];
            } else {
                x = a->i64[k];
                y = vi;
            }
            t->i64[k] = sg_tiop(op, x, y, fn, file, line);
        }
    } else {
        t->f64 = (double*)sg_xmalloc(sizeof(double) * (size_t)n);
        t->i64 = 0;
        for (int64_t k = 0; k < n; ++k) {
            double x, y;
            if (tb) {
                x = sg_tget_f64(a, k);
                y = sg_tget_f64(tb, k);
            } else if (scalarLeft) {
                x = vf;
                y = sg_tget_f64(a, k);
            } else {
                x = sg_tget_f64(a, k);
                y = vf;
            }
            t->f64[k] = sg_tfop(op, x, y, fn, file, line);
        }
    }
    return t;
}

SgTensor* sg_tensor_binop_tt(int op, SgTensor* a, SgTensor* b,
                             const char* fn, const char* file, int64_t line) {
    return sg_tbinop(op, a, b, 0, 0, 0, 0.0, fn, file, line);
}

SgTensor* sg_tensor_binop_ts(int op, SgTensor* a, int si64, int64_t vi,
                             double vf, const char* fn, const char* file,
                             int64_t line) {
    return sg_tbinop(op, a, 0, 0, si64, vi, vf, fn, file, line);
}

SgTensor* sg_tensor_binop_st(int op, int si64, int64_t vi, double vf,
                             SgTensor* b, const char* fn, const char* file,
                             int64_t line) {
    /* 标量在左：结果形状取张量侧，x/y 方向按左标量右张量 */
    return sg_tbinop(op, b, 0, 1, si64, vi, vf, fn, file, line);
}

SgTensor* sg_tensor_matmul(SgTensor* a, SgTensor* b, const char* fn,
                           const char* file, int64_t line) {
    if (a->rank != 2 || b->rank != 2) {
        char msg[512], sa[256], sb[256];
        snprintf(msg, sizeof msg,
                 "matmul（@）要求两个二维张量，实际 %s 与 %s",
                 sg_shape_str(sa, sizeof sa, a), sg_shape_str(sb, sizeof sb, b));
        sg_panic(fn, file, line, msg);
    }
    if (a->shape[1] != b->shape[0]) {
        char msg[512], sa[256], sb[256];
        snprintf(msg, sizeof msg,
                 "matmul 维度不匹配：%s 与 %s（左列数 != 右行数）",
                 sg_shape_str(sa, sizeof sa, a), sg_shape_str(sb, sizeof sb, b));
        sg_panic(fn, file, line, msg);
    }
    const int64_t m = a->shape[0], k = a->shape[1], n = b->shape[1];
    sg_tsync(a); /* 设备驻留输入先回拷（GPU try 失败回退 CPU 时） */
    sg_tsync(b);
    SgTensor* t = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    t->rank = 2;
    t->shape = (int64_t*)sg_xmalloc(sizeof(int64_t) * 2);
    t->shape[0] = m;
    t->shape[1] = n;
    t->isInt = a->isInt && b->isInt;
    t->dev = 0;
    t->hostStale = 0;
    const int64_t count = m * n;
    if (t->isInt) {
        t->i64 = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)count);
        t->f64 = 0;
        /* N1 根治：i-k-j 行流式。原 i-j-p 对 B 按列跨行步进且三缓冲为
         * 不透明指针，clang -O2 因别名无法向量化（store-to-load 依赖链），
         * 大尺寸 AOT 慢于 VM。restrict 标注三缓冲互不别名；溢出逐轮检测，
         * 检测的乘/加组合集合与 VM (i,j,p) 顺序一致。 */
        const int64_t* __restrict aq = a->i64;
        const int64_t* __restrict bq = b->i64;
        int64_t* __restrict cq = t->i64;
        for (int64_t i = 0; i < m; ++i) {
            int64_t* crow = cq + i * n;
            for (int64_t j = 0; j < n; ++j) crow[j] = 0;
            for (int64_t p = 0; p < k; ++p) {
                const int64_t av = aq[i * k + p];
                const int64_t* brow = bq + p * n;
                for (int64_t j = 0; j < n; ++j) {
                    int64_t prod = 0;
                    if (__builtin_mul_overflow(av, brow[j], &prod) ||
                        __builtin_add_overflow(crow[j], prod, &crow[j]))
                        sg_panic(fn, file, line, "整数乘法溢出");
                }
            }
        }
    } else {
        t->f64 = (double*)sg_xmalloc(sizeof(double) * (size_t)count);
        t->i64 = 0;
        /* i-k-j 行流式（同 int 路径）：对每个 c[i][j] 的累加序仍按 p 递增
         * （0 + Σ a[i][p]*b[p][j]），与 VM i-j-k 逐位一致。mul 与 add 分离
         * 为两条语句禁 FP 收缩（FMA 单舍入会破坏双模逐位一致红线）。混合
         * dtype 时 int 侧经 (double) 转换（同 VM tensorAsF64 / sg_tget_f64）；
         * b 侧分支提出内层循环，保证热点循环体无分支可向量化。 */
        const int aInt = a->isInt, bInt = b->isInt;
        const double* __restrict ap = a->f64;
        const double* __restrict bp = b->f64;
        const int64_t* __restrict aq = a->i64;
        const int64_t* __restrict bq = b->i64;
        for (int64_t i = 0; i < m; ++i) {
            double* crow = t->f64 + i * n;
            for (int64_t j = 0; j < n; ++j) crow[j] = 0.0;
            for (int64_t p = 0; p < k; ++p) {
                const double av =
                    aInt ? (double)aq[i * k + p] : ap[i * k + p];
                if (bInt) {
                    const int64_t* brow = bq + p * n;
                    for (int64_t j = 0; j < n; ++j) {
                        const double prod = av * (double)brow[j];
                        crow[j] += prod;
                    }
                } else {
                    const double* brow = bp + p * n;
                    for (int64_t j = 0; j < n; ++j) {
                        const double prod = av * brow[j];
                        crow[j] += prod;
                    }
                }
            }
        }
    }
    return t;
}

int64_t sg_tensor_get_i(SgTensor* t, int64_t n, const int64_t* idx,
                        const char* fn, const char* file, int64_t line) {
    sg_tsync(t); /* 设备驻留先回拷（GPU 输出惰性回拷读入口） */
    return t->i64[sg_tflat(t, n, idx, fn, file, line)];
}

double sg_tensor_get_f(SgTensor* t, int64_t n, const int64_t* idx,
                       const char* fn, const char* file, int64_t line) {
    sg_tsync(t); /* 设备驻留先回拷（GPU 输出惰性回拷读入口） */
    return sg_tget_f64(t, sg_tflat(t, n, idx, fn, file, line));
}

/* 部分索引 → 子张量拷贝（行主序连续切片，与 VM tensorIndexRead 一致） */
SgTensor* sg_tensor_slice(SgTensor* t, int64_t n, const int64_t* idx,
                          const char* fn, const char* file, int64_t line) {
    sg_tsync(t); /* 设备驻留先回拷 */
    if (n > t->rank) {
        char msg[128];
        snprintf(msg, sizeof msg, "下标数量 %lld 超过张量秩 %lld",
                 (long long)n, (long long)t->rank);
        sg_panic(fn, file, line, msg);
    }
    int64_t flat = sg_tflat(t, n, idx, fn, file, line);
    int64_t rest = 1;
    for (int64_t k = n; k < t->rank; ++k) rest *= t->shape[k];
    SgTensor* s = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    s->rank = t->rank - n;
    s->shape = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)(s->rank > 0 ? s->rank : 1));
    for (int64_t k = 0; k < s->rank; ++k) s->shape[k] = t->shape[n + k];
    s->isInt = t->isInt;
    s->dev = 0;
    s->hostStale = 0;
    const int64_t start = flat * rest;
    if (t->isInt) {
        s->i64 = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)rest);
        s->f64 = 0;
        memcpy(s->i64, t->i64 + start, sizeof(int64_t) * (size_t)rest);
    } else {
        s->f64 = (double*)sg_xmalloc(sizeof(double) * (size_t)rest);
        s->i64 = 0;
        memcpy(s->f64, t->f64 + start, sizeof(double) * (size_t)rest);
    }
    return s;
}

void sg_tensor_set_i(SgTensor* t, int64_t n, const int64_t* idx, int64_t v,
                     const char* fn, const char* file, int64_t line) {
    if (n != t->rank) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "张量下标赋值需提供全部 %lld 维索引（当前 %lld 个）",
                 (long long)t->rank, (long long)n);
        sg_panic(fn, file, line, msg);
    }
    sg_tsync(t); /* GPU 输出惰性回拷：host 写入前先取回设备新数据（同 get） */
    t->i64[sg_tflat(t, n, idx, fn, file, line)] = v;
    sg_tdirty(t); /* host 写入：设备副本整体失效 */
}

void sg_tensor_set_f(SgTensor* t, int64_t n, const int64_t* idx, double v,
                     const char* fn, const char* file, int64_t line) {
    if (n != t->rank) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "张量下标赋值需提供全部 %lld 维索引（当前 %lld 个）",
                 (long long)t->rank, (long long)n);
        sg_panic(fn, file, line, msg);
    }
    sg_tsync(t); /* GPU 输出惰性回拷：host 写入前先取回设备新数据（同 get） */
    t->f64[sg_tflat(t, n, idx, fn, file, line)] = v;
    sg_tdirty(t); /* host 写入：设备副本整体失效 */
}

int64_t sg_tensor_sum_i(SgTensor* t, const char* fn, const char* file,
                        int64_t line) {
    int64_t acc = 0;
    sg_tsync(t); /* 设备驻留先回拷（GPU try 失败回退 CPU 时） */
    for (int64_t k = 0, n = sg_tsize(t); k < n; ++k) {
        int64_t out = 0;
        if (__builtin_add_overflow(acc, t->i64[k], &out))
            sg_panic(fn, file, line, "整数求和溢出");
        acc = out;
    }
    return acc;
}

double sg_tensor_sum_f(SgTensor* t) {
    double acc = 0.0;
    sg_tsync(t); /* 设备驻留先回拷（GPU try 失败回退 CPU 时） */
    for (int64_t k = 0, n = sg_tsize(t); k < n; ++k) acc += t->f64[k];
    return acc;
}

/* 逐元素数学：fnid 0=abs 1=sqrt 2=exp 3=log；abs 保 dtype，其余恒 f64 */
SgTensor* sg_tensor_un(int fnid, SgTensor* t, const char* fn,
                       const char* file, int64_t line) {
    const int64_t n = sg_tsize(t);
    sg_tsync(t); /* 设备驻留先回拷 */
    SgTensor* r = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    r->rank = t->rank;
    r->shape = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)t->rank);
    memcpy(r->shape, t->shape, sizeof(int64_t) * (size_t)t->rank);
    const int outInt = (fnid == 0) && t->isInt;
    r->isInt = outInt;
    r->dev = 0;
    r->hostStale = 0;
    if (outInt) {
        r->i64 = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)n);
        r->f64 = 0;
        for (int64_t k = 0; k < n; ++k) {
            int64_t x = t->i64[k];
            r->i64[k] = x < 0 ? -x : x;
        }
    } else {
        r->f64 = (double*)sg_xmalloc(sizeof(double) * (size_t)n);
        r->i64 = 0;
        for (int64_t k = 0; k < n; ++k) {
            double x = sg_tget_f64(t, k);
            r->f64[k] = fnid == 0 ? fabs(x)
                        : fnid == 1 ? sqrt(x)
                        : fnid == 2 ? exp(x)
                                    : log(x);
        }
    }
    return r;
}

/* 归约到标量：isMax 0=min 1=max；name 为 "max"/"min"（panic 消息对齐 VM） */
int64_t sg_tensor_reduce_i(int isMax, SgTensor* t, const char* name,
                           const char* fn, const char* file, int64_t line) {
    if (sg_tsize(t) == 0) {
        char msg[64];
        snprintf(msg, sizeof msg, "%s() 不支持空张量", name);
        sg_panic(fn, file, line, msg);
    }
    sg_tsync(t); /* 设备驻留先回拷 */
    int64_t best = t->i64[0];
    for (int64_t k = 1, n = sg_tsize(t); k < n; ++k) {
        int64_t x = t->i64[k];
        best = isMax ? (x > best ? x : best) : (x < best ? x : best);
    }
    return best;
}

double sg_tensor_reduce_f(int isMax, SgTensor* t, const char* name,
                          const char* fn, const char* file, int64_t line) {
    if (sg_tsize(t) == 0) {
        char msg[64];
        snprintf(msg, sizeof msg, "%s() 不支持空张量", name);
        sg_panic(fn, file, line, msg);
    }
    sg_tsync(t); /* 设备驻留先回拷 */
    double best = t->f64[0];
    for (int64_t k = 1, n = sg_tsize(t); k < n; ++k) {
        double x = t->f64[k];
        best = isMax ? (x > best ? x : best) : (x < best ? x : best);
    }
    return best;
}

SgTensor* sg_tensor_transpose(SgTensor* t, const char* fn, const char* file,
                              int64_t line) {
    if (t->rank != 2) {
        char msg[320], sa[256];
        snprintf(msg, sizeof msg, "transpose() 要求二维张量，实际 %s",
                 sg_shape_str(sa, sizeof sa, t));
        sg_panic(fn, file, line, msg);
    }
    const int64_t m = t->shape[0], c = t->shape[1];
    sg_tsync(t); /* 设备驻留先回拷（GPU try 失败回退 CPU 时） */
    SgTensor* r = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    r->rank = 2;
    r->shape = (int64_t*)sg_xmalloc(sizeof(int64_t) * 2);
    r->shape[0] = c;
    r->shape[1] = m;
    r->isInt = t->isInt;
    r->dev = 0;
    r->hostStale = 0;
    if (t->isInt) {
        r->i64 = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)(m * c));
        r->f64 = 0;
        for (int64_t i = 0; i < m; ++i)
            for (int64_t j = 0; j < c; ++j)
                r->i64[j * m + i] = t->i64[i * c + j];
    } else {
        r->f64 = (double*)sg_xmalloc(sizeof(double) * (size_t)(m * c));
        r->i64 = 0;
        for (int64_t i = 0; i < m; ++i)
            for (int64_t j = 0; j < c; ++j)
                r->f64[j * m + i] = t->f64[i * c + j];
    }
    return r;
}

/* 形状重组：数据顺序不变；元素总数校验与 VM 一致 */
SgTensor* sg_tensor_reshape(SgTensor* t, int64_t rank, const int64_t* dims,
                            const char* fn, const char* file, int64_t line) {
    int64_t prod = 1;
    for (int64_t k = 0; k < rank; ++k) prod *= dims[k];
    if (prod != sg_tsize(t)) {
        char sb[256], msg[512];
        snprintf(msg, sizeof msg,
                 "reshape() 元素总数不一致：%s（共 %lld 个）与目标形状（共 %lld "
                 "个）",
                 sg_shape_str(sb, sizeof sb, t), (long long)sg_tsize(t),
                 (long long)prod);
        sg_panic(fn, file, line, msg);
    }
    SgTensor* r = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    r->rank = rank;
    r->shape = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)rank);
    memcpy(r->shape, dims, sizeof(int64_t) * (size_t)rank);
    r->isInt = t->isInt;
    r->dev = 0;
    r->hostStale = 0;
    const int64_t n = sg_tsize(t);
    sg_tsync(t); /* 设备驻留先回拷 */
    if (t->isInt) {
        r->i64 = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)n);
        r->f64 = 0;
        memcpy(r->i64, t->i64, sizeof(int64_t) * (size_t)n);
    } else {
        r->f64 = (double*)sg_xmalloc(sizeof(double) * (size_t)n);
        r->i64 = 0;
        memcpy(r->f64, t->f64, sizeof(double) * (size_t)n);
    }
    return r;
}

/* 张量字符串化（print 与 str() 同格式，与 VM toString 的 Tensor 分支一致）：
 * "Tensor(2, 3) [[1.0, 2.0], [3.0, 4.0]]"；静态缓冲仅 print 立即消费。 */
static void sg_tfmt_rec(const SgTensor* t, int64_t dim, int64_t off,
                        char** pp) {
    char* p = *pp;
    *p++ = '[';
    if (dim + 1 == t->rank) {
        const int64_t n = t->shape[dim];
        for (int64_t k = 0; k < n; ++k) {
            if (k) {
                *p++ = ',';
                *p++ = ' ';
            }
            if (t->isInt)
                p += sprintf(p, "%lld", (long long)t->i64[off + k]);
            else
                p += sprintf(p, "%s", sg_fmt_f64(t->f64[off + k]));
        }
    } else {
        int64_t stride = 1;
        for (int64_t k = dim + 1; k < t->rank; ++k) stride *= t->shape[k];
        const int64_t n = t->shape[dim];
        for (int64_t k = 0; k < n; ++k) {
            if (k) {
                *p++ = ',';
                *p++ = ' ';
            }
            sg_tfmt_rec(t, dim + 1, off + k * stride, &p);
        }
    }
    *p++ = ']';
    *pp = p;
}

static const char* sg_tfmt(const SgTensor* t) {
    static char buf[1 << 20]; /* 1MB 静态缓冲；print 立即消费 */
    char sb[256];
    char* p = buf;
    sg_tsync((SgTensor*)t); /* 设备驻留先回拷（print/str 读入口） */
    p += sprintf(p, "Tensor%s ", sg_shape_str(sb, sizeof sb, t));
    sg_tfmt_rec(t, 0, 0, &p);
    *p = 0;
    return buf;
}

void sg_print_tensor(SgTensor* t) { fputs(sg_tfmt(t), stdout); }
const char* sg_str_from_tensor(SgTensor* t) { return sg_tfmt(t); }

/* ===========================================================================
 * ---- Result 运行时（错误即值；SgResult 堆句柄，程序生命周期持有） ----
 * 载荷按位编码：i64 原值 / f64 位型 memcpy / bool 0-1 / str 指针。
 * unwrap 携带缺省值：ok 取载荷、失败态取缺省（VM CallMethod 语义一致）。
 * ======================================================================== */
typedef struct {
    int64_t isOk;
    int64_t bits; /* OK 载荷按位存储 */
    const char* err;
} SgResult;

static SgResult* sg_result_new(int isOk) {
    SgResult* r = (SgResult*)malloc(sizeof(SgResult));
    if (!r) exit(1);
    r->isOk = isOk;
    r->bits = 0;
    r->err = "";
    return r;
}

SgResult* sg_result_ok_i(int64_t v) {
    SgResult* r = sg_result_new(1);
    r->bits = v;
    return r;
}
SgResult* sg_result_ok_f(double v) {
    SgResult* r = sg_result_new(1);
    memcpy(&r->bits, &v, 8);
    return r;
}
SgResult* sg_result_ok_b(int v) {
    SgResult* r = sg_result_new(1);
    r->bits = v ? 1 : 0;
    return r;
}
SgResult* sg_result_ok_s(const char* s) {
    SgResult* r = sg_result_new(1);
    r->bits = (int64_t)(intptr_t)s;
    return r;
}
SgResult* sg_result_ok_n(void) { return sg_result_new(1); }
SgResult* sg_result_err(const char* msg) {
    SgResult* r = sg_result_new(0);
    r->err = msg;
    return r;
}

int sg_result_is_ok(SgResult* r) { return r->isOk ? 1 : 0; }
int64_t sg_result_unwrap_i(SgResult* r, int64_t def) {
    return r->isOk ? r->bits : def;
}
double sg_result_unwrap_f(SgResult* r, double def) {
    if (!r->isOk) return def;
    double v;
    memcpy(&v, &r->bits, 8);
    return v;
}
int sg_result_unwrap_b(SgResult* r, int def) {
    return r->isOk ? (int)r->bits : def;
}
const char* sg_result_unwrap_s(SgResult* r, const char* def) {
    return r->isOk ? (const char*)(intptr_t)r->bits : def;
}

/* ===========================================================================
 * ---- List 运行时（SgList 堆句柄；etag 0=i64 1=f64 2=str 3=bool） ----
 * 下标读取复刻 VM normIndex：负下标从尾计，越界 panic 消息逐字一致。
 * ======================================================================== */
typedef struct {
    int64_t len;
    int etag;
    void* data; /* etag 0/3: int64_t*  1: double*  2: char** */
} SgList;

SgList* sg_list_empty(void) {
    SgList* l = (SgList*)malloc(sizeof(SgList));
    if (!l) exit(1);
    l->len = 0;
    l->etag = 0;
    l->data = NULL;
    return l;
}

SgList* sg_list_new_i(int64_t n, const int64_t* es) {
    SgList* l = sg_list_empty();
    l->len = n;
    l->etag = 0;
    l->data = malloc(sizeof(int64_t) * (size_t)(n > 0 ? n : 1));
    if (!l->data) exit(1);
    for (int64_t k = 0; k < n; ++k) ((int64_t*)l->data)[k] = es[k];
    return l;
}

SgList* sg_list_new_b(int64_t n, const int64_t* es) {
    SgList* l = sg_list_new_i(n, es);
    l->etag = 3;
    return l;
}

SgList* sg_list_new_f(int64_t n, const double* es) {
    SgList* l = sg_list_empty();
    l->len = n;
    l->etag = 1;
    l->data = malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
    if (!l->data) exit(1);
    for (int64_t k = 0; k < n; ++k) ((double*)l->data)[k] = es[k];
    return l;
}

SgList* sg_list_new_s(int64_t n, const char* const* es) {
    SgList* l = sg_list_empty();
    l->len = n;
    l->etag = 2;
    l->data = malloc(sizeof(char*) * (size_t)(n > 0 ? n : 1));
    if (!l->data) exit(1);
    for (int64_t k = 0; k < n; ++k) ((char**)l->data)[k] = (char*)es[k];
    return l;
}

int64_t sg_list_len(SgList* l) { return l->len; }

/* 负下标归一 + 越界 panic（与 VM normIndex(what="列表") 消息逐字一致）。 */
static int64_t sg_list_norm(SgList* l, int64_t idx, const char* fn,
                            const char* file, int64_t line) {
    int64_t orig = idx;
    if (idx < 0) idx += l->len;
    if (idx < 0 || idx >= l->len) {
        char buf[64];
        snprintf(buf, sizeof buf, "列表索引越界：%lld（长度 %lld）",
                 (long long)orig, (long long)l->len);
        sg_panic(fn, file, line, buf);
    }
    return idx;
}

int64_t sg_list_get_i(SgList* l, int64_t idx, const char* fn, const char* file,
                      int64_t line) {
    return ((int64_t*)l->data)[sg_list_norm(l, idx, fn, file, line)];
}
double sg_list_get_f(SgList* l, int64_t idx, const char* fn, const char* file,
                     int64_t line) {
    return ((double*)l->data)[sg_list_norm(l, idx, fn, file, line)];
}
int sg_list_get_b(SgList* l, int64_t idx, const char* fn, const char* file,
                  int64_t line) {
    return ((int64_t*)l->data)[sg_list_norm(l, idx, fn, file, line)] ? 1 : 0;
}
const char* sg_list_get_s(SgList* l, int64_t idx, const char* fn,
                          const char* file, int64_t line) {
    return ((char**)l->data)[sg_list_norm(l, idx, fn, file, line)];
}

/* ---- 成员判定（in）：按 etag 线性扫描；键类型与元素类型经检查器保证一致
 * （int 键对浮点列表已在 MLIRGen 提升为 f64），故不做跨类型比较。 ---- */
int sg_list_in_i(SgList* l, int64_t key) {
    for (int64_t k = 0; k < l->len; ++k)
        if (((int64_t*)l->data)[k] == key) return 1;
    return 0;
}
int sg_list_in_f(SgList* l, double key) {
    for (int64_t k = 0; k < l->len; ++k)
        if (((double*)l->data)[k] == key) return 1;
    return 0;
}
int sg_list_in_b(SgList* l, int key) {
    for (int64_t k = 0; k < l->len; ++k)
        if ((((int64_t*)l->data)[k] ? 1 : 0) == (key ? 1 : 0)) return 1;
    return 0;
}
int sg_list_in_s(SgList* l, const char* key) {
    for (int64_t k = 0; k < l->len; ++k)
        if (strcmp(((char**)l->data)[k], key) == 0) return 1;
    return 0;
}

/* ---- 字典（0.9.3）：插入序哈希表（链地址 + 插入序数组，迭代序恒为
 * 插入序，与 VM DictData 语义一致）。ktag/vtag：0=i64 1=f64 2=str 3=bool；
 * 键/值统一 64 位槽 ABI（int 直通、float 位模式、bool 零扩展、str 指针）。
 * 哈希与 VM dictHashKey 同构：数值键按 double 位模式（-0.0 归一）、str
 * FNV-1a、splitmix64 终混；哈希仅决定桶分布，观察行为只取决于插入序。
 * 键类型由检查器统一（int 键对 float 字典已在 MLIRGen 提升为 f64 位），
 * 故相等判断按槽位语义精确比较，无跨类型分支。 ---- */
typedef struct { uint64_t k, v, h; int32_t next; } SgDictEnt;
typedef struct {
    int64_t len, bcap;     /* bcap 恒为 2 的幂 */
    int32_t ktag, vtag;
    int64_t ec_cap;
    SgDictEnt* ents;       /* 插入序 */
    int32_t* buckets;      /* 桶头 = ents 下标 + 1（0 = 空桶） */
} SgDict;

static uint64_t sg_dict_mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static uint64_t sg_dict_hash(const SgDict* d, uint64_t k) {
    if (d->ktag == 1) { /* f64：按 double 位模式（-0.0 与 0.0 同键） */
        double dv;
        uint64_t bits;
        memcpy(&dv, &k, sizeof dv);
        if (dv == 0.0) dv = 0.0;
        memcpy(&bits, &dv, sizeof bits);
        return sg_dict_mix64(bits);
    }
    if (d->ktag == 2) { /* str：FNV-1a */
        uint64_t h = 14695981039346656037ull;
        for (const unsigned char* c = (const unsigned char*)(size_t)k; *c; ++c) {
            h ^= *c;
            h *= 1099511628211ull;
        }
        return sg_dict_mix64(h);
    }
    return sg_dict_mix64(k); /* i64/bool：按槽位混，等值判断精确 */
}

static int sg_dict_eq(const SgDict* d, uint64_t a, uint64_t b) {
    if (d->ktag == 2)
        return strcmp((const char*)(size_t)a, (const char*)(size_t)b) == 0;
    if (d->ktag == 1) {
        double da, db;
        memcpy(&da, &a, sizeof da);
        memcpy(&db, &b, sizeof db);
        return da == db;
    }
    return a == b;
}

static void sg_dict_check_key(const SgDict* d, uint64_t k, const char* fn,
                              const char* file, int64_t line) {
    if (d->ktag == 1) {
        double dv;
        memcpy(&dv, &k, sizeof dv);
        if (dv != dv) sg_panic(fn, file, line, "字典键不能为 NaN");
    }
}

static int64_t sg_dict_find(const SgDict* d, uint64_t k) {
    uint64_t h = sg_dict_hash(d, k);
    for (int32_t e = d->buckets[h % (uint64_t)d->bcap]; e != 0;
         e = d->ents[e - 1].next)
        if (d->ents[e - 1].h == h && sg_dict_eq(d, d->ents[e - 1].k, k))
            return e - 1;
    return -1;
}

/* 装载因子 > 3/4 扩容（×2 重链；arena 纪律：旧块经 realloc 交还系统） */
static void sg_dict_grow(SgDict* d) {
    int64_t ncap = d->bcap * 2;
    d->buckets = (int32_t*)realloc(d->buckets, sizeof(int32_t) * (size_t)ncap);
    if (!d->buckets) exit(1);
    for (int64_t i = 0; i < ncap; ++i) d->buckets[i] = 0;
    for (int64_t i = 0; i < d->len; ++i) {
        uint64_t b = d->ents[i].h % (uint64_t)ncap;
        d->ents[i].next = d->buckets[b];
        d->buckets[b] = (int32_t)(i + 1);
    }
    d->bcap = ncap;
}

void sg_dict_set(SgDict* d, uint64_t k, uint64_t v, const char* fn,
                 const char* file, int64_t line);

SgDict* sg_dict_new(int32_t ktag, int32_t vtag, int64_t n,
                    const int64_t* keys, const int64_t* vals,
                    const char* fn, const char* file, int64_t line) {
    SgDict* d = (SgDict*)sg_xmalloc(sizeof(SgDict));
    d->len = 0;
    d->bcap = 8;
    d->ktag = ktag;
    d->vtag = vtag;
    d->ec_cap = n > 4 ? n : 4;
    d->ents = (SgDictEnt*)sg_xmalloc(sizeof(SgDictEnt) * (size_t)d->ec_cap);
    d->buckets = (int32_t*)sg_xmalloc(sizeof(int32_t) * (size_t)d->bcap);
    for (int64_t i = 0; i < d->bcap; ++i) d->buckets[i] = 0;
    for (int64_t i = 0; i < n; ++i)
        sg_dict_set(d, (uint64_t)keys[i], (uint64_t)vals[i], fn, file, line);
    return d;
}

int64_t sg_dict_len(SgDict* d) { return d->len; }

uint64_t sg_dict_get(SgDict* d, uint64_t k, const char* fn, const char* file,
                     int64_t line) {
    int64_t e = sg_dict_find(d, k);
    if (e < 0) sg_panic(fn, file, line, "字典键不存在");
    return d->ents[e].v;
}

void sg_dict_set(SgDict* d, uint64_t k, uint64_t v, const char* fn,
                 const char* file, int64_t line) {
    sg_dict_check_key(d, k, fn, file, line);
    int64_t e = sg_dict_find(d, k);
    if (e >= 0) {
        d->ents[e].v = v;
        return;
    }
    if ((d->len + 1) * 4 > d->bcap * 3) sg_dict_grow(d);
    if (d->len == d->ec_cap) {
        d->ec_cap *= 2;
        d->ents = (SgDictEnt*)realloc(d->ents,
                                      sizeof(SgDictEnt) * (size_t)d->ec_cap);
        if (!d->ents) exit(1);
    }
    uint64_t h = sg_dict_hash(d, k);
    uint64_t b = h % (uint64_t)d->bcap;
    SgDictEnt* ent = &d->ents[d->len];
    ent->k = k;
    ent->v = v;
    ent->h = h;
    ent->next = d->buckets[b];
    d->buckets[b] = (int32_t)(d->len + 1);
    d->len++;
}

int sg_dict_in(SgDict* d, uint64_t k) { return sg_dict_find(d, k) >= 0; }

uint64_t sg_dict_key_at(SgDict* d, int64_t idx, const char* fn,
                        const char* file, int64_t line) {
    if (idx < 0 || idx >= d->len)
        sg_panic(fn, file, line, "字典迭代下标越界");
    return d->ents[idx].k;
}

/* ---- sorted（v0.9.3）：按 etag 稳定升序，返回新列表。
 * 稳定性经 (值, 原下标) 装饰 + qsort 实现（qsort 不稳定，下标决胜）。
 * NaN 显式拒绝：NaN 不可比破坏全序，panic 而非静默（与 VM 同语义同消息）。
 * etag 0=i64 1=f64 2=str 3=bool（bool 借道 i64 路径，false<true）。 ---- */
typedef struct { int64_t key; int64_t idx; } SgSortI64;
typedef struct { double key; int64_t idx; } SgSortF64;
typedef struct { const char* key; int64_t idx; } SgSortStr;

static int sg_sort_i64(const void* x, const void* y) {
    const SgSortI64* a = (const SgSortI64*)x;
    const SgSortI64* b = (const SgSortI64*)y;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->idx < b->idx ? -1 : 1;
}
static int sg_sort_f64(const void* x, const void* y) {
    const SgSortF64* a = (const SgSortF64*)x;
    const SgSortF64* b = (const SgSortF64*)y;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->idx < b->idx ? -1 : 1;
}
static int sg_sort_str(const void* x, const void* y) {
    const SgSortStr* a = (const SgSortStr*)x;
    const SgSortStr* b = (const SgSortStr*)y;
    int c = strcmp(a->key, b->key);
    if (c != 0) return c;
    return a->idx < b->idx ? -1 : 1;
}

SgList* sg_sorted(SgList* l, const char* fn, const char* file, int64_t line) {
    SgList* out = sg_list_empty();
    out->len = l->len;
    out->etag = l->etag;
    int64_t n = l->len;
    if (n <= 0) return out;
    if (l->etag == 0 || l->etag == 3) {
        SgSortI64* tmp = (SgSortI64*)malloc(sizeof(SgSortI64) * (size_t)n);
        if (!tmp) exit(1);
        for (int64_t k = 0; k < n; ++k) {
            tmp[k].key = ((const int64_t*)l->data)[k];
            tmp[k].idx = k;
        }
        qsort(tmp, (size_t)n, sizeof(SgSortI64), sg_sort_i64);
        out->data = malloc(sizeof(int64_t) * (size_t)n);
        if (!out->data) exit(1);
        for (int64_t k = 0; k < n; ++k) ((int64_t*)out->data)[k] = tmp[k].key;
        free(tmp);
        return out;
    }
    if (l->etag == 1) {
        const double* a = (const double*)l->data;
        for (int64_t k = 0; k < n; ++k)
            if (isnan(a[k]))
                sg_panic(fn, file, line, "sorted() 不支持含 NaN 的浮点元素");
        SgSortF64* tmp = (SgSortF64*)malloc(sizeof(SgSortF64) * (size_t)n);
        if (!tmp) exit(1);
        for (int64_t k = 0; k < n; ++k) {
            tmp[k].key = a[k];
            tmp[k].idx = k;
        }
        qsort(tmp, (size_t)n, sizeof(SgSortF64), sg_sort_f64);
        out->data = malloc(sizeof(double) * (size_t)n);
        if (!out->data) exit(1);
        for (int64_t k = 0; k < n; ++k) ((double*)out->data)[k] = tmp[k].key;
        free(tmp);
        return out;
    }
    /* etag 2：str 按字节序（与 sg_str_cmp / VM str 比较一致）。 */
    SgSortStr* tmp = (SgSortStr*)malloc(sizeof(SgSortStr) * (size_t)n);
    if (!tmp) exit(1);
    for (int64_t k = 0; k < n; ++k) {
        tmp[k].key = ((const char* const*)l->data)[k];
        tmp[k].idx = k;
    }
    qsort(tmp, (size_t)n, sizeof(SgSortStr), sg_sort_str);
    out->data = malloc(sizeof(char*) * (size_t)n);
    if (!out->data) exit(1);
    for (int64_t k = 0; k < n; ++k) ((char**)out->data)[k] = (char*)tmp[k].key;
    free(tmp);
    return out;
}
)RT";

// ----------------------------------------------------------------------------
// CUDA 第 1 层：GPU kernel PTX 与分派运行时（--gpu 时追加写入 _rt.c）
// ----------------------------------------------------------------------------
// PTX 由 verify/cuda/sg_kernels.ll 经 D:\LLVM\bin\llc
// (-march=nvptx64 -mcpu=sm_120) 生成——八个形状参数化 kernel：
//   sg_ew_tt / sg_ew_ts（f64 逐元素 + - *，ts/st 经 sleft 选择子方向）、
//   sg_ew_ti / sg_ew_tsi（int 逐元素 + - *，sadd/ssub/smul.with.overflow
//   溢出仿真 + atomicrmw or 状态上报）、
//   sg_mm（matmul shared-memory tiling 32×32，p 串行升序累加）、
//   sg_sum（f64 单线程升序归约）、
//   sg_sum_i（int 归约，同款溢出仿真）、sg_tr（2D 转置）。
// 确定性红线：kernel 浮点/整数运算顺序与 CPU 路径逐位一致（add.rn.f64
// IEEE RN，mm/sum 串行升序累加，int 溢出判定与 __builtin_*_overflow 同一
// intrinsic），三模输出逐字节可比；sm_120 之外机型 JIT 失败 →
// 运行时自动回退 CPU（语义一致，仅性能降级）。
const char kGpuPtxC[] = R"PTX(//
// Generated by LLVM NVPTX Back-End
//

.version 8.7
.target sm_120
.address_size 64

	// .globl	sg_ew_tt                // -- Begin function sg_ew_tt
// sg_mm_at has been demoted
// sg_mm_bt has been demoted
                                        // @sg_ew_tt
.visible .entry sg_ew_tt(
	.param .u64 .ptr .align 1 sg_ew_tt_param_0,
	.param .u64 .ptr .align 1 sg_ew_tt_param_1,
	.param .u64 .ptr .align 1 sg_ew_tt_param_2,
	.param .u64 sg_ew_tt_param_3,
	.param .u32 sg_ew_tt_param_4
)
{
	.reg .pred 	%p<5>;
	.reg .b32 	%r<5>;
	.reg .b64 	%rd<18>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd7, [sg_ew_tt_param_0];
	ld.param.b64 	%rd8, [sg_ew_tt_param_1];
	cvta.to.global.u64 	%rd1, %rd8;
	ld.param.b64 	%rd9, [sg_ew_tt_param_2];
	cvta.to.global.u64 	%rd2, %rd9;
	cvta.to.global.u64 	%rd3, %rd7;
	ld.param.b64 	%rd10, [sg_ew_tt_param_3];
	mov.u32 	%r2, %tid.x;
	mov.u32 	%r3, %ntid.x;
	mov.u32 	%r4, %ctaid.x;
	cvt.u64.u32 	%rd11, %r2;
	mul.wide.u32 	%rd12, %r4, %r3;
	add.s64 	%rd4, %rd12, %rd11;
	setp.ge.s64 	%p1, %rd4, %rd10;
	@%p1 bra 	$L__BB0_8;
// %bb.1:                               // %do
	ld.param.b32 	%r1, [sg_ew_tt_param_4];
	shl.b64 	%rd13, %rd4, 3;
	add.s64 	%rd14, %rd1, %rd13;
	add.s64 	%rd15, %rd2, %rd13;
	ld.global.b64 	%rd5, [%rd14];
	ld.global.b64 	%rd6, [%rd15];
	setp.eq.b32 	%p2, %r1, 2;
	@%p2 bra 	$L__BB0_6;
// %bb.2:                               // %do
	setp.eq.b32 	%p3, %r1, 1;
	@%p3 bra 	$L__BB0_5;
// %bb.3:                               // %do
	setp.ne.b32 	%p4, %r1, 0;
	@%p4 bra 	$L__BB0_8;
// %bb.4:                               // %add
	add.rn.f64 	%rd17, %rd5, %rd6;
	bra.uni 	$L__BB0_7;
$L__BB0_5:                              // %sub
	sub.rn.f64 	%rd17, %rd5, %rd6;
	bra.uni 	$L__BB0_7;
$L__BB0_6:                              // %mul
	mul.rn.f64 	%rd17, %rd5, %rd6;
$L__BB0_7:                              // %wr
	add.s64 	%rd16, %rd3, %rd13;
	st.global.b64 	[%rd16], %rd17;
$L__BB0_8:                              // %exit
	ret;
                                        // -- End function
}
	// .globl	sg_ew_ts                // -- Begin function sg_ew_ts
.visible .entry sg_ew_ts(
	.param .u64 .ptr .align 1 sg_ew_ts_param_0,
	.param .u64 .ptr .align 1 sg_ew_ts_param_1,
	.param .f64 sg_ew_ts_param_2,
	.param .u64 sg_ew_ts_param_3,
	.param .u32 sg_ew_ts_param_4,
	.param .u32 sg_ew_ts_param_5
)                                       // @sg_ew_ts
{
	.reg .pred 	%p<6>;
	.reg .b32 	%r<6>;
	.reg .b64 	%rd<17>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd7, [sg_ew_ts_param_0];
	ld.param.b64 	%rd8, [sg_ew_ts_param_1];
	cvta.to.global.u64 	%rd1, %rd8;
	cvta.to.global.u64 	%rd2, %rd7;
	ld.param.b64 	%rd9, [sg_ew_ts_param_3];
	mov.u32 	%r3, %tid.x;
	mov.u32 	%r4, %ntid.x;
	mov.u32 	%r5, %ctaid.x;
	cvt.u64.u32 	%rd10, %r3;
	mul.wide.u32 	%rd11, %r5, %r4;
	add.s64 	%rd3, %rd11, %rd10;
	setp.ge.s64 	%p1, %rd3, %rd9;
	@%p1 bra 	$L__BB1_8;
// %bb.1:                               // %do
	ld.param.b32 	%r2, [sg_ew_ts_param_5];
	ld.param.b32 	%r1, [sg_ew_ts_param_4];
	ld.param.b64 	%rd6, [sg_ew_ts_param_2];
	shl.b64 	%rd12, %rd3, 3;
	add.s64 	%rd13, %rd1, %rd12;
	ld.global.b64 	%rd14, [%rd13];
	setp.ne.b32 	%p2, %r2, 0;
	selp.f64 	%rd4, %rd6, %rd14, %p2;
	selp.f64 	%rd5, %rd14, %rd6, %p2;
	setp.eq.b32 	%p3, %r1, 2;
	@%p3 bra 	$L__BB1_6;
// %bb.2:                               // %do
	setp.eq.b32 	%p4, %r1, 1;
	@%p4 bra 	$L__BB1_5;
// %bb.3:                               // %do
	setp.ne.b32 	%p5, %r1, 0;
	@%p5 bra 	$L__BB1_8;
// %bb.4:                               // %add
	add.rn.f64 	%rd16, %rd4, %rd5;
	bra.uni 	$L__BB1_7;
$L__BB1_5:                              // %sub
	sub.rn.f64 	%rd16, %rd4, %rd5;
	bra.uni 	$L__BB1_7;
$L__BB1_6:                              // %mul
	mul.rn.f64 	%rd16, %rd4, %rd5;
$L__BB1_7:                              // %wr
	add.s64 	%rd15, %rd2, %rd12;
	st.global.b64 	[%rd15], %rd16;
$L__BB1_8:                              // %exit
	ret;
                                        // -- End function
}
	// .globl	sg_ew_ti                // -- Begin function sg_ew_ti
.visible .entry sg_ew_ti(
	.param .u64 .ptr .align 1 sg_ew_ti_param_0,
	.param .u64 .ptr .align 1 sg_ew_ti_param_1,
	.param .u64 .ptr .align 1 sg_ew_ti_param_2,
	.param .u64 sg_ew_ti_param_3,
	.param .u32 sg_ew_ti_param_4,
	.param .u64 .ptr .align 1 sg_ew_ti_param_5
)                                       // @sg_ew_ti
{
	.reg .pred 	%p<11>;
	.reg .b32 	%r<6>;
	.reg .b64 	%rd<22>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd8, [sg_ew_ti_param_0];
	ld.param.b64 	%rd9, [sg_ew_ti_param_1];
	cvta.to.global.u64 	%rd1, %rd9;
	ld.param.b64 	%rd10, [sg_ew_ti_param_2];
	cvta.to.global.u64 	%rd2, %rd10;
	ld.param.b64 	%rd11, [sg_ew_ti_param_5];
	cvta.to.global.u64 	%rd3, %rd11;
	ld.param.b64 	%rd12, [sg_ew_ti_param_3];
	cvta.to.global.u64 	%rd4, %rd8;
	mov.u32 	%r2, %tid.x;
	mov.u32 	%r3, %ntid.x;
	mov.u32 	%r4, %ctaid.x;
	cvt.u64.u32 	%rd13, %r2;
	mul.wide.u32 	%rd14, %r4, %r3;
	add.s64 	%rd5, %rd14, %rd13;
	setp.ge.s64 	%p1, %rd5, %rd12;
	@%p1 bra 	$L__BB2_10;
// %bb.1:                               // %do
	ld.param.b32 	%r1, [sg_ew_ti_param_4];
	shl.b64 	%rd15, %rd5, 3;
	add.s64 	%rd16, %rd1, %rd15;
	add.s64 	%rd17, %rd2, %rd15;
	ld.global.b64 	%rd6, [%rd16];
	ld.global.b64 	%rd7, [%rd17];
	setp.eq.b32 	%p2, %r1, 2;
	@%p2 bra 	$L__BB2_6;
// %bb.2:                               // %do
	setp.eq.b32 	%p3, %r1, 1;
	@%p3 bra 	$L__BB2_5;
// %bb.3:                               // %do
	setp.ne.b32 	%p4, %r1, 0;
	@%p4 bra 	$L__BB2_10;
// %bb.4:                               // %add
	add.s64 	%rd21, %rd6, %rd7;
	setp.lt.s64 	%p7, %rd21, %rd6;
	setp.lt.s64 	%p8, %rd7, 0;
	xor.pred 	%p10, %p8, %p7;
	bra.uni 	$L__BB2_7;
$L__BB2_5:                              // %sub
	setp.lt.s64 	%p5, %rd6, %rd7;
	sub.s64 	%rd21, %rd6, %rd7;
	setp.lt.s64 	%p6, %rd21, 0;
	xor.pred 	%p10, %p5, %p6;
	bra.uni 	$L__BB2_7;
$L__BB2_6:                              // %mul
	mul.hi.s64 	%rd18, %rd6, %rd7;
	mul.lo.s64 	%rd21, %rd6, %rd7;
	shr.s64 	%rd19, %rd21, 63;
	setp.ne.b64 	%p10, %rd18, %rd19;
$L__BB2_7:                              // %chk
	not.pred 	%p9, %p10;
	@%p9 bra 	$L__BB2_9;
// %bb.8:                               // %ovf
	atom.relaxed.sys.global.or.b32 	%r5, [%rd3], 1;
$L__BB2_9:                              // %wr
	add.s64 	%rd20, %rd4, %rd15;
	st.global.b64 	[%rd20], %rd21;
$L__BB2_10:                             // %exit
	ret;
                                        // -- End function
}
	// .globl	sg_ew_tsi               // -- Begin function sg_ew_tsi
.visible .entry sg_ew_tsi(
	.param .u64 .ptr .align 1 sg_ew_tsi_param_0,
	.param .u64 .ptr .align 1 sg_ew_tsi_param_1,
	.param .u64 sg_ew_tsi_param_2,
	.param .u64 sg_ew_tsi_param_3,
	.param .u32 sg_ew_tsi_param_4,
	.param .u32 sg_ew_tsi_param_5,
	.param .u64 .ptr .align 1 sg_ew_tsi_param_6
)                                       // @sg_ew_tsi
{
	.reg .pred 	%p<12>;
	.reg .b32 	%r<7>;
	.reg .b64 	%rd<21>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd8, [sg_ew_tsi_param_0];
	ld.param.b64 	%rd9, [sg_ew_tsi_param_1];
	cvta.to.global.u64 	%rd1, %rd9;
	ld.param.b64 	%rd10, [sg_ew_tsi_param_6];
	cvta.to.global.u64 	%rd2, %rd10;
	cvta.to.global.u64 	%rd3, %rd8;
	ld.param.b64 	%rd11, [sg_ew_tsi_param_3];
	mov.u32 	%r3, %tid.x;
	mov.u32 	%r4, %ntid.x;
	mov.u32 	%r5, %ctaid.x;
	cvt.u64.u32 	%rd12, %r3;
	mul.wide.u32 	%rd13, %r5, %r4;
	add.s64 	%rd4, %rd13, %rd12;
	setp.ge.s64 	%p1, %rd4, %rd11;
	@%p1 bra 	$L__BB3_10;
// %bb.1:                               // %do
	ld.param.b32 	%r2, [sg_ew_tsi_param_5];
	ld.param.b32 	%r1, [sg_ew_tsi_param_4];
	ld.param.b64 	%rd7, [sg_ew_tsi_param_2];
	shl.b64 	%rd14, %rd4, 3;
	add.s64 	%rd15, %rd1, %rd14;
	ld.global.b64 	%rd16, [%rd15];
	setp.ne.b32 	%p2, %r2, 0;
	selp.b64 	%rd5, %rd7, %rd16, %p2;
	selp.b64 	%rd6, %rd16, %rd7, %p2;
	setp.eq.b32 	%p3, %r1, 2;
	@%p3 bra 	$L__BB3_6;
// %bb.2:                               // %do
	setp.eq.b32 	%p4, %r1, 1;
	@%p4 bra 	$L__BB3_5;
// %bb.3:                               // %do
	setp.ne.b32 	%p5, %r1, 0;
	@%p5 bra 	$L__BB3_10;
// %bb.4:                               // %add
	add.s64 	%rd20, %rd5, %rd6;
	setp.lt.s64 	%p8, %rd20, %rd5;
	setp.lt.s64 	%p9, %rd6, 0;
	xor.pred 	%p11, %p9, %p8;
	bra.uni 	$L__BB3_7;
$L__BB3_5:                              // %sub
	setp.lt.s64 	%p6, %rd5, %rd6;
	sub.s64 	%rd20, %rd5, %rd6;
	setp.lt.s64 	%p7, %rd20, 0;
	xor.pred 	%p11, %p6, %p7;
	bra.uni 	$L__BB3_7;
$L__BB3_6:                              // %mul
	mul.hi.s64 	%rd17, %rd5, %rd6;
	mul.lo.s64 	%rd20, %rd5, %rd6;
	shr.s64 	%rd18, %rd20, 63;
	setp.ne.b64 	%p11, %rd17, %rd18;
$L__BB3_7:                              // %chk
	not.pred 	%p10, %p11;
	@%p10 bra 	$L__BB3_9;
// %bb.8:                               // %ovf
	atom.relaxed.sys.global.or.b32 	%r6, [%rd2], 1;
$L__BB3_9:                              // %wr
	add.s64 	%rd19, %rd3, %rd14;
	st.global.b64 	[%rd19], %rd20;
$L__BB3_10:                             // %exit
	ret;
                                        // -- End function
}
	// .globl	sg_mm                   // -- Begin function sg_mm
.visible .entry sg_mm(
	.param .u64 .ptr .align 1 sg_mm_param_0,
	.param .u64 .ptr .align 1 sg_mm_param_1,
	.param .u64 .ptr .align 1 sg_mm_param_2,
	.param .u64 sg_mm_param_3,
	.param .u64 sg_mm_param_4,
	.param .u64 sg_mm_param_5
)                                       // @sg_mm
{
	.reg .pred 	%p<12>;
	.reg .b32 	%r<5>;
	.reg .b64 	%rd<54>;
	// demoted variable
	.shared .align 8 .b8 sg_mm_at[8192];
	// demoted variable
	.shared .align 8 .b8 sg_mm_bt[8192];
// %bb.0:                               // %entry
	ld.param.b64 	%rd14, [sg_mm_param_5];
	ld.param.b64 	%rd13, [sg_mm_param_4];
	ld.param.b64 	%rd12, [sg_mm_param_3];
	ld.param.b64 	%rd15, [sg_mm_param_0];
	ld.param.b64 	%rd16, [sg_mm_param_1];
	cvta.to.global.u64 	%rd1, %rd16;
	ld.param.b64 	%rd17, [sg_mm_param_2];
	cvta.to.global.u64 	%rd2, %rd17;
	cvta.to.global.u64 	%rd3, %rd15;
	mov.u32 	%r1, %tid.x;
	mov.u32 	%r2, %tid.y;
	mov.u32 	%r3, %ctaid.x;
	mov.u32 	%r4, %ctaid.y;
	cvt.u64.u32 	%rd4, %r1;
	cvt.u64.u32 	%rd5, %r2;
	mul.wide.u32 	%rd18, %r3, 32;
	mul.wide.u32 	%rd19, %r4, 32;
	add.s64 	%rd6, %rd19, %rd5;
	add.s64 	%rd7, %rd18, %rd4;
	setp.lt.s64 	%p2, %rd6, %rd12;
	setp.lt.s64 	%p3, %rd7, %rd13;
	and.pred 	%p1, %p2, %p3;
	mul.wide.u32 	%rd20, %r2, 32;
	add.s64 	%rd8, %rd20, %rd4;
	mul.wide.u32 	%rd21, %r1, 8;
	mov.b64 	%rd22, sg_mm_bt;
	add.s64 	%rd9, %rd22, %rd21;
	mul.wide.u32 	%rd23, %r2, 256;
	mov.b64 	%rd24, sg_mm_at;
	add.s64 	%rd10, %rd24, %rd23;
	mov.b64 	%rd50, 0d0000000000000000;
	mov.b64 	%rd49, 0;
	shl.b64 	%rd41, %rd8, 3;
$L__BB4_1:                              // %blk
                                        // =>This Loop Header: Depth=1
                                        //     Child Loop BB4_3 Depth 2
	setp.ge.s64 	%p4, %rd49, %rd14;
	@%p4 bra 	$L__BB4_5;
// %bb.2:                               // %load
                                        //   in Loop: Header=BB4_1 Depth=1
	add.s64 	%rd27, %rd49, %rd4;
	setp.lt.s64 	%p6, %rd27, %rd14;
	and.pred 	%p7, %p2, %p6;
	mad.lo.s64 	%rd28, %rd6, %rd14, %rd27;
	selp.b64 	%rd29, %rd28, 0, %p7;
	shl.b64 	%rd30, %rd29, 3;
	add.s64 	%rd31, %rd1, %rd30;
	ld.global.b64 	%rd32, [%rd31];
	selp.f64 	%rd33, %rd32, 0d0000000000000000, %p7;
	add.s64 	%rd34, %rd49, %rd5;
	setp.lt.s64 	%p8, %rd34, %rd14;
	and.pred 	%p9, %p8, %p3;
	mad.lo.s64 	%rd35, %rd34, %rd13, %rd7;
	selp.b64 	%rd36, %rd35, 0, %p9;
	shl.b64 	%rd37, %rd36, 3;
	add.s64 	%rd38, %rd2, %rd37;
	ld.global.b64 	%rd39, [%rd38];
	selp.f64 	%rd40, %rd39, 0d0000000000000000, %p9;
	add.s64 	%rd42, %rd24, %rd41;
	st.shared.b64 	[%rd42], %rd33;
	add.s64 	%rd43, %rd22, %rd41;
	st.shared.b64 	[%rd43], %rd40;
	bar.sync 	0;
	mov.b64 	%rd53, 0;
	mov.b64 	%rd51, %rd10;
	mov.b64 	%rd52, %rd9;
$L__BB4_3:                              // %inner
                                        //   Parent Loop BB4_1 Depth=1
                                        // =>  This Inner Loop Header: Depth=2
	add.s64 	%rd44, %rd49, %rd53;
	ld.shared.b64 	%rd45, [%rd51];
	ld.shared.b64 	%rd46, [%rd52];
	mul.rn.f64 	%rd47, %rd45, %rd46;
	add.rn.f64 	%rd48, %rd50, %rd47;
	setp.lt.s64 	%p10, %rd44, %rd14;
	selp.f64 	%rd50, %rd48, %rd50, %p10;
	add.s64 	%rd53, %rd53, 1;
	add.s64 	%rd52, %rd52, 256;
	add.s64 	%rd51, %rd51, 8;
	setp.ne.b64 	%p11, %rd53, 32;
	@%p11 bra 	$L__BB4_3;
// %bb.4:                               // %bar2
                                        //   in Loop: Header=BB4_1 Depth=1
	bar.sync 	0;
	add.s64 	%rd49, %rd49, 32;
	bra.uni 	$L__BB4_1;
$L__BB4_5:                              // %done
	not.pred 	%p5, %p1;
	@%p5 bra 	$L__BB4_7;
// %bb.6:                               // %wr
	mad.lo.s64 	%rd25, %rd6, %rd13, %rd7;
	shl.b64 	%rd26, %rd25, 3;
	add.s64 	%rd11, %rd3, %rd26;
	st.global.b64 	[%rd11], %rd50;
$L__BB4_7:                              // %exit
	ret;
                                        // -- End function
}
	// .globl	sg_sum                  // -- Begin function sg_sum
.visible .entry sg_sum(
	.param .u64 .ptr .align 1 sg_sum_param_0,
	.param .u64 .ptr .align 1 sg_sum_param_1,
	.param .u64 sg_sum_param_2
)                                       // @sg_sum
{
	.reg .pred 	%p<3>;
	.reg .b32 	%r<4>;
	.reg .b64 	%rd<9>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd3, [sg_sum_param_0];
	ld.param.b64 	%rd4, [sg_sum_param_1];
	cvta.to.global.u64 	%rd6, %rd4;
	cvta.to.global.u64 	%rd1, %rd3;
	mov.u32 	%r1, %tid.x;
	mov.u32 	%r2, %ctaid.x;
	or.b32 	%r3, %r1, %r2;
	setp.ne.b32 	%p1, %r3, 0;
	@%p1 bra 	$L__BB5_5;
// %bb.1:                               // %lp.preheader
	ld.param.b64 	%rd2, [sg_sum_param_2];
	mov.b64 	%rd8, 0d0000000000000000;
	mov.b64 	%rd7, 0;
$L__BB5_2:                              // %lp
                                        // =>This Inner Loop Header: Depth=1
	setp.ge.s64 	%p2, %rd7, %rd2;
	@%p2 bra 	$L__BB5_4;
// %bb.3:                               // %body
                                        //   in Loop: Header=BB5_2 Depth=1
	ld.global.b64 	%rd5, [%rd6];
	add.rn.f64 	%rd8, %rd8, %rd5;
	add.s64 	%rd7, %rd7, 1;
	add.s64 	%rd6, %rd6, 8;
	bra.uni 	$L__BB5_2;
$L__BB5_4:                              // %done
	st.global.b64 	[%rd1], %rd8;
$L__BB5_5:                              // %exit
	ret;
                                        // -- End function
}
	// .globl	sg_sum_i                // -- Begin function sg_sum_i
.visible .entry sg_sum_i(
	.param .u64 .ptr .align 1 sg_sum_i_param_0,
	.param .u64 .ptr .align 1 sg_sum_i_param_1,
	.param .u64 .ptr .align 1 sg_sum_i_param_2,
	.param .u64 sg_sum_i_param_3
)                                       // @sg_sum_i
{
	.reg .pred 	%p<7>;
	.reg .b32 	%r<4>;
	.reg .b64 	%rd<12>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd5, [sg_sum_i_param_0];
	ld.param.b64 	%rd6, [sg_sum_i_param_2];
	cvta.to.global.u64 	%rd9, %rd6;
	ld.param.b64 	%rd7, [sg_sum_i_param_1];
	cvta.to.global.u64 	%rd1, %rd7;
	cvta.to.global.u64 	%rd2, %rd5;
	mov.u32 	%r1, %tid.x;
	mov.u32 	%r2, %ctaid.x;
	or.b32 	%r3, %r1, %r2;
	setp.ne.b32 	%p1, %r3, 0;
	@%p1 bra 	$L__BB6_7;
// %bb.1:                               // %lp.preheader
	ld.param.b64 	%rd4, [sg_sum_i_param_3];
	mov.b64 	%rd11, 0;
	mov.b64 	%rd10, %rd11;
	bra.uni 	$L__BB6_2;
$L__BB6_5:                              // %cont
                                        //   in Loop: Header=BB6_2 Depth=1
	add.s64 	%rd10, %rd10, 1;
	add.s64 	%rd9, %rd9, 8;
	mov.b64 	%rd11, %rd3;
$L__BB6_2:                              // %lp
                                        // =>This Inner Loop Header: Depth=1
	setp.ge.s64 	%p2, %rd10, %rd4;
	@%p2 bra 	$L__BB6_6;
// %bb.3:                               // %body
                                        //   in Loop: Header=BB6_2 Depth=1
	ld.global.b64 	%rd8, [%rd9];
	add.s64 	%rd3, %rd11, %rd8;
	setp.lt.s64 	%p3, %rd3, %rd11;
	setp.lt.s64 	%p4, %rd8, 0;
	xor.pred 	%p5, %p4, %p3;
	not.pred 	%p6, %p5;
	@%p6 bra 	$L__BB6_5;
// %bb.4:                               // %ovf
                                        //   in Loop: Header=BB6_2 Depth=1
	st.global.b32 	[%rd1], 1;
	bra.uni 	$L__BB6_5;
$L__BB6_6:                              // %done
	st.global.b64 	[%rd2], %rd11;
$L__BB6_7:                              // %exit
	ret;
                                        // -- End function
}
	// .globl	sg_tr                   // -- Begin function sg_tr
.visible .entry sg_tr(
	.param .u64 .ptr .align 1 sg_tr_param_0,
	.param .u64 .ptr .align 1 sg_tr_param_1,
	.param .u64 sg_tr_param_2,
	.param .u64 sg_tr_param_3
)                                       // @sg_tr
{
	.reg .pred 	%p<3>;
	.reg .b32 	%r<9>;
	.reg .b64 	%rd<22>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd5, [sg_tr_param_3];
	ld.param.b64 	%rd4, [sg_tr_param_2];
	ld.param.b64 	%rd6, [sg_tr_param_0];
	ld.param.b64 	%rd7, [sg_tr_param_1];
	cvta.to.global.u64 	%rd1, %rd7;
	cvta.to.global.u64 	%rd2, %rd6;
	mov.u32 	%r1, %tid.x;
	mov.u32 	%r2, %ntid.x;
	mov.u32 	%r3, %ctaid.x;
	cvt.u64.u32 	%rd8, %r1;
	mul.wide.u32 	%rd9, %r3, %r2;
	add.s64 	%rd3, %rd9, %rd8;
	mul.lo.s64 	%rd10, %rd4, %rd5;
	setp.ge.s64 	%p1, %rd3, %rd10;
	@%p1 bra 	$L__BB7_5;
// %bb.1:                               // %do
	or.b64 	%rd11, %rd3, %rd4;
	and.b64 	%rd12, %rd11, -4294967296;
	setp.ne.b64 	%p2, %rd12, 0;
	@!%p2 bra 	$L__BB7_2;
// %bb.3:
	div.s64 	%rd20, %rd3, %rd4;
	mul.lo.s64 	%rd13, %rd20, %rd4;
	sub.s64 	%rd21, %rd3, %rd13;
	bra.uni 	$L__BB7_4;
$L__BB7_2:
	cvt.u32.u64 	%r4, %rd4;
	cvt.u32.u64 	%r5, %rd3;
	div.u32 	%r6, %r5, %r4;
	mul.lo.s32 	%r7, %r6, %r4;
	sub.s32 	%r8, %r5, %r7;
	cvt.u64.u32 	%rd20, %r6;
	cvt.u64.u32 	%rd21, %r8;
$L__BB7_4:                              // %do.split
	mad.lo.s64 	%rd14, %rd21, %rd5, %rd20;
	shl.b64 	%rd15, %rd14, 3;
	add.s64 	%rd16, %rd1, %rd15;
	ld.global.b64 	%rd17, [%rd16];
	shl.b64 	%rd18, %rd3, 3;
	add.s64 	%rd19, %rd2, %rd18;
	st.global.b64 	[%rd19], %rd17;
$L__BB7_5:                              // %exit
	ret;
                                        // -- End function
}
	// .globl	sg_red_f                // -- Begin function sg_red_f
.visible .entry sg_red_f(
	.param .u64 .ptr .align 1 sg_red_f_param_0,
	.param .u64 .ptr .align 1 sg_red_f_param_1,
	.param .u64 sg_red_f_param_2,
	.param .u32 sg_red_f_param_3
)                                       // @sg_red_f
{
	.reg .pred 	%p<6>;
	.reg .b32 	%r<5>;
	.reg .b64 	%rd<12>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd4, [sg_red_f_param_0];
	ld.param.b64 	%rd5, [sg_red_f_param_1];
	cvta.to.global.u64 	%rd1, %rd5;
	cvta.to.global.u64 	%rd2, %rd4;
	mov.u32 	%r2, %tid.x;
	mov.u32 	%r3, %ctaid.x;
	or.b32 	%r4, %r2, %r3;
	setp.ne.b32 	%p1, %r4, 0;
	@%p1 bra 	$L__BB8_5;
// %bb.1:                               // %init
	ld.param.b32 	%r1, [sg_red_f_param_3];
	ld.param.b64 	%rd3, [sg_red_f_param_2];
	ld.global.b64 	%rd11, [%rd1];
	add.s64 	%rd9, %rd1, 8;
	mov.b64 	%rd10, 1;
	setp.ne.b32 	%p3, %r1, 0;
$L__BB8_2:                              // %lp
                                        // =>This Inner Loop Header: Depth=1
	setp.ge.s64 	%p2, %rd10, %rd3;
	@%p2 bra 	$L__BB8_4;
// %bb.3:                               // %body
                                        //   in Loop: Header=BB8_2 Depth=1
	ld.global.b64 	%rd6, [%rd9];
	setp.gt.f64 	%p4, %rd6, %rd11;
	selp.f64 	%rd7, %rd6, %rd11, %p4;
	setp.lt.f64 	%p5, %rd6, %rd11;
	selp.f64 	%rd8, %rd6, %rd11, %p5;
	selp.f64 	%rd11, %rd7, %rd8, %p3;
	add.s64 	%rd10, %rd10, 1;
	add.s64 	%rd9, %rd9, 8;
	bra.uni 	$L__BB8_2;
$L__BB8_4:                              // %done
	st.global.b64 	[%rd2], %rd11;
$L__BB8_5:                              // %exit
	ret;
                                        // -- End function
}
	// .globl	sg_red_i                // -- Begin function sg_red_i
.visible .entry sg_red_i(
	.param .u64 .ptr .align 1 sg_red_i_param_0,
	.param .u64 .ptr .align 1 sg_red_i_param_1,
	.param .u64 sg_red_i_param_2,
	.param .u32 sg_red_i_param_3
)                                       // @sg_red_i
{
	.reg .pred 	%p<4>;
	.reg .b32 	%r<5>;
	.reg .b64 	%rd<12>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd4, [sg_red_i_param_0];
	ld.param.b64 	%rd5, [sg_red_i_param_1];
	cvta.to.global.u64 	%rd1, %rd5;
	cvta.to.global.u64 	%rd2, %rd4;
	mov.u32 	%r2, %tid.x;
	mov.u32 	%r3, %ctaid.x;
	or.b32 	%r4, %r2, %r3;
	setp.ne.b32 	%p1, %r4, 0;
	@%p1 bra 	$L__BB9_5;
// %bb.1:                               // %init
	ld.param.b32 	%r1, [sg_red_i_param_3];
	ld.param.b64 	%rd3, [sg_red_i_param_2];
	ld.global.b64 	%rd11, [%rd1];
	add.s64 	%rd9, %rd1, 8;
	mov.b64 	%rd10, 1;
	setp.ne.b32 	%p3, %r1, 0;
$L__BB9_2:                              // %lp
                                        // =>This Inner Loop Header: Depth=1
	setp.ge.s64 	%p2, %rd10, %rd3;
	@%p2 bra 	$L__BB9_4;
// %bb.3:                               // %body
                                        //   in Loop: Header=BB9_2 Depth=1
	ld.global.b64 	%rd6, [%rd9];
	max.s64 	%rd7, %rd6, %rd11;
	min.s64 	%rd8, %rd6, %rd11;
	selp.b64 	%rd11, %rd7, %rd8, %p3;
	add.s64 	%rd10, %rd10, 1;
	add.s64 	%rd9, %rd9, 8;
	bra.uni 	$L__BB9_2;
$L__BB9_4:                              // %done
	st.global.b64 	[%rd2], %rd11;
$L__BB9_5:                              // %exit
	ret;
                                        // -- End function
}
	// .globl	sg_un_f                 // -- Begin function sg_un_f
.visible .entry sg_un_f(
	.param .u64 .ptr .align 1 sg_un_f_param_0,
	.param .u64 .ptr .align 1 sg_un_f_param_1,
	.param .u64 sg_un_f_param_2,
	.param .u32 sg_un_f_param_3
)                                       // @sg_un_f
{
	.reg .pred 	%p<3>;
	.reg .b32 	%r<5>;
	.reg .b64 	%rd<16>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd4, [sg_un_f_param_0];
	ld.param.b64 	%rd5, [sg_un_f_param_1];
	cvta.to.global.u64 	%rd1, %rd5;
	cvta.to.global.u64 	%rd2, %rd4;
	ld.param.b64 	%rd6, [sg_un_f_param_2];
	mov.u32 	%r2, %tid.x;
	mov.u32 	%r3, %ntid.x;
	mov.u32 	%r4, %ctaid.x;
	cvt.u64.u32 	%rd7, %r2;
	mul.wide.u32 	%rd8, %r4, %r3;
	add.s64 	%rd3, %rd8, %rd7;
	setp.ge.s64 	%p1, %rd3, %rd6;
	@%p1 bra 	$L__BB10_2;
// %bb.1:                               // %do
	ld.param.b32 	%r1, [sg_un_f_param_3];
	shl.b64 	%rd9, %rd3, 3;
	add.s64 	%rd10, %rd1, %rd9;
	ld.global.b64 	%rd11, [%rd10];
	abs.f64 	%rd12, %rd11;
	sqrt.rn.f64 	%rd13, %rd11;
	setp.eq.b32 	%p2, %r1, 0;
	selp.f64 	%rd14, %rd12, %rd13, %p2;
	add.s64 	%rd15, %rd2, %rd9;
	st.global.b64 	[%rd15], %rd14;
$L__BB10_2:                             // %exit
	ret;
                                        // -- End function
}
	// .globl	sg_un_ai                // -- Begin function sg_un_ai
.visible .entry sg_un_ai(
	.param .u64 .ptr .align 1 sg_un_ai_param_0,
	.param .u64 .ptr .align 1 sg_un_ai_param_1,
	.param .u64 sg_un_ai_param_2
)                                       // @sg_un_ai
{
	.reg .pred 	%p<2>;
	.reg .b32 	%r<4>;
	.reg .b64 	%rd<15>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd4, [sg_un_ai_param_0];
	ld.param.b64 	%rd5, [sg_un_ai_param_1];
	cvta.to.global.u64 	%rd1, %rd5;
	cvta.to.global.u64 	%rd2, %rd4;
	ld.param.b64 	%rd6, [sg_un_ai_param_2];
	mov.u32 	%r1, %tid.x;
	mov.u32 	%r2, %ntid.x;
	mov.u32 	%r3, %ctaid.x;
	cvt.u64.u32 	%rd7, %r1;
	mul.wide.u32 	%rd8, %r3, %r2;
	add.s64 	%rd3, %rd8, %rd7;
	setp.ge.s64 	%p1, %rd3, %rd6;
	@%p1 bra 	$L__BB11_2;
// %bb.1:                               // %do
	shl.b64 	%rd9, %rd3, 3;
	add.s64 	%rd10, %rd1, %rd9;
	ld.global.b64 	%rd11, [%rd10];
	neg.s64 	%rd12, %rd11;
	max.s64 	%rd13, %rd11, %rd12;
	add.s64 	%rd14, %rd2, %rd9;
	st.global.b64 	[%rd14], %rd13;
$L__BB11_2:                             // %exit
	ret;
                                        // -- End function
}
	// .globl	sg_un_si                // -- Begin function sg_un_si
.visible .entry sg_un_si(
	.param .u64 .ptr .align 1 sg_un_si_param_0,
	.param .u64 .ptr .align 1 sg_un_si_param_1,
	.param .u64 sg_un_si_param_2
)                                       // @sg_un_si
{
	.reg .pred 	%p<2>;
	.reg .b32 	%r<4>;
	.reg .b64 	%rd<15>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd3, [sg_un_si_param_0];
	ld.param.b64 	%rd4, [sg_un_si_param_1];
	cvta.to.global.u64 	%rd5, %rd4;
	cvta.to.global.u64 	%rd6, %rd3;
	ld.param.b64 	%rd7, [sg_un_si_param_2];
	mov.u32 	%r1, %tid.x;
	mov.u32 	%r2, %ntid.x;
	mov.u32 	%r3, %ctaid.x;
	cvt.u64.u32 	%rd8, %r1;
	mul.wide.u32 	%rd9, %r3, %r2;
	add.s64 	%rd10, %rd9, %rd8;
	setp.ge.s64 	%p1, %rd10, %rd7;
	@%p1 bra 	$L__BB12_2;
// %bb.1:                               // %do
	shl.b64 	%rd11, %rd10, 3;
	add.s64 	%rd1, %rd5, %rd11;
	add.s64 	%rd2, %rd6, %rd11;
	ld.global.b64 	%rd12, [%rd1];
	cvt.rn.f64.s64 	%rd13, %rd12;
	sqrt.rn.f64 	%rd14, %rd13;
	st.global.b64 	[%rd2], %rd14;
$L__BB12_2:                             // %exit
	ret;
                                        // -- End function
}
	// .globl	sg_cpy                  // -- Begin function sg_cpy
.visible .entry sg_cpy(
	.param .u64 .ptr .align 1 sg_cpy_param_0,
	.param .u64 .ptr .align 1 sg_cpy_param_1,
	.param .u64 sg_cpy_param_2,
	.param .u64 sg_cpy_param_3
)                                       // @sg_cpy
{
	.reg .pred 	%p<2>;
	.reg .b32 	%r<4>;
	.reg .b64 	%rd<16>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd3, [sg_cpy_param_0];
	ld.param.b64 	%rd4, [sg_cpy_param_1];
	cvta.to.global.u64 	%rd5, %rd4;
	cvta.to.global.u64 	%rd6, %rd3;
	ld.param.b64 	%rd8, [sg_cpy_param_3];
	mov.u32 	%r1, %tid.x;
	mov.u32 	%r2, %ntid.x;
	mov.u32 	%r3, %ctaid.x;
	cvt.u64.u32 	%rd9, %r1;
	mul.wide.u32 	%rd10, %r3, %r2;
	add.s64 	%rd11, %rd10, %rd9;
	setp.ge.s64 	%p1, %rd11, %rd8;
	@%p1 bra 	$L__BB13_2;
// %bb.1:                               // %do
	ld.param.b64 	%rd7, [sg_cpy_param_2];
	add.s64 	%rd12, %rd11, %rd7;
	shl.b64 	%rd13, %rd12, 3;
	add.s64 	%rd1, %rd5, %rd13;
	shl.b64 	%rd14, %rd11, 3;
	add.s64 	%rd2, %rd6, %rd14;
	ld.global.b64 	%rd15, [%rd1];
	st.global.b64 	[%rd2], %rd15;
$L__BB13_2:                             // %exit
	ret;
                                        // -- End function
}
)PTX";

// OpenCL C kernel 源（OpenCL 后端 clBuildProgram 运行时 JIT）。与
// verify/cuda/sg_kernels.ll 逐 kernel 镜像——.ll 为语义权威，两文件改动必须
// 同步：确定性纪律同 .ll 头注（浮点/整数运算顺序与 CPU 逐位一致——mm/sum
// 串行升序归约，逐元素每元素恰一次 IEEE RN 运算；int 溢出判定与 CPU
// __builtin_*_overflow 逐位一致，回绕值照常写回，panic 由运行时回读状态后
// 以同一 sg_panic 终止）。FP_CONTRACT OFF + -cl-fmad=false 禁止 fma 收缩
// （sg_mm 的 mul+add 必须保持两条 IEEE 运算，与 CPU 逐位一致）。
static const char kGpuOpenCLC[] = R"OCL(/* sg_kernels.cl — Scourge OpenCL 后端 kernel（sg_kernels.ll 逐 kernel 镜像；
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

// GPU 分派运行时：后端抽象（SgGpuBe vtbl 六原语 alloc/mfree/h2d/d2h/launch1d/
// launch2d）+ 双后端——CUDA（nvcuda.dll driver API + 内嵌 PTX JIT）与 OpenCL
// （OpenCL.dll ICD loader + 内嵌 OpenCL C 源码 JIT，kernel 与 sg_kernels.ll
// 一一镜像、.ll 为语义权威）。SCOURGE_GPU_BACKEND=cuda|opencl|cpu 选择（缺省
// 自适应：先 CUDA 后 OpenCL fp64 GPU）；SCOURGE_NO_GPU=1 强制 CPU（逃生门兼
// 回退路径测试钩子）。任何初始化/launch 失败：sg_gpu_state=0 永久回退 CPU
// 实现（语义一致仅性能降级）。动态形状放宽后覆盖判定不查 shape 属性：
// dtype/秩/形状一致/内维由各 try 函数运行时守卫校验，不匹配返回 NULL 回退
// CPU（panic 消息逐字一致）。int 覆盖集（ew_ti/tsi/sum_i）的溢出由 kernel
// 以状态缓冲上报：回读非 0 即以与 CPU 同一 sg_panic 终止（fn/file/line 为
// 调用点 ABI 形参，f64 覆盖集无 panic 路径仅对齐用）。
const char kGpuRuntimeC[] = R"GP(/* ==== GPU 分派第 1 层（后端抽象 + CUDA/OpenCL 覆盖集） ==== */
#ifdef _WIN32
typedef int CUresult;
typedef struct CUctx_st *CUcontext;
typedef struct CUmod_st *CUmodule;
typedef struct CUfunc_st *CUfunction;
typedef unsigned long long CUdeviceptr;

static CUcontext sg_gpu_ctx = NULL;
static CUmodule sg_gpu_mod = NULL;
/* kernel 句柄数组（后端无关索引；CUDA 存 CUfunction） */
enum {
    SG_K_TT = 0, SG_K_TS, SG_K_TI, SG_K_TSI, SG_K_MM, SG_K_SUM, SG_K_SUMI,
    SG_K_TR, SG_K_REDF, SG_K_REDI, SG_K_UNF, SG_K_UNAI, SG_K_UNSI, SG_K_CPY,
    SG_K_N
};
static CUfunction sg_cu_fn[SG_K_N];
static CUresult (*sg_cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*sg_cuMemFree)(CUdeviceptr);
static CUresult (*sg_cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
static CUresult (*sg_cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
static CUresult (*sg_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                     unsigned, unsigned, unsigned, unsigned,
                                     CUdeviceptr, void **, void ***);
static CUresult (*sg_cuCtxSynchronize)(void);
static int sg_gpu_state = -1; /* -1 未初始化 0 不可用 1 就绪 */

/* ---- 后端原语 vtbl：CUDA/OpenCL 各自填充，try 函数与缓存层后端无关 ----
 * dev 句柄统一 void*（CUDA CUdeviceptr / OpenCL cl_mem，x64 上均 8 字节）；
 * launch 自带完成同步（CUDA CtxSynchronize / OpenCL clFinish），一次调用
 * 返回即 kernel 已完成——try 函数读回/panic 时序与缓存逐出假设依赖此点。 */
typedef struct {
    int (*alloc)(void **out, size_t bytes);
    void (*mfree)(void *p);
    int (*h2d)(void *dev, const void *host, size_t bytes);
    int (*d2h)(void *host, const void *dev, size_t bytes);
    int (*launch1d)(int k, int64_t n, void **args);
    int (*launch2d)(int k, int64_t gx, int64_t gy, void **args);
} SgGpuBe;
static SgGpuBe sg_be;

/* 设备缓冲钩子实现（定义见后段注册表块；sg_gpu_init 安装） */
static void sg_tsync_impl(SgTensor *t);
static void sg_tdirty_impl(SgTensor *t);

static void sg_gpu_fail(void) { sg_gpu_state = 0; }

/* ---- CUDA 后端（nvcuda.dll driver API + 内嵌 PTX JIT；原路径） ---- */
static int sg_cuda_init(void) {
    HMODULE h = LoadLibraryA("nvcuda.dll");
    if (!h) return 1;
    CUresult (*cuInit)(unsigned) = (void *)GetProcAddress(h, "cuInit");
    CUresult (*cuDeviceGet)(int *, int) =
        (void *)GetProcAddress(h, "cuDeviceGet");
    CUresult (*cuRetain)(CUcontext *, int) =
        (void *)GetProcAddress(h, "cuDevicePrimaryCtxRetain");
    CUresult (*cuSetCtx)(CUcontext) =
        (void *)GetProcAddress(h, "cuCtxSetCurrent");
    sg_cuCtxSynchronize = (void *)GetProcAddress(h, "cuCtxSynchronize");
    CUresult (*cuModLoad)(CUmodule *, const void *) =
        (void *)GetProcAddress(h, "cuModuleLoadData");
    CUresult (*cuModFn)(CUfunction *, CUmodule, const char *) =
        (void *)GetProcAddress(h, "cuModuleGetFunction");
    sg_cuMemAlloc = (void *)GetProcAddress(h, "cuMemAlloc_v2");
    sg_cuMemFree = (void *)GetProcAddress(h, "cuMemFree_v2");
    sg_cuMemcpyHtoD = (void *)GetProcAddress(h, "cuMemcpyHtoD_v2");
    sg_cuMemcpyDtoH = (void *)GetProcAddress(h, "cuMemcpyDtoH_v2");
    sg_cuLaunchKernel = (void *)GetProcAddress(h, "cuLaunchKernel");
    if (!cuInit || !cuDeviceGet || !cuRetain || !cuSetCtx ||
        !sg_cuCtxSynchronize || !cuModLoad || !cuModFn || !sg_cuMemAlloc ||
        !sg_cuMemFree || !sg_cuMemcpyHtoD || !sg_cuMemcpyDtoH ||
        !sg_cuLaunchKernel)
        return 1;
    if (cuInit(0)) return 1;
    int dev = 0;
    if (cuDeviceGet(&dev, 0)) return 1;
    if (cuRetain(&sg_gpu_ctx, dev)) return 1;
    if (cuSetCtx(sg_gpu_ctx)) return 1;
    if (cuModLoad(&sg_gpu_mod, sg_gpu_ptx)) return 1;
    if (cuModFn(&sg_cu_fn[SG_K_TT], sg_gpu_mod, "sg_ew_tt") ||
        cuModFn(&sg_cu_fn[SG_K_TS], sg_gpu_mod, "sg_ew_ts") ||
        cuModFn(&sg_cu_fn[SG_K_TI], sg_gpu_mod, "sg_ew_ti") ||
        cuModFn(&sg_cu_fn[SG_K_TSI], sg_gpu_mod, "sg_ew_tsi") ||
        cuModFn(&sg_cu_fn[SG_K_MM], sg_gpu_mod, "sg_mm") ||
        cuModFn(&sg_cu_fn[SG_K_SUM], sg_gpu_mod, "sg_sum") ||
        cuModFn(&sg_cu_fn[SG_K_SUMI], sg_gpu_mod, "sg_sum_i") ||
        cuModFn(&sg_cu_fn[SG_K_TR], sg_gpu_mod, "sg_tr") ||
        cuModFn(&sg_cu_fn[SG_K_REDF], sg_gpu_mod, "sg_red_f") ||
        cuModFn(&sg_cu_fn[SG_K_REDI], sg_gpu_mod, "sg_red_i") ||
        cuModFn(&sg_cu_fn[SG_K_UNF], sg_gpu_mod, "sg_un_f") ||
        cuModFn(&sg_cu_fn[SG_K_UNAI], sg_gpu_mod, "sg_un_ai") ||
        cuModFn(&sg_cu_fn[SG_K_UNSI], sg_gpu_mod, "sg_un_si") ||
        cuModFn(&sg_cu_fn[SG_K_CPY], sg_gpu_mod, "sg_cpy"))
        return 1;
    return 0;
}

/* ---- CUDA vtbl 实现（launch 自带 CtxSynchronize，返回即 kernel 完成） ---- */
static int sg_cu_be_alloc(void **out, size_t b) {
    CUdeviceptr d = 0;
    if (sg_cuMemAlloc(&d, b)) return 1;
    *out = (void *)d;
    return 0;
}
static void sg_cu_be_free(void *p) { sg_cuMemFree((CUdeviceptr)p); }
static int sg_cu_be_h2d(void *d, const void *h, size_t b) {
    return sg_cuMemcpyHtoD((CUdeviceptr)d, h, b);
}
static int sg_cu_be_d2h(void *h, const void *d, size_t b) {
    return sg_cuMemcpyDtoH(h, (CUdeviceptr)d, b);
}
static int sg_cu_be_launch1d(int k, int64_t n, void **args) {
    unsigned th = 256, bl = (unsigned)((n + 255) / 256);
    if (bl == 0) bl = 1;
    if (sg_cuLaunchKernel(sg_cu_fn[k], bl, 1, 1, th, 1, 1, 0, 0, args, NULL))
        return 1;
    return sg_cuCtxSynchronize();
}
static int sg_cu_be_launch2d(int k, int64_t gx, int64_t gy, void **args) {
    if (sg_cuLaunchKernel(sg_cu_fn[k], (unsigned)gx, (unsigned)gy, 1, 32, 32,
                          1, 0, 0, args, NULL))
        return 1;
    return sg_cuCtxSynchronize();
}

/* ---- OpenCL 后端（OpenCL.dll ICD loader + OpenCL C 源码运行时 JIT；
 * kernel 源 kGpuOpenCLC 与 sg_kernels.ll 一一镜像，.ll 为语义权威） ---- */
typedef struct _cl_platform_id *cl_platform_id;
typedef struct _cl_device_id *cl_device_id;
typedef struct _cl_context *cl_context;
typedef struct _cl_command_queue *cl_command_queue;
typedef struct _cl_program *cl_program;
typedef struct _cl_mem *cl_mem;
typedef struct _cl_kernel *cl_kernel;
static cl_context sg_ocl_ctx = NULL;
static cl_command_queue sg_ocl_q = NULL;
static cl_kernel sg_ocl_kn[SG_K_N];
/* 各 kernel 参数尺寸表（clSetKernelArg 用；指针=cl_mem 8B、u64/f64 8B、
 * u32 4B——与 try 函数 args 数组的实参类型逐一对应） */
static const unsigned char sg_ocl_asz[SG_K_N][8] = {
    {8, 8, 8, 8, 4, 0, 0, 0}, /* TT:   out,a,b,n,op */
    {8, 8, 8, 8, 4, 4, 0, 0}, /* TS:   out,a,s,n,op,sleft */
    {8, 8, 8, 8, 4, 8, 0, 0}, /* TI:   out,a,b,n,op,status */
    {8, 8, 8, 8, 4, 4, 8, 0}, /* TSI:  out,a,s,n,op,sleft,status */
    {8, 8, 8, 8, 8, 8, 0, 0}, /* MM:   out,a,b,m,n,k */
    {8, 8, 8, 0, 0, 0, 0, 0}, /* SUM:  out,a,n */
    {8, 8, 8, 8, 0, 0, 0, 0}, /* SUMI: out,status,a,n */
    {8, 8, 8, 8, 0, 0, 0, 0}, /* TR:   out,a,m,n */
    {8, 8, 8, 4, 0, 0, 0, 0}, /* REDF: out,a,n,isMax */
    {8, 8, 8, 4, 0, 0, 0, 0}, /* REDI: out,a,n,isMax */
    {8, 8, 8, 4, 0, 0, 0, 0}, /* UNF:  out,a,n,fnid */
    {8, 8, 8, 0, 0, 0, 0, 0}, /* UNAI: out,a,n */
    {8, 8, 8, 0, 0, 0, 0, 0}, /* UNSI: out,a,n */
    {8, 8, 8, 8, 0, 0, 0, 0}  /* CPY:  out,a,off,n */
};
static const unsigned char sg_ocl_an[SG_K_N] = {5, 6, 6, 7, 6, 3, 4, 4,
                                                4, 4, 4, 3, 3, 4};
static int (*sg_clGetPlatformIDs)(unsigned, cl_platform_id *, unsigned *);
static int (*sg_clGetDeviceIDs)(cl_platform_id, unsigned long long, unsigned,
                                cl_device_id *, unsigned *);
static int (*sg_clGetDeviceInfo)(cl_device_id, unsigned, size_t, void *,
                                 size_t *);
static cl_context (*sg_clCreateContext)(const intptr_t *, unsigned,
                                        cl_device_id *, void *, void *, int *);
static cl_command_queue (*sg_clCreateCommandQueue)(cl_context, cl_device_id,
                                                   unsigned long long, int *);
static cl_program (*sg_clCreateProgramWithSource)(cl_context, unsigned,
                                                  const char **, const size_t *,
                                                  int *);
static int (*sg_clBuildProgram)(cl_program, unsigned, cl_device_id *,
                                const char *, void *, void *);
static cl_kernel (*sg_clCreateKernel)(cl_program, const char *, int *);
static int (*sg_clSetKernelArg)(cl_kernel, unsigned, size_t, const void *);
static int (*sg_clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, unsigned,
                                        const size_t *, const size_t *,
                                        const size_t *, unsigned, void *,
                                        void *);
static int (*sg_clEnqueueWriteBuffer)(cl_command_queue, cl_mem, unsigned,
                                      size_t, size_t, const void *, unsigned,
                                      void *, void *);
static int (*sg_clEnqueueReadBuffer)(cl_command_queue, cl_mem, unsigned,
                                     size_t, size_t, void *, unsigned, void *,
                                     void *);
static int (*sg_clFinish)(cl_command_queue);
static int (*sg_clReleaseMemObject)(cl_mem);
static cl_mem (*sg_clCreateBuffer)(cl_context, unsigned long long, size_t,
                                   void *, int *);

static int sg_ocl_be_alloc(void **out, size_t b) {
    int err = 0;
    cl_mem m = sg_clCreateBuffer(sg_ocl_ctx, 4ull /*CL_MEM_READ_WRITE*/,
                                 b ? b : 1, NULL, &err);
    if (!m || err) return 1;
    *out = (void *)m;
    return 0;
}
static void sg_ocl_be_free(void *p) { sg_clReleaseMemObject((cl_mem)p); }
static int sg_ocl_be_h2d(void *d, const void *h, size_t b) {
    /* 阻塞传输（CL_TRUE）：与 vtbl「调用返回即数据就位」纪律一致 */
    return sg_clEnqueueWriteBuffer(sg_ocl_q, (cl_mem)d, 1, 0, b, h, 0, NULL,
                                   NULL)
               ? 1
               : 0;
}
static int sg_ocl_be_d2h(void *h, const void *d, size_t b) {
    return sg_clEnqueueReadBuffer(sg_ocl_q, (cl_mem)d, 1, 0, b, h, 0, NULL,
                                  NULL)
               ? 1
               : 0;
}
static int sg_ocl_launch(int k, unsigned dim, const size_t *gws,
                         const size_t *lws, void **args) {
    cl_kernel kn = sg_ocl_kn[k];
    for (int i = 0; i < sg_ocl_an[k]; ++i)
        if (sg_clSetKernelArg(kn, (unsigned)i, sg_ocl_asz[k][i], args[i]))
            return 1;
    if (sg_clEnqueueNDRangeKernel(sg_ocl_q, kn, dim, NULL, gws, lws, 0, NULL,
                                  NULL))
        return 1;
    return sg_clFinish(sg_ocl_q) ? 1 : 0;
}
static int sg_ocl_be_launch1d(int k, int64_t n, void **args) {
    size_t g = (size_t)((n + 255) / 256) * 256;
    if (g == 0) g = 256; /* kernel 侧 idx>=n 守卫，多余线程空转 */
    return sg_ocl_launch(k, 1, &g, NULL, args);
}
static int sg_ocl_be_launch2d(int k, int64_t gx, int64_t gy, void **args) {
    size_t g[2] = {(size_t)gx * 32, (size_t)gy * 32};
    size_t l[2] = {32, 32}; /* sg_mm tiling：local 必须 32×32 */
    return sg_ocl_launch(k, 2, g, l, args);
}

static void sg_opencl_init(void) {
    HMODULE h = LoadLibraryA("OpenCL.dll");
    if (!h) return;
    sg_clGetPlatformIDs = (void *)GetProcAddress(h, "clGetPlatformIDs");
    sg_clGetDeviceIDs = (void *)GetProcAddress(h, "clGetDeviceIDs");
    sg_clGetDeviceInfo = (void *)GetProcAddress(h, "clGetDeviceInfo");
    sg_clCreateContext = (void *)GetProcAddress(h, "clCreateContext");
    sg_clCreateCommandQueue = (void *)GetProcAddress(h, "clCreateCommandQueue");
    sg_clCreateProgramWithSource =
        (void *)GetProcAddress(h, "clCreateProgramWithSource");
    sg_clBuildProgram = (void *)GetProcAddress(h, "clBuildProgram");
    sg_clCreateKernel = (void *)GetProcAddress(h, "clCreateKernel");
    sg_clSetKernelArg = (void *)GetProcAddress(h, "clSetKernelArg");
    sg_clEnqueueNDRangeKernel =
        (void *)GetProcAddress(h, "clEnqueueNDRangeKernel");
    sg_clEnqueueWriteBuffer = (void *)GetProcAddress(h, "clEnqueueWriteBuffer");
    sg_clEnqueueReadBuffer = (void *)GetProcAddress(h, "clEnqueueReadBuffer");
    sg_clFinish = (void *)GetProcAddress(h, "clFinish");
    sg_clReleaseMemObject = (void *)GetProcAddress(h, "clReleaseMemObject");
    sg_clCreateBuffer = (void *)GetProcAddress(h, "clCreateBuffer");
    if (!sg_clGetPlatformIDs || !sg_clGetDeviceIDs || !sg_clGetDeviceInfo ||
        !sg_clCreateContext || !sg_clCreateCommandQueue ||
        !sg_clCreateProgramWithSource || !sg_clBuildProgram ||
        !sg_clCreateKernel || !sg_clSetKernelArg || !sg_clEnqueueNDRangeKernel ||
        !sg_clEnqueueWriteBuffer || !sg_clEnqueueReadBuffer || !sg_clFinish ||
        !sg_clReleaseMemObject || !sg_clCreateBuffer)
        return;
    unsigned np = 0;
    if (sg_clGetPlatformIDs(0, NULL, &np) || np == 0) return;
    cl_platform_id *plats =
        (cl_platform_id *)malloc(sizeof(cl_platform_id) * np);
    if (!plats) return;
    if (sg_clGetPlatformIDs(np, plats, NULL)) {
        free(plats);
        return;
    }
    /* 取首个带 fp64 的 GPU 设备（CL_DEVICE_TYPE_GPU 过滤 CPU 设备；
     * 无 fp64 → 覆盖集需 f64 → CPU 回退） */
    cl_device_id dev = NULL;
    for (unsigned i = 0; i < np && !dev; ++i) {
        cl_device_id d = NULL;
        unsigned nd = 0;
        if (sg_clGetDeviceIDs(plats[i], 4 /*CL_DEVICE_TYPE_GPU*/, 1, &d, &nd) ||
            nd == 0)
            continue;
        unsigned long long fp64 = 0;
        if (sg_clGetDeviceInfo(d, 0x0F02 /*CL_DEVICE_DOUBLE_FP_CONFIG*/,
                               sizeof(fp64), &fp64, NULL) || !fp64)
            continue;
        dev = d;
    }
    free(plats);
    if (!dev) return;
    int err = 0;
    sg_ocl_ctx = sg_clCreateContext(NULL, 1, &dev, NULL, NULL, &err);
    if (!sg_ocl_ctx || err) return;
    sg_ocl_q = sg_clCreateCommandQueue(sg_ocl_ctx, dev, 0, &err);
    if (!sg_ocl_q || err) return;
    const char *src = sg_gpu_oclc;
    size_t srclen = strlen(src);
    cl_program prog =
        sg_clCreateProgramWithSource(sg_ocl_ctx, 1, &src, &srclen, &err);
    if (!prog || err) return;
    if (sg_clBuildProgram(prog, 1, &dev, "-cl-std=CL1.2 -cl-fmad=false", NULL,
                          NULL))
        return;
    {
        static const char *kn[SG_K_N] = {"sg_ew_tt", "sg_ew_ts", "sg_ew_ti",
                                         "sg_ew_tsi", "sg_mm", "sg_sum",
                                         "sg_sum_i", "sg_tr", "sg_red_f",
                                         "sg_red_i", "sg_un_f", "sg_un_ai",
                                         "sg_un_si", "sg_cpy"};
        for (int i = 0; i < SG_K_N; ++i) {
            sg_ocl_kn[i] = sg_clCreateKernel(prog, kn[i], &err);
            if (!sg_ocl_kn[i]) return;
        }
    }
    sg_be.alloc = sg_ocl_be_alloc;
    sg_be.mfree = sg_ocl_be_free;
    sg_be.h2d = sg_ocl_be_h2d;
    sg_be.d2h = sg_ocl_be_d2h;
    sg_be.launch1d = sg_ocl_be_launch1d;
    sg_be.launch2d = sg_ocl_be_launch2d;
    sg_tsync_fn = sg_tsync_impl;
    sg_tdirty_fn = sg_tdirty_impl;
    sg_gpu_state = 1;
}

/* 后端选择：SCOURGE_GPU_BACKEND=cuda|opencl|cpu（缺省自适应——NVIDIA 优先
 * CUDA PTX 路径，失败/缺失落 OpenCL fp64 GPU，再失败 CPU）；SCOURGE_NO_GPU=1
 * 恒 CPU（逃生门兼回退测试钩子）。后端选择发生在运行时，不进构建缓存键。 */
static void sg_gpu_init(void) {
    sg_gpu_state = 0;
    if (getenv("SCOURGE_NO_GPU")) return;
    const char *be = getenv("SCOURGE_GPU_BACKEND");
    int tryCuda = 1, tryOcl = 1;
    if (be) {
        if (!strcmp(be, "cuda"))
            tryOcl = 0;
        else if (!strcmp(be, "opencl"))
            tryCuda = 0;
        else if (!strcmp(be, "cpu"))
            tryCuda = tryOcl = 0; /* 其他值：按缺省自适应 */
    }
    if (tryCuda && !sg_cuda_init()) {
        sg_be.alloc = sg_cu_be_alloc;
        sg_be.mfree = sg_cu_be_free;
        sg_be.h2d = sg_cu_be_h2d;
        sg_be.d2h = sg_cu_be_d2h;
        sg_be.launch1d = sg_cu_be_launch1d;
        sg_be.launch2d = sg_cu_be_launch2d;
        sg_tsync_fn = sg_tsync_impl;
        sg_tdirty_fn = sg_tdirty_impl;
        sg_gpu_state = 1;
        return;
    }
    if (tryOcl) sg_opencl_init();
}

static int sg_gpu_ready(void) {
    if (sg_gpu_state < 0) sg_gpu_init();
    return sg_gpu_state;
}

/* ---- 设备缓冲复用 + 惰性回拷（LRU 容量管理） ----
 * SgTensor.dev 非 0 = 设备驻留副本；hostStale=1 = 设备比 host 新（GPU 输出
 * 延迟回拷，host 读入口经 sg_tsync_impl 回拷）。不变量：dev 非 0 时设备副本
 * 恒为最新——host 写入走 sg_tdirty_impl 整体失效释放，故输入复用无需比对。
 * 注册表只做容量管理：驻留总量超 SCOURGE_GPU_CACHE_MB（默认 512MB）按 LRU
 * 逐出（hostStale 条目先回拷——数据唯一副本在设备）。释放纪律与 host arena
 * 一致：进程生命周期持有，仅容量/失效路径释放（进程退出由驱动统一回收）。 */
typedef struct {
    SgTensor *t;
    void *d;
    size_t bytes;
    uint64_t use;
} DevEnt;
static DevEnt *sg_devtab = NULL;
static int sg_devn = 0, sg_devcap = 0;
static size_t sg_devbytes = 0;
static uint64_t sg_devclk = 0;

/* 张量数据字节数（i64/f64 元素均 8 字节）；空张量占位 8 字节哑分配 */
static size_t sg_dev_bytes(const SgTensor *t) {
    size_t b = (size_t)sg_tsize(t) * 8;
    return b ? b : 8;
}

/* 预算：SCOURGE_GPU_CACHE_MB（MB），非法/未设取 512 */
static size_t sg_dev_budget(void) {
    static size_t budget = 0;
    if (!budget) {
        const char *e = getenv("SCOURGE_GPU_CACHE_MB");
        long v = e ? atol(e) : 512;
        if (v <= 0) v = 512;
        budget = (size_t)v * 1024 * 1024;
    }
    return budget;
}

/* LRU 逐出：驻留总量 + need 超预算时逐出最旧条目直至达标；p1/p2 为保护
 * 张量（本轮仍需其设备块的输入，NULL 可省）——受保护条目全时软预算放行。 */
static void sg_dev_evict(size_t need, const SgTensor *p1, const SgTensor *p2) {
    size_t budget = sg_dev_budget();
    while (sg_devn > 0 && sg_devbytes + need > budget) {
        int old = -1;
        for (int k = 0; k < sg_devn; ++k) {
            SgTensor *tk = sg_devtab[k].t;
            if (tk == p1 || tk == p2) continue;
            if (old < 0 || sg_devtab[k].use < sg_devtab[old].use) old = k;
        }
        if (old < 0) return;
        SgTensor *t = sg_devtab[old].t;
        void *d = sg_devtab[old].d;
        if (t->hostStale) { /* 数据唯一副本在设备：先回拷再释放 */
            void *h = t->isInt ? (void *)t->i64 : (void *)t->f64;
            if (sg_be.d2h(h, d, sg_devtab[old].bytes))
                sg_gpu_fail(); /* 上下文整体已坏（拷贝通道随 launch 级失败
                                  存活，实际不可达）； arena 纪律不恢复 */
            else
                t->hostStale = 0;
        }
        sg_be.mfree(d);
        t->dev = NULL;
        sg_devbytes -= sg_devtab[old].bytes;
        sg_devtab[old] = sg_devtab[--sg_devn];
    }
}

/* 登记（dget/devown 共用）：容量翻倍扩表，失败返回 0（调用方释放设备块） */
static int sg_dev_reg(SgTensor *t, void *d, size_t bytes, int stale) {
    if (sg_devn == sg_devcap) {
        int cap = sg_devcap ? sg_devcap * 2 : 64;
        DevEnt *nt = (DevEnt *)realloc(sg_devtab, (size_t)cap * sizeof(DevEnt));
        if (!nt) return 0;
        sg_devtab = nt;
        sg_devcap = cap;
    }
    sg_devtab[sg_devn].t = t;
    sg_devtab[sg_devn].d = d;
    sg_devtab[sg_devn].bytes = bytes;
    sg_devtab[sg_devn].use = ++sg_devclk;
    ++sg_devn;
    sg_devbytes += bytes;
    t->dev = d;
    t->hostStale = stale;
    return 1;
}

/* dget：取输入张量当前设备块——已驻留直接复用（含 hostStale：设备仍最新，
 * 免拷贝吃链），未驻留上传并登记。prot 为本轮已取得、kernel 尚未运行、
 * 不可被逐出的输入张量（NULL 可省）；失败返回 0 → 调用方整体回退 CPU。 */
static void *sg_gpu_dget(SgTensor *t, const SgTensor *prot) {
    size_t bytes = sg_dev_bytes(t);
    if (t->dev) {
        for (int k = 0; k < sg_devn; ++k)
            if (sg_devtab[k].t == t) sg_devtab[k].use = ++sg_devclk;
        return t->dev;
    }
    sg_dev_evict(bytes, prot, NULL);
    void *h = t->isInt ? (void *)t->i64 : (void *)t->f64;
    void *d = NULL;
    if (sg_be.alloc(&d, bytes) || sg_be.h2d(d, h, bytes)) {
        if (d) sg_be.mfree(d);
        return 0;
    }
    if (!sg_dev_reg(t, d, bytes, 0)) {
        sg_be.mfree(d);
        return 0;
    }
    return d;
}

/* devown：GPU 输出设备块归属张量（hostStale=1 惰性回拷）。失败释放并返回
 * 0 → 调用方回退 CPU 重算（此刻 kernel 已同步完成，输入设备块仍有效缓存）。 */
static int sg_gpu_devown(SgTensor *t, void *d, size_t bytes) {
    sg_dev_evict(bytes, NULL, NULL); /* kernel 已同步：输入设备块可逐出 */
    if (!sg_dev_reg(t, d, bytes, 1)) {
        sg_be.mfree(d);
        return 0;
    }
    return 1;
}

/* tsync 实现：host 读入口回拷（仅 hostStale 时有一次 DtoH，其余零开销） */
static void sg_tsync_impl(SgTensor *t) {
    if (!t->dev || !t->hostStale) return;
    void *h = t->isInt ? (void *)t->i64 : (void *)t->f64;
    if (sg_be.d2h(h, t->dev, sg_dev_bytes(t)))
        sg_gpu_fail(); /* 同上：上下文整体已坏，实际不可达 */
    else
        t->hostStale = 0;
}

/* tdirty 实现：host 写入口整体失效（host 为新权威，设备副本释放） */
static void sg_tdirty_impl(SgTensor *t) {
    if (!t->dev) return;
    sg_be.mfree(t->dev);
    for (int k = 0; k < sg_devn; ++k) {
        if (sg_devtab[k].t == t) {
            sg_devbytes -= sg_devtab[k].bytes;
            sg_devtab[k] = sg_devtab[--sg_devn];
            break;
        }
    }
    t->dev = NULL;
    t->hostStale = 0;
}

/* ---- GPU try 函数：单次尝试，失败置 state=0 并返回 NULL/!ok → 调用方 ----
 * 回退 CPU。args 数组逐项取局部变量地址，顺序与各 kernel 参数一一对应
 * （指针 .u64 / 整形维度 .u64 / op 与方向标志 .u32 / 标量 .f64）。 ---- */

/* 逐元素 tt（kernel sg_ew_tt(out,a,b,n,op)；op 0=+ 1=- 2=*）。
 * 动态形状放宽后覆盖判定不查 shape 属性：秩/逐维一致与 dtype 由本守卫
 * 运行时校验，不匹配返回 NULL → CPU panic 语义逐字一致。 */
static SgTensor *sg_gpu_ew_tt_try(int op, const SgTensor *a,
                                  const SgTensor *b) {
    if (op < 0 || op > 2) return NULL; /* 防御：覆盖集外 → CPU */
    if (a->isInt || b->isInt) return NULL; /* f64 kernel 防御 */
    if (a->rank != b->rank) return NULL;
    for (int64_t k = 0; k < a->rank; ++k)
        if (a->shape[k] != b->shape[k]) return NULL;
    int64_t n = sg_tsize(a);
    SgTensor *t = sg_tensor_alloc(0, a->rank, a->shape);
    if (n == 0) return t; /* 空张量：与 CPU 同为空结果，免 launch */
    void *da = sg_gpu_dget((SgTensor *)a, NULL); /* 复用或上传 */
    void *db = sg_gpu_dget((SgTensor *)b, a);    /* 逐出保护 a */
    void *dout = NULL;
    int32_t op32 = (int32_t)op;
    void *args[] = {&dout, &da, &db, &n, &op32};
    int bad = !da || !db || sg_be.alloc(&dout, (size_t)n * 8) ||
              sg_be.launch1d(SG_K_TT, n, args);
    if (bad) {
        if (dout) sg_be.mfree(dout);
        sg_gpu_fail();
        return NULL; /* 输入设备块仍有效缓存，留给后续复用 */
    }
    /* 惰性回拷：输出驻留设备（hostStale=1），链式 GPU op 直接以其为输入，
     * host 读入口经 sg_tsync 回拷——免除每调用 HtoD/DtoH/MemFree。 */
    if (!sg_gpu_devown(t, dout, (size_t)n * 8)) {
        sg_gpu_fail();
        return NULL;
    }
    return t;
}

/* 逐元素 ts/st（kernel sg_ew_ts(out,a,s,n,op,sleft)；sleft=0 → a[i] op s，
 * 1 → s op a[i]；标量恒取 f64 值，与覆盖集 f64 语义一致） */
static SgTensor *sg_gpu_ew_ts_try(int op, const SgTensor *a, double s,
                                  int sleft) {
    if (op < 0 || op > 2) return NULL;
    if (a->isInt) return NULL; /* f64 kernel 防御 */
    int64_t n = sg_tsize(a);
    SgTensor *t = sg_tensor_alloc(0, a->rank, a->shape);
    if (n == 0) return t;
    void *da = sg_gpu_dget((SgTensor *)a, NULL);
    void *dout = NULL;
    int32_t op32 = (int32_t)op, sl32 = (int32_t)sleft;
    void *args[] = {&dout, &da, &s, &n, &op32, &sl32};
    int bad = !da || sg_be.alloc(&dout, (size_t)n * 8) ||
              sg_be.launch1d(SG_K_TS, n, args);
    if (bad) {
        if (dout) sg_be.mfree(dout);
        sg_gpu_fail();
        return NULL;
    }
    if (!sg_gpu_devown(t, dout, (size_t)n * 8)) { /* 惰性回拷 */
        sg_gpu_fail();
        return NULL;
    }
    return t;
}

/* int 溢出消息（与 CPU sg_add_i64/sg_sub_i64/sg_mul_i64 逐字一致） */
static const char *sg_ti_msg(int op) {
    static const char *msgs[] = {"整数加法溢出", "整数减法溢出",
                                 "整数乘法溢出"};
    return msgs[op];
}

/* int 逐元素 tt（kernel sg_ew_ti(out,a,b,n,op,status)；op 0=+ 1=- 2=*）。
 * 溢出由 kernel 经状态缓冲 atomic_or 上报：回读非 0 → 与 CPU 同一
 * sg_panic 终止（panic 可能 longjmp 进 try/except，故先释放设备缓冲）。 */
static SgTensor *sg_gpu_ew_ti_try(int op, const SgTensor *a,
                                  const SgTensor *b, const char *fn,
                                  const char *file, int64_t line) {
    if (op < 0 || op > 2) return NULL; /* 防御：覆盖集外 → CPU */
    if (!a->isInt || !b->isInt) return NULL; /* int kernel 防御 */
    if (a->rank != b->rank) return NULL;
    for (int64_t k = 0; k < a->rank; ++k)
        if (a->shape[k] != b->shape[k]) return NULL;
    int64_t n = sg_tsize(a);
    SgTensor *t = sg_tensor_alloc(1, a->rank, a->shape);
    if (n == 0) return t; /* 空张量：与 CPU 同为空结果，免 launch */
    void *da = sg_gpu_dget((SgTensor *)a, NULL); /* 复用或上传 */
    void *db = sg_gpu_dget((SgTensor *)b, a);    /* 逐出保护 a */
    void *dout = NULL, *dst = NULL;
    int32_t op32 = (int32_t)op, zero = 0, ovf = 0;
    void *args[] = {&dout, &da, &db, &n, &op32, &dst};
    int bad = !da || !db || sg_be.alloc(&dout, (size_t)n * 8) ||
              sg_be.alloc(&dst, 4) || sg_be.h2d(dst, &zero, 4) ||
              sg_be.launch1d(SG_K_TI, n, args) || sg_be.d2h(&ovf, dst, 4);
    if (dst) sg_be.mfree(dst);
    if (bad) {
        if (dout) sg_be.mfree(dout);
        sg_gpu_fail();
        return NULL;
    }
    if (ovf) { /* 溢出：panic 可能 longjmp，先释放未归属的输出块 */
        sg_be.mfree(dout);
        sg_panic(fn, file, line, sg_ti_msg(op));
    }
    if (!sg_gpu_devown(t, dout, (size_t)n * 8)) { /* 惰性回拷 */
        sg_gpu_fail();
        return NULL;
    }
    return t;
}

/* int 逐元素 ts/st（kernel sg_ew_tsi(out,a,s,n,op,sleft,status)；标量取
 * vi 槽位 i64 值——int 覆盖集 s_int=1，标量恒 int；sleft 同 f64 路径） */
static SgTensor *sg_gpu_ew_tsi_try(int op, const SgTensor *a, int64_t s,
                                   int sleft, const char *fn,
                                   const char *file, int64_t line) {
    if (op < 0 || op > 2) return NULL;
    if (!a->isInt) return NULL; /* int kernel 防御 */
    int64_t n = sg_tsize(a);
    SgTensor *t = sg_tensor_alloc(1, a->rank, a->shape);
    if (n == 0) return t;
    void *da = sg_gpu_dget((SgTensor *)a, NULL);
    void *dout = NULL, *dst = NULL;
    int32_t op32 = (int32_t)op, sl32 = (int32_t)sleft, zero = 0, ovf = 0;
    void *args[] = {&dout, &da, &s, &n, &op32, &sl32, &dst};
    int bad = !da || sg_be.alloc(&dout, (size_t)n * 8) ||
              sg_be.alloc(&dst, 4) || sg_be.h2d(dst, &zero, 4) ||
              sg_be.launch1d(SG_K_TSI, n, args) || sg_be.d2h(&ovf, dst, 4);
    if (dst) sg_be.mfree(dst);
    if (bad) {
        if (dout) sg_be.mfree(dout);
        sg_gpu_fail();
        return NULL;
    }
    if (ovf) { /* 溢出：panic 可能 longjmp，先释放未归属的输出块 */
        sg_be.mfree(dout);
        sg_panic(fn, file, line, sg_ti_msg(op));
    }
    if (!sg_gpu_devown(t, dout, (size_t)n * 8)) { /* 惰性回拷 */
        sg_gpu_fail();
        return NULL;
    }
    return t;
}

/* matmul（kernel sg_mm(out,a,b,m,n,k)，m=左行 n=右列 k=内维；shared-memory
 * tiling 32×32：grid = ceil(n/32) × ceil(m/32)，block 32×32，kernel 内
 * p 升序累加与 CPU 逐位一致；k==0 时输入零长 → 8 字节哑分配占位，kernel
 * 不读、累加器恒 0.0 与 CPU 一致；矩形 3×4 @ 4×2 与方形均覆盖） */
static SgTensor *sg_gpu_mm_try(const SgTensor *a, const SgTensor *b) {
    if (a->rank != 2 || b->rank != 2) return NULL; /* 防御：覆盖集外 → CPU */
    if (a->isInt || b->isInt) return NULL;         /* f64 kernel 防御 */
    if (a->shape[1] != b->shape[0]) return NULL; /* 内维不匹配 → CPU panic */
    int64_t m = a->shape[0], k = a->shape[1], n = b->shape[1];
    const int64_t outShape[2] = {m, n};
    SgTensor *t = sg_tensor_alloc(0, 2, outShape);
    int64_t count = m * n;
    if (count == 0) return t; /* 空结果：与 CPU 同，免 launch */
    /* k==0 时输入零长：sg_dev_bytes 占位 8 字节哑分配（kernel 不读） */
    void *da = sg_gpu_dget((SgTensor *)a, NULL); /* 复用或上传 */
    void *db = sg_gpu_dget((SgTensor *)b, a);    /* 逐出保护 a */
    void *dout = NULL;
    void *args[] = {&dout, &da, &db, &m, &n, &k};
    int bad = !da || !db || sg_be.alloc(&dout, (size_t)count * 8) ||
              sg_be.launch2d(SG_K_MM, (n + 31) / 32, (m + 31) / 32, args);
    if (bad) {
        if (dout) sg_be.mfree(dout);
        sg_gpu_fail();
        return NULL;
    }
    if (!sg_gpu_devown(t, dout, (size_t)count * 8)) { /* 惰性回拷 */
        sg_gpu_fail();
        return NULL;
    }
    return t;
}

/* sum（kernel sg_sum(out,a,n)：tid|ctaid==0 单线程 k 升序 fadd 累加，
 * 与 CPU 逐位一致；grid 取 1；空张量和为 0.0 免 launch） */
static double sg_gpu_sum_try(const SgTensor *t, int *ok) {
    *ok = 0;
    if (t->isInt) return 0.0; /* f64 kernel 防御 */
    int64_t n = sg_tsize(t);
    if (n == 0) {
        *ok = 1;
        return 0.0;
    }
    void *da = sg_gpu_dget((SgTensor *)t, NULL); /* 复用或上传 */
    void *dout = NULL;
    double out = 0.0;
    void *args[] = {&dout, &da, &n};
    int bad = !da || sg_be.alloc(&dout, 8) || sg_be.launch1d(SG_K_SUM, 1, args) ||
              sg_be.d2h(&out, dout, 8);
    if (dout) sg_be.mfree(dout);
    if (bad) {
        sg_gpu_fail();
        return 0.0;
    }
    *ok = 1;
    return out;
}

/* int 归约（kernel sg_sum_i(out,status,a,n)：单线程 k 升序
 * sadd.with.overflow 累加，与 sg_tensor_sum_i 逐位一致；溢出经状态缓冲
 * 上报，回读非 0 → 与 CPU 同一 sg_panic 终止） */
static int64_t sg_gpu_sum_i_try(const SgTensor *t, int *ok, const char *fn,
                                const char *file, int64_t line) {
    *ok = 0;
    if (!t->isInt) return 0; /* int kernel 防御 */
    int64_t n = sg_tsize(t);
    if (n == 0) {
        *ok = 1;
        return 0;
    }
    void *da = sg_gpu_dget((SgTensor *)t, NULL); /* 复用或上传 */
    void *dout = NULL, *dst = NULL;
    int64_t out = 0;
    int32_t zero = 0, ovf = 0;
    void *args[] = {&dout, &dst, &da, &n};
    int bad = !da || sg_be.alloc(&dout, 8) || sg_be.alloc(&dst, 4) ||
              sg_be.h2d(dst, &zero, 4) ||
              sg_be.launch1d(SG_K_SUMI, 1, args) || sg_be.d2h(&ovf, dst, 4) ||
              sg_be.d2h(&out, dout, 8);
    if (dout) sg_be.mfree(dout);
    if (dst) sg_be.mfree(dst);
    if (bad) {
        sg_gpu_fail();
        return 0;
    }
    if (ovf) sg_panic(fn, file, line, "整数求和溢出");
    *ok = 1;
    return out;
}

/* 2D 转置（kernel sg_tr(out,a,m,n)：m=输入行数，out[i*m+j]=a[j*n+i]，
 * grid=m*n；输出形状 n×m） */
static SgTensor *sg_gpu_tr_try(const SgTensor *t) {
    if (t->rank != 2) return NULL; /* 防御：覆盖集外 → CPU */
    if (t->isInt) return NULL;     /* f64 kernel 防御 */
    int64_t m = t->shape[0], n = t->shape[1];
    const int64_t outShape[2] = {n, m};
    SgTensor *r = sg_tensor_alloc(0, 2, outShape);
    int64_t count = m * n;
    if (count == 0) return r;
    void *da = sg_gpu_dget((SgTensor *)t, NULL); /* 复用或上传 */
    void *dout = NULL;
    void *args[] = {&dout, &da, &m, &n};
    int bad = !da || sg_be.alloc(&dout, (size_t)count * 8) ||
              sg_be.launch1d(SG_K_TR, count, args);
    if (bad) {
        if (dout) sg_be.mfree(dout);
        sg_gpu_fail();
        return NULL;
    }
    if (!sg_gpu_devown(r, dout, (size_t)count * 8)) { /* 惰性回拷 */
        sg_gpu_fail();
        return NULL;
    }
    return r;
}

/* max/min 归约（kernel sg_red_f/sg_red_i：单线程 k 升序比较，与 CPU 逐位
 * 一致；空张量返回未成功 → CPU 以 "%s() 不支持空张量" panic——消息含 name
 * 形参，须由 CPU 侧产生） */
static double sg_gpu_red_f_try(int isMax, const SgTensor *t, int *ok) {
    *ok = 0;
    if (t->isInt) return 0.0; /* f64 kernel 防御 */
    int64_t n = sg_tsize(t);
    if (n == 0) return 0.0; /* 空张量 → CPU panic */
    void *da = sg_gpu_dget((SgTensor *)t, NULL);
    void *dout = NULL;
    double out = 0.0;
    int32_t mx = (int32_t)isMax;
    void *args[] = {&dout, &da, &n, &mx};
    int bad = !da || sg_be.alloc(&dout, 8) ||
              sg_be.launch1d(SG_K_REDF, 1, args) || sg_be.d2h(&out, dout, 8);
    if (dout) sg_be.mfree(dout);
    if (bad) {
        sg_gpu_fail();
        return 0.0;
    }
    *ok = 1;
    return out;
}

static int64_t sg_gpu_red_i_try(int isMax, const SgTensor *t, int *ok) {
    *ok = 0;
    if (!t->isInt) return 0; /* int kernel 防御 */
    int64_t n = sg_tsize(t);
    if (n == 0) return 0;
    void *da = sg_gpu_dget((SgTensor *)t, NULL);
    void *dout = NULL;
    int64_t out = 0;
    int32_t mx = (int32_t)isMax;
    void *args[] = {&dout, &da, &n, &mx};
    int bad = !da || sg_be.alloc(&dout, 8) ||
              sg_be.launch1d(SG_K_REDI, 1, args) || sg_be.d2h(&out, dout, 8);
    if (dout) sg_be.mfree(dout);
    if (bad) {
        sg_gpu_fail();
        return 0;
    }
    *ok = 1;
    return out;
}

/* 一元逐元素（fnid 0=abs 1=sqrt；kernel sg_un_f f64 入 / sg_un_ai int abs /
 * sg_un_si int 入 sitofp+sqrt）：输出 dtype 规则与 CPU 一致（abs 保 dtype，
 * sqrt 恒 f64）；空张量与 CPU 同为空结果免 launch */
static SgTensor *sg_gpu_un_try(int fnid, const SgTensor *t) {
    if (fnid < 0 || fnid > 1) return NULL; /* 防御：covers 外 → CPU */
    int64_t n = sg_tsize(t);
    SgTensor *r =
        sg_tensor_alloc((fnid == 0 && t->isInt) ? 1 : 0, t->rank, t->shape);
    if (n == 0) return r;
    void *da = sg_gpu_dget((SgTensor *)t, NULL);
    void *dout = NULL;
    int32_t fn32 = (int32_t)fnid;
    void *args[] = {&dout, &da, &n, &fn32}; /* un_ai/un_si 仅消费前 3 参 */
    int k = t->isInt ? (fnid == 0 ? SG_K_UNAI : SG_K_UNSI) : SG_K_UNF;
    int bad = !da || sg_be.alloc(&dout, (size_t)n * 8) ||
              sg_be.launch1d(k, n, args);
    if (bad) {
        if (dout) sg_be.mfree(dout);
        sg_gpu_fail();
        return NULL;
    }
    if (!sg_gpu_devown(r, dout, (size_t)n * 8)) { /* 惰性回拷 */
        sg_gpu_fail();
        return NULL;
    }
    return r;
}

/* 部分索引切片（kernel sg_cpy 设备内连续拷贝）：索引校验复用 sg_tflat——
 * 越界 panic 消息与 CPU 逐字一致（同一函数产生）；秩超限返回 NULL → CPU
 * panic。输出设备驻留 hostStale=1 惰性回拷，链式 GPU op 免回拷直用。 */
static SgTensor *sg_gpu_slice_try(SgTensor *t, int64_t n, const int64_t *idx,
                                  const char *fn, const char *file,
                                  int64_t line) {
    if (n > t->rank) return NULL; /* CPU panic "下标数量…超过张量秩…" */
    int64_t flat = sg_tflat(t, n, idx, fn, file, line); /* 越界在此 panic（同 CPU） */
    int64_t rest = 1;
    for (int64_t k = n; k < t->rank; ++k) rest *= t->shape[k];
    SgTensor *s = sg_tensor_alloc(t->isInt ? 1 : 0, t->rank - n, t->shape + n);
    if (rest == 0) return s; /* 空结果：与 CPU 同，免 launch */
    int64_t off = flat * rest; /* 部分索引为前缀平坦值：起始元素 = flat*rest（同 CPU） */
    void *da = sg_gpu_dget(t, NULL);
    void *dout = NULL;
    void *args[] = {&dout, &da, &off, &rest};
    int bad = !da || sg_be.alloc(&dout, (size_t)rest * 8) ||
              sg_be.launch1d(SG_K_CPY, rest, args);
    if (bad) {
        if (dout) sg_be.mfree(dout);
        sg_gpu_fail();
        return NULL;
    }
    if (!sg_gpu_devown(s, dout, (size_t)rest * 8)) { /* 惰性回拷 */
        sg_gpu_fail();
        return NULL;
    }
    return s;
}

/* 下标读（8 字节 DtoH 直读设备驻留张量，免整张量回拷）：host 驻留返回 0 →
 * CPU 直读（上传整张量反而更慢）；索引校验复用 sg_tflat（panic 同 CPU） */
static int sg_gpu_get_try(const SgTensor *t, int64_t n, const int64_t *idx,
                          const char *fn, const char *file, int64_t line,
                          void *out) {
    int64_t flat = sg_tflat(t, n, idx, fn, file, line);
    if (!t->dev) return 0; /* host 驻留：CPU 直读更快 */
    if (sg_be.d2h(out, (char *)t->dev + (size_t)flat * 8, 8)) {
        sg_gpu_fail(); /* 拷贝通道随上下文失败：回退 CPU 读（sg_tsync 拉取） */
        return 0;
    }
    return 1;
}

/* 下标写（8 字节 HtoD 直写设备驻留张量）：设备成为新权威（hostStale=1），
 * 与 dget/tdirty 不变量一致；host 驻留返回 0 → CPU host 写；秩不匹配返回
 * 0 → CPU panic（消息含秩与个数）。 */
static int sg_gpu_set_try(SgTensor *t, int64_t n, const int64_t *idx,
                          const void *v, const char *fn, const char *file,
                          int64_t line) {
    if (n != t->rank) return 0;
    int64_t flat = sg_tflat(t, n, idx, fn, file, line);
    if (!t->dev) return 0;
    if (sg_be.h2d((char *)t->dev + (size_t)flat * 8, v, 8)) {
        sg_gpu_fail();
        return 0;
    }
    t->hostStale = 1;
    return 1;
}

#else
/* 非 Windows：第 1 层 GPU 路径暂不实现（ready 恒 0、try 桩永不触达），
 * 下方公开分派直通 CPU 实现。 */
static int sg_gpu_ready(void) { return 0; }
static SgTensor *sg_gpu_ew_tt_try(int op, const SgTensor *a,
                                  const SgTensor *b) {
    (void)op;
    (void)a;
    (void)b;
    return NULL;
}
static SgTensor *sg_gpu_ew_ts_try(int op, const SgTensor *a, double s,
                                  int sleft) {
    (void)op;
    (void)a;
    (void)s;
    (void)sleft;
    return NULL;
}
static SgTensor *sg_gpu_mm_try(const SgTensor *a, const SgTensor *b) {
    (void)a;
    (void)b;
    return NULL;
}
static double sg_gpu_sum_try(const SgTensor *t, int *ok) {
    (void)t;
    *ok = 0;
    return 0.0;
}
static SgTensor *sg_gpu_tr_try(const SgTensor *t) {
    (void)t;
    return NULL;
}
static SgTensor *sg_gpu_ew_ti_try(int op, const SgTensor *a,
                                  const SgTensor *b, const char *fn,
                                  const char *file, int64_t line) {
    (void)op;
    (void)a;
    (void)b;
    (void)fn;
    (void)file;
    (void)line;
    return NULL;
}
static SgTensor *sg_gpu_ew_tsi_try(int op, const SgTensor *a, int64_t s,
                                   int sleft, const char *fn,
                                   const char *file, int64_t line) {
    (void)op;
    (void)a;
    (void)s;
    (void)sleft;
    (void)fn;
    (void)file;
    (void)line;
    return NULL;
}
static int64_t sg_gpu_sum_i_try(const SgTensor *t, int *ok, const char *fn,
                                const char *file, int64_t line) {
    (void)t;
    (void)fn;
    (void)file;
    (void)line;
    *ok = 0;
    return 0;
}
static double sg_gpu_red_f_try(int isMax, const SgTensor *t, int *ok) {
    (void)isMax;
    (void)t;
    *ok = 0;
    return 0.0;
}
static int64_t sg_gpu_red_i_try(int isMax, const SgTensor *t, int *ok) {
    (void)isMax;
    (void)t;
    *ok = 0;
    return 0;
}
static SgTensor *sg_gpu_un_try(int fnid, const SgTensor *t) {
    (void)fnid;
    (void)t;
    return NULL;
}
static SgTensor *sg_gpu_slice_try(SgTensor *t, int64_t n, const int64_t *idx,
                                  const char *fn, const char *file,
                                  int64_t line) {
    (void)t;
    (void)n;
    (void)idx;
    (void)fn;
    (void)file;
    (void)line;
    return NULL;
}
static int sg_gpu_get_try(const SgTensor *t, int64_t n, const int64_t *idx,
                          const char *fn, const char *file, int64_t line,
                          void *out) {
    (void)t;
    (void)n;
    (void)idx;
    (void)fn;
    (void)file;
    (void)line;
    (void)out;
    return 0;
}
static int sg_gpu_set_try(SgTensor *t, int64_t n, const int64_t *idx,
                          const void *v, const char *fn, const char *file,
                          int64_t line) {
    (void)t;
    (void)n;
    (void)idx;
    (void)v;
    (void)fn;
    (void)file;
    (void)line;
    return 0;
}
#endif

/* ---- 公开分派（ABI 与对应 sg_* 逐一相同；覆盖集 op 由类型检查器保证 ----
 * 形状合法；f64 路径无 panic，int 路径溢出经状态缓冲回读后以与 CPU 同一
 * sg_panic 终止；GPU 就绪则按 dtype 分流尝试 GPU（混合 dtype 覆盖集外，
 * 防御性直通 CPU），任何失败当次即回退，输出逐位一致） ---- */

SgTensor *sg_gpu_tensor_binop_tt(int op, SgTensor *a, SgTensor *b,
                                 const char *fn, const char *file,
                                 int64_t line) {
    if (sg_gpu_ready()) {
        SgTensor *r = NULL;
        if (a->isInt && b->isInt) /* int 覆盖集：溢出 panic 语义与 CPU 一致 */
            r = sg_gpu_ew_ti_try(op, a, b, fn, file, line);
        else if (!a->isInt && !b->isInt) /* f64 覆盖集 */
            r = sg_gpu_ew_tt_try(op, a, b);
        if (r) return r;
    }
    return sg_tensor_binop_tt(op, a, b, fn, file, line);
}

SgTensor *sg_gpu_tensor_binop_ts(int op, SgTensor *a, int si64, int64_t vi,
                                 double vf, const char *fn, const char *file,
                                 int64_t line) {
    if (sg_gpu_ready()) {
        SgTensor *r = NULL;
        if (a->isInt && si64) /* int 覆盖集：标量取 vi 槽位 i64 值 */
            r = sg_gpu_ew_tsi_try(op, a, vi, 0, fn, file, line);
        else if (!a->isInt && !si64) /* f64 覆盖集：标量恒取 vf 浮点值 */
            r = sg_gpu_ew_ts_try(op, a, vf, 0);
        if (r) return r;
    }
    return sg_tensor_binop_ts(op, a, si64, vi, vf, fn, file, line);
}

SgTensor *sg_gpu_tensor_binop_st(int op, int si64, int64_t vi, double vf,
                                 SgTensor *b, const char *fn, const char *file,
                                 int64_t line) {
    if (sg_gpu_ready()) {
        SgTensor *r = NULL;
        if (b->isInt && si64) /* int 覆盖集：标量在左，张量侧 b，sleft=1 */
            r = sg_gpu_ew_tsi_try(op, b, vi, 1, fn, file, line);
        else if (!b->isInt && !si64) /* f64 覆盖集 */
            r = sg_gpu_ew_ts_try(op, b, vf, 1);
        if (r) return r;
    }
    return sg_tensor_binop_st(op, si64, vi, vf, b, fn, file, line);
}

SgTensor *sg_gpu_tensor_matmul(SgTensor *a, SgTensor *b, const char *fn,
                               const char *file, int64_t line) {
    (void)fn;
    (void)file;
    (void)line;
    if (sg_gpu_ready()) {
        SgTensor *r = sg_gpu_mm_try(a, b);
        if (r) return r;
    }
    return sg_tensor_matmul(a, b, fn, file, line);
}

double sg_gpu_tensor_sum_f(SgTensor *t) {
    if (sg_gpu_ready()) {
        int ok = 0;
        double r = sg_gpu_sum_try(t, &ok);
        if (ok) return r;
    }
    return sg_tensor_sum_f(t);
}

int64_t sg_gpu_tensor_sum_i(SgTensor *t, const char *fn, const char *file,
                            int64_t line) {
    if (sg_gpu_ready()) {
        int ok = 0;
        int64_t r = sg_gpu_sum_i_try(t, &ok, fn, file, line);
        if (ok) return r;
    }
    return sg_tensor_sum_i(t, fn, file, line);
}

SgTensor *sg_gpu_tensor_transpose(SgTensor *t, const char *fn, const char *file,
                                  int64_t line) {
    (void)fn;
    (void)file;
    (void)line;
    if (sg_gpu_ready()) {
        SgTensor *r = sg_gpu_tr_try(t);
        if (r) return r;
    }
    return sg_tensor_transpose(t, fn, file, line);
}

int64_t sg_gpu_tensor_reduce_i(int isMax, SgTensor *t, const char *name,
                               const char *fn, const char *file,
                               int64_t line) {
    if (sg_gpu_ready()) {
        int ok = 0;
        int64_t r = sg_gpu_red_i_try(isMax, t, &ok);
        if (ok) return r; /* 空张量 ok=0 → CPU panic（消息含 name） */
    }
    return sg_tensor_reduce_i(isMax, t, name, fn, file, line);
}

double sg_gpu_tensor_reduce_f(int isMax, SgTensor *t, const char *name,
                              const char *fn, const char *file,
                              int64_t line) {
    if (sg_gpu_ready()) {
        int ok = 0;
        double r = sg_gpu_red_f_try(isMax, t, &ok);
        if (ok) return r;
    }
    return sg_tensor_reduce_f(isMax, t, name, fn, file, line);
}

SgTensor *sg_gpu_tensor_un(int fnid, SgTensor *t, const char *fn,
                           const char *file, int64_t line) {
    if (sg_gpu_ready()) {
        SgTensor *r = sg_gpu_un_try(fnid, t);
        if (r) return r;
    }
    return sg_tensor_un(fnid, t, fn, file, line);
}

SgTensor *sg_gpu_tensor_slice(SgTensor *t, int64_t n, const int64_t *idx,
                              const char *fn, const char *file,
                              int64_t line) {
    if (sg_gpu_ready()) {
        SgTensor *r = sg_gpu_slice_try(t, n, idx, fn, file, line);
        if (r) return r;
    }
    return sg_tensor_slice(t, n, idx, fn, file, line);
}

int64_t sg_gpu_tensor_get_i(SgTensor *t, int64_t n, const int64_t *idx,
                            const char *fn, const char *file, int64_t line) {
    if (sg_gpu_ready()) {
        int64_t v = 0;
        if (sg_gpu_get_try(t, n, idx, fn, file, line, &v)) return v;
    }
    return sg_tensor_get_i(t, n, idx, fn, file, line);
}

double sg_gpu_tensor_get_f(SgTensor *t, int64_t n, const int64_t *idx,
                           const char *fn, const char *file, int64_t line) {
    if (sg_gpu_ready()) {
        double v = 0.0;
        if (sg_gpu_get_try(t, n, idx, fn, file, line, &v)) return v;
    }
    return sg_tensor_get_f(t, n, idx, fn, file, line);
}

void sg_gpu_tensor_set_i(SgTensor *t, int64_t n, const int64_t *idx,
                         int64_t v, const char *fn, const char *file,
                         int64_t line) {
    if (sg_gpu_ready()) {
        if (sg_gpu_set_try(t, n, idx, &v, fn, file, line)) return;
    }
    sg_tensor_set_i(t, n, idx, v, fn, file, line);
}

void sg_gpu_tensor_set_f(SgTensor *t, int64_t n, const int64_t *idx,
                         double v, const char *fn, const char *file,
                         int64_t line) {
    if (sg_gpu_ready()) {
        if (sg_gpu_set_try(t, n, idx, &v, fn, file, line)) return;
    }
    sg_tensor_set_f(t, n, idx, v, fn, file, line);
}
)GP";

// ----------------------------------------------------------------------------
// 外部命令驱动
// ----------------------------------------------------------------------------

/// 执行外部命令并捕获合并输出（stdout+stderr → 捕获文件 → 读回 → 删除）。
int runCapture(const std::string& exe, const std::string& args,
               const fs::path& capPath, std::string& out) {
    std::error_code ec;
    fs::remove(capPath, ec);
    // cmd /c 对以引号开头且含特殊字符的命令会剥首尾引号——外层再包一对
    // 引号（cmd 官方文档的标准规避法），内层各路径引号原样保留。
    std::string cmd = "\"\"" + exe + "\" " + args + " > \"" + capPath.string() +
                      "\" 2>&1\"";
    int rc = std::system(cmd.c_str());
    out.clear();
    std::ifstream in(capPath, std::ios::binary);
    if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
    }
    fs::remove(capPath, ec);
    return rc;
}

/// 内嵌文本 → C 字符串字面量体（换行 → "\n" 续段，转义反斜杠与引号；
/// 用于把 kernel PTX 原文嵌入生成的 _rt.c，逐行可读便于检视）。
std::string cStringLiteral(const char* raw) {
    std::string out;
    for (const char* p = raw; *p; ++p) {
        switch (*p) {
        case '\n': out += "\\n\"\n\""; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        default: out += *p; break;
        }
    }
    return out;
}

// ----------------------------------------------------------------------------
// AOT obj 增量缓存（设计文档 5.2：MLIR→LLVM→object 按（模块, 目标, 优化档）
// 缓存，链接必付）。缓存是加速项而非正确性依赖——读写失败一律静默降级，
// 对齐 .sgbc 缓存纪律（src/Tool/Runner.cpp）。
// ----------------------------------------------------------------------------

/// 缓存键格式版本：键成分或 obj 语义变化时递增，旧缓存整体失效。
constexpr const char* kObjCacheVer = "scourge-aot-obj-v1";

/// fnv1a64 包装（重载歧义防护）：bc::fnv1a64 有 (string_view, seed) 与
/// (const void*, len, seed=默认) 两个重载——const char* 实参会精确匹配后者，
/// seed 被当作 len 造成野读崩溃。字符串键一律经此包装显式走 string_view。
inline uint64_t fnvKey(std::string_view s,
                       uint64_t seed = 0xcbf29ce484222325ull) {
    return bc::fnv1a64(s, seed);
}

/// 工具链标签：clang 路径 + 大小 + 修改时间——工具链升级即缓存失效。
std::string clangTag(const fs::path& clang) {
    std::error_code ec;
    uintmax_t size = fs::file_size(clang, ec);
    uint64_t sizeV = ec ? 0 : static_cast<uint64_t>(size);
    ec.clear();
    auto t = fs::last_write_time(clang, ec);
    uint64_t mtime = ec ? 0 : static_cast<uint64_t>(t.time_since_epoch().count());
    return clang.string() + "|" + std::to_string(sizeV) + "|" +
           std::to_string(mtime);
}

/// main.obj 缓存键（五重一致性，对齐 .sgbc 五重校验）：格式版本、
/// 工具链标签、源指纹（依赖层序逐模块：名/路径/归一化源文本）、
/// 优化档+gpuMode、入口。任一不符 → 未命中，全量重编。
uint64_t mainObjKey(const std::string& tag, bool gpuMode,
                    const std::string& entry,
                    const std::vector<std::string>& order,
                    const Session& session) {
    uint64_t h = fnvKey(kObjCacheVer);
    // 编译器构建指纹（本 TU 编译时刻）：编译器任何重建（MLIRGen/AOT 代码
    // 生成行为变化）自动失效全部 obj 缓存——仅哈希源文本会让编译器升级后
    // 仍命中旧 obj（曾致验证脚本 gpu_syms 假阴性、mlir_same 假阳性）。
    h = fnvKey(__DATE__ " " __TIME__, h);
    h = fnvKey(tag, h);
    h = fnvKey(gpuMode ? "gpu" : "cpu", h);
    h = fnvKey("-O2", h);
    h = fnvKey(entry, h);
    for (const auto& name : order) {
        h = fnvKey(name, h);
        h = fnvKey(session.loadedModulePath(name), h);
        h = fnvKey(session.loadedSource(name), h);
    }
    return h;
}

/// rt.obj 缓存键（源无关）：格式版本、工具链标签、gpuMode、运行时源文本。
/// kRuntimeC / kGpuRuntimeC / kGpuPtxC / kGpuOpenCLC 内嵌于编译器二进制，文本
/// 哈希入键——开发期改动运行时源自动失效，无需手工递增版本号。
uint64_t rtObjKey(const std::string& tag, bool gpuMode) {
    uint64_t h = fnvKey(kObjCacheVer);
    h = fnvKey(tag, h);
    h = fnvKey(gpuMode ? "gpu" : "cpu", h);
    h = fnvKey(kRuntimeC, h);
    if (gpuMode) {
        h = fnvKey(kGpuRuntimeC, h);
        h = fnvKey(kGpuPtxC, h);
        h = fnvKey(kGpuOpenCLC, h);
    }
    return h;
}

/// 缓存键文件（十进制单行文本）。读取不符/失败即未命中。
bool readObjKey(const fs::path& p, uint64_t expect) {
    std::ifstream in(p);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line)) return false;
    return line == std::to_string(expect);
}

/// 写键（失败静默：下次按未命中全量重编，不影响正确性）。
void writeObjKey(const fs::path& p, uint64_t v) {
    std::ofstream of(p, std::ios::binary);
    if (of) of << v << "\n";
}

} // namespace

// ----------------------------------------------------------------------------
// 构建主流程
// ----------------------------------------------------------------------------

BuildOutcome buildOnce(const fs::path& inPath, const fs::path& outPath,
                       bool gpuMode, bool useCache) {
    BuildOutcome b;
    b.exe = outPath;
    const std::string entry = inPath.stem().string();

    // ---- 工具链定位（SCOURGE_LLVM_BIN 可覆盖；缓存键成分，最先确定） ----
    const char* envBin = std::getenv("SCOURGE_LLVM_BIN");
    fs::path llvmBin =
        (envBin && *envBin) ? fs::path(envBin) : fs::path("D:/LLVM/bin");
    fs::path clang = llvmBin / "clang.exe";
    {
        std::error_code ec;
        if (!fs::exists(clang, ec)) {
            b.log = "构建中止：未找到 " + clang.string() +
                    "（可用环境变量 SCOURGE_LLVM_BIN 指定 LLVM bin 目录）\n";
            b.exitCode = 1;
            return b;
        }
    }

    // ---- 阶段 0：加载（解析）。全量检查延后至缓存未命中——与 run 缓存同
    //      纪律：命中即跳过检查/MLIR/转换/编译（源未变则上次构建已过门禁），
    //      仅付链接开销。 ----
    Session session;
    if (!session.load(inPath)) {
        session.diag().emitText(b.log);
        b.exitCode = 1;
        return b;
    }
    auto order = session.modulesInDependencyOrder(entry);

    // ---- 中间产物路径（与可执行同目录，便于检视） ----
    fs::path dir = outPath.parent_path();
    if (dir.empty()) dir = ".";
    std::string stem = outPath.stem().string();
    fs::path mlirPath = dir / (stem + ".mlir");
    fs::path llPath = dir / (stem + ".ll");
    fs::path rtPath = dir / (stem + "_rt.c");
    fs::path capPath = dir / (stem + ".buildcap");

    // ---- AOT obj 缓存目录（<stem>.sgbuild/obj，设计第零/五章命名）。
    //      目录不可建（权限/占用）→ 静默降级为无缓存单步直编。 ----
    fs::path cacheDir = dir / (stem + ".sgbuild") / "obj";
    bool cacheOk = false;
    if (useCache) {
        std::error_code ec;
        fs::create_directories(cacheDir, ec);
        cacheOk = !ec;
    }
    const uint64_t keyMain =
        mainObjKey(clangTag(clang), gpuMode, entry, order, session);
    const uint64_t keyRt = rtObjKey(clangTag(clang), gpuMode);

    std::string toolOut;

    // ---- 缓存命中判定：双 obj 在盘且双键全等才链接（显式拒绝，无静默回退） ----
    if (cacheOk) {
        fs::path objMain = cacheDir / "main.obj";
        fs::path objRt = cacheDir / "rt.obj";
        std::error_code ec;
        if (fs::exists(objMain, ec) && fs::exists(objRt, ec) &&
            readObjKey(cacheDir / "main.key", keyMain) &&
            readObjKey(cacheDir / "rt.key", keyRt)) {
            int rc = runCapture(clang.string(),
                                "\"" + objMain.string() + "\" \"" +
                                    objRt.string() + "\" -o \"" +
                                    outPath.string() + "\"",
                                capPath, toolOut);
            if (rc == 0) {
                b.fromCache = true;
                return b;
            }
            // 缓存对象损坏（链接失败）→ 失效整目录，就地回退全量重建。
            b.log = "AOT 缓存命中但链接失败，已失效缓存并全量重建\n" + toolOut;
            fs::remove_all(cacheDir, ec);
            fs::create_directories(cacheDir, ec);
            cacheOk = !ec;
        }
    }

    // ---- 阶段 1：全量检查（与 check/run 同一门禁；命中路径不付此开销） ----
    session.check();
    if (session.diag().hasErrors()) {
        session.diag().emitText(b.log);
        b.exitCode = 1;
        return b;
    }

    // ---- 阶段 2：AST → 文本 MLIR（不支持构造显式 E1007） ----
    std::string mlir;
    if (!aot::generateMLIR(session, entry, mlir)) {
        session.diag().emitText(b.log);
        b.exitCode = 1;
        return b;
    }

    {
        std::ofstream f(mlirPath, std::ios::binary);
        f << mlir;
    }
    {
        std::ofstream f(rtPath, std::ios::binary);
        f << kRuntimeC;
        if (gpuMode) {
            /* GPU 分派层：内嵌 kernel 源（Windows 分支内定义，供 JIT 加载）
             * + 后端抽象分派函数（覆盖集 op，失败回退 CPU） */
            f << "\n#ifdef _WIN32\n/* 内嵌 kernel PTX（llc sm_120 产物，"
                 "cuModuleLoadData JIT） */\nstatic const char sg_gpu_ptx[] =\n\""
              << cStringLiteral(kGpuPtxC) << "\";\n"
                 "/* 内嵌 OpenCL C kernel 源（clBuildProgram 运行时 JIT，"
                 "与 sg_kernels.ll 镜像） */\nstatic const char sg_gpu_oclc[] =\n\""
              << cStringLiteral(kGpuOpenCLC) << "\";\n#endif\n"
              << kGpuRuntimeC;
        }
    }

    // ---- 阶段 3（合并）：进程内 MLIR → LLVM dialect → LLVM IR ----
    // gpuMode：GPU 覆盖集 op 保持 lang 层直达 LangToLLVM（调用换 sg_gpu_*，
    // 内联降级跳过）；.mlir 落盘产物与 CPU 模式逐字节相同。
    if (!aot::convertToLLVMInProcess(mlir, llPath.string(), toolOut, gpuMode)) {
        std::ofstream cap(capPath, std::ios::binary);
        cap << toolOut;
        b.log = "构建中止：MLIR → LLVM IR 进程内转换失败\n" + toolOut;
        b.exitCode = 1;
        return b;
    }

    // ---- 阶段 4+5：clang 分段编译 → 增量链接（缓存可用路径） ----
    // main.obj ← .ll（依赖源+工具链+优化档）；rt.obj ← _rt.c（源无关）。
    // obj 编译即写缓存（产物落缓存目录），链接自缓存目录取用。
    if (cacheOk) {
        fs::path objMain = cacheDir / "main.obj";
        fs::path objRt = cacheDir / "rt.obj";
        int rc = runCapture(
            clang.string(),
            "-c -O2 -Wno-override-module -D_CRT_SECURE_NO_WARNINGS \"" +
                llPath.string() + "\" -o \"" + objMain.string() + "\"",
            capPath, toolOut);
        if (rc != 0) {
            b.log = "构建中止：clang 编译 " + llPath.string() +
                    " 失败（exit " + std::to_string(rc) + "）\n" + toolOut;
            b.exitCode = 1;
            return b;
        }
        rc = runCapture(clang.string(),
                        "-c -O2 -D_CRT_SECURE_NO_WARNINGS \"" +
                            rtPath.string() + "\" -o \"" + objRt.string() +
                            "\"",
                        capPath, toolOut);
        if (rc != 0) {
            b.log = "构建中止：clang 编译运行时失败（exit " +
                    std::to_string(rc) + "）\n" + toolOut;
            b.exitCode = 1;
            return b;
        }
        // obj 落盘成功后写键（写失败静默——下次按未命中处理）。
        writeObjKey(cacheDir / "main.key", keyMain);
        writeObjKey(cacheDir / "rt.key", keyRt);

        rc = runCapture(clang.string(),
                        "\"" + objMain.string() + "\" \"" + objRt.string() +
                            "\" -o \"" + outPath.string() + "\"",
                        capPath, toolOut);
        if (rc != 0) {
            b.log = "构建中止：clang 链接失败（exit " + std::to_string(rc) +
                    "）\n" + toolOut;
            b.exitCode = 1;
            return b;
        }
        return b; // exitCode 0
    }

    // ---- 静默降级路径：单步编译链接（原命令形，probe4 验证过） ----
    int rc = runCapture(
        clang.string(),
        "-O2 -Wno-override-module -D_CRT_SECURE_NO_WARNINGS \"" +
            llPath.string() + "\" \"" + rtPath.string() +
            "\" -o \"" + outPath.string() + "\"",
        capPath, toolOut);
    if (rc != 0) {
        b.log = "构建中止：clang 链接失败（exit " + std::to_string(rc) +
                "）\n" + toolOut;
        b.exitCode = 1;
        return b;
    }

    return b; // exitCode 0
}

} // namespace scourge
