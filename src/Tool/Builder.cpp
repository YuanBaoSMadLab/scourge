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
} SgTensor;

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
    return t;
}

/* ---- LangToLinalg 桥（AOT 步骤 3：linalg 降级的零拷贝输出/数据直取） ---- */

/* 数据指针直取（按 isInt 分派 i64/f64 存储；LangToLinalg 输入桥用） */
void* sg_tensor_data(SgTensor* t) {
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
    for (int64_t k = 0; k < nrows; ++k) allInt &= rows[k]->isInt;

    SgTensor* t = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    t->rank = r0->rank + 1;
    t->shape = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)t->rank);
    t->shape[0] = nrows;
    for (int64_t d = 0; d < r0->rank; ++d) t->shape[d + 1] = r0->shape[d];
    t->isInt = allInt;
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
    SgTensor* t = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    t->rank = a->rank;
    t->shape = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)a->rank);
    memcpy(t->shape, a->shape, sizeof(int64_t) * (size_t)a->rank);
    t->isInt = outInt;
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
    SgTensor* t = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    t->rank = 2;
    t->shape = (int64_t*)sg_xmalloc(sizeof(int64_t) * 2);
    t->shape[0] = m;
    t->shape[1] = n;
    t->isInt = a->isInt && b->isInt;
    const int64_t count = m * n;
    if (t->isInt) {
        t->i64 = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)count);
        t->f64 = 0;
        for (int64_t i = 0; i < m; ++i)
            for (int64_t j = 0; j < n; ++j) {
                int64_t acc = 0;
                for (int64_t p = 0; p < k; ++p) {
                    int64_t prod = 0;
                    if (__builtin_mul_overflow(
                            a->i64[i * k + p], b->i64[p * n + j], &prod) ||
                        __builtin_add_overflow(acc, prod, &acc))
                        sg_panic(fn, file, line, "整数乘法溢出");
                }
                t->i64[i * n + j] = acc;
            }
    } else {
        t->f64 = (double*)sg_xmalloc(sizeof(double) * (size_t)count);
        t->i64 = 0;
        for (int64_t i = 0; i < m; ++i)
            for (int64_t j = 0; j < n; ++j) {
                double acc = 0.0;
                for (int64_t p = 0; p < k; ++p)
                    acc += sg_tget_f64(a, i * k + p) *
                           sg_tget_f64(b, p * n + j);
                t->f64[i * n + j] = acc;
            }
    }
    return t;
}

int64_t sg_tensor_get_i(SgTensor* t, int64_t n, const int64_t* idx,
                        const char* fn, const char* file, int64_t line) {
    return t->i64[sg_tflat(t, n, idx, fn, file, line)];
}

double sg_tensor_get_f(SgTensor* t, int64_t n, const int64_t* idx,
                       const char* fn, const char* file, int64_t line) {
    return sg_tget_f64(t, sg_tflat(t, n, idx, fn, file, line));
}

/* 部分索引 → 子张量拷贝（行主序连续切片，与 VM tensorIndexRead 一致） */
SgTensor* sg_tensor_slice(SgTensor* t, int64_t n, const int64_t* idx,
                          const char* fn, const char* file, int64_t line) {
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
    t->i64[sg_tflat(t, n, idx, fn, file, line)] = v;
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
    t->f64[sg_tflat(t, n, idx, fn, file, line)] = v;
}

int64_t sg_tensor_sum_i(SgTensor* t, const char* fn, const char* file,
                        int64_t line) {
    int64_t acc = 0;
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
    for (int64_t k = 0, n = sg_tsize(t); k < n; ++k) acc += t->f64[k];
    return acc;
}

/* 逐元素数学：fnid 0=abs 1=sqrt 2=exp 3=log；abs 保 dtype，其余恒 f64 */
SgTensor* sg_tensor_un(int fnid, SgTensor* t, const char* fn,
                       const char* file, int64_t line) {
    const int64_t n = sg_tsize(t);
    SgTensor* r = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    r->rank = t->rank;
    r->shape = (int64_t*)sg_xmalloc(sizeof(int64_t) * (size_t)t->rank);
    memcpy(r->shape, t->shape, sizeof(int64_t) * (size_t)t->rank);
    const int outInt = (fnid == 0) && t->isInt;
    r->isInt = outInt;
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
    SgTensor* r = (SgTensor*)sg_xmalloc(sizeof(SgTensor));
    r->rank = 2;
    r->shape = (int64_t*)sg_xmalloc(sizeof(int64_t) * 2);
    r->shape[0] = c;
    r->shape[1] = m;
    r->isInt = t->isInt;
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
    const int64_t n = sg_tsize(t);
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
)RT";

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

} // namespace

// ----------------------------------------------------------------------------
// 构建主流程
// ----------------------------------------------------------------------------

BuildOutcome buildOnce(const fs::path& inPath, const fs::path& outPath) {
    BuildOutcome b;
    b.exe = outPath;

    // ---- 阶段 0：加载 + 全量检查（与 check/run 同一门禁） ----
    Session session;
    if (!session.load(inPath)) {
        session.diag().emitText(b.log);
        b.exitCode = 1;
        return b;
    }
    session.check();
    if (session.diag().hasErrors()) {
        session.diag().emitText(b.log);
        b.exitCode = 1;
        return b;
    }

    // ---- 阶段 1：AST → 文本 MLIR（不支持构造显式 E1007） ----
    std::string mlir;
    const std::string entry = inPath.stem().string();
    if (!aot::generateMLIR(session, entry, mlir)) {
        session.diag().emitText(b.log);
        b.exitCode = 1;
        return b;
    }

    // ---- 中间产物路径（与可执行同目录，便于检视） ----
    fs::path dir = outPath.parent_path();
    if (dir.empty()) dir = ".";
    std::string stem = outPath.stem().string();
    fs::path mlirPath = dir / (stem + ".mlir");
    fs::path llPath = dir / (stem + ".ll");
    fs::path rtPath = dir / (stem + "_rt.c");
    fs::path capPath = dir / (stem + ".buildcap");

    {
        std::ofstream f(mlirPath, std::ios::binary);
        f << mlir;
    }
    {
        std::ofstream f(rtPath, std::ios::binary);
        f << kRuntimeC;
    }

    // ---- 工具链定位（SCOURGE_LLVM_BIN 可覆盖；MLIR→LLVM IR 已进程内化） ----
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

    // ---- 阶段 2+3（合并）：进程内 MLIR → LLVM dialect → LLVM IR ----
    std::string toolOut;
    if (!aot::convertToLLVMInProcess(mlir, llPath.string(), toolOut)) {
        std::ofstream cap(capPath, std::ios::binary);
        cap << toolOut;
        b.log = "构建中止：MLIR → LLVM IR 进程内转换失败\n" + toolOut;
        b.exitCode = 1;
        return b;
    }

    // ---- 阶段 4：clang 编译链接（probe4 验证过的命令形） ----
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
