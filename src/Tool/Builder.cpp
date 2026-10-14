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
// CUDA 第 1 层：GPU kernel PTX 与分派运行时（--gpu 时追加写入 _rt.c）
// ----------------------------------------------------------------------------
// PTX 由 build-rel/verify/cuda/sg_kernels.ll 经 E:\llvm llc
// (-march=nvptx64 -mcpu=sm_120) 生成——五个形状参数化 kernel：
//   sg_ew_tt / sg_ew_ts（逐元素 + - *，ts/st 经 sleft 选择子方向）、
//   sg_mm（matmul，k 串行升序）、sg_sum（单线程升序归约）、sg_tr（2D 转置）。
// 确定性红线：kernel 浮点运算顺序与 CPU 路径逐位一致（add.rn.f64 IEEE RN，
// mm/sum 串行升序累加），三模输出逐字节可比；sm_120 之外机型 JIT 失败 →
// 运行时自动回退 CPU（语义一致，仅性能降级）。
const char kGpuPtxC[] = R"PTX(//
// Generated by LLVM NVPTX Back-End
//

.version 8.7
.target sm_120
.address_size 64

	// .globl	sg_ew_tt                // -- Begin function sg_ew_tt
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
	.reg .pred 	%p<4>;
	.reg .b32 	%r<9>;
	.reg .b64 	%rd<32>;

// %bb.0:                               // %entry
	ld.param.b64 	%rd6, [sg_mm_param_4];
	ld.param.b64 	%rd8, [sg_mm_param_0];
	ld.param.b64 	%rd9, [sg_mm_param_1];
	cvta.to.global.u64 	%rd1, %rd9;
	ld.param.b64 	%rd10, [sg_mm_param_2];
	cvta.to.global.u64 	%rd2, %rd10;
	cvta.to.global.u64 	%rd3, %rd8;
	ld.param.b64 	%rd11, [sg_mm_param_3];
	mov.u32 	%r1, %tid.x;
	mov.u32 	%r2, %ntid.x;
	mov.u32 	%r3, %ctaid.x;
	cvt.u64.u32 	%rd12, %r1;
	mul.wide.u32 	%rd13, %r3, %r2;
	add.s64 	%rd4, %rd13, %rd12;
	mul.lo.s64 	%rd14, %rd11, %rd6;
	setp.ge.s64 	%p1, %rd4, %rd14;
	@%p1 bra 	$L__BB2_8;
// %bb.1:                               // %do
	ld.param.b64 	%rd7, [sg_mm_param_5];
	or.b64 	%rd15, %rd4, %rd6;
	and.b64 	%rd16, %rd15, -4294967296;
	setp.ne.b64 	%p2, %rd16, 0;
	@!%p2 bra 	$L__BB2_2;
// %bb.3:
	div.s64 	%rd26, %rd4, %rd6;
	mul.lo.s64 	%rd17, %rd26, %rd6;
	sub.s64 	%rd27, %rd4, %rd17;
	bra.uni 	$L__BB2_4;
$L__BB2_2:
	cvt.u32.u64 	%r4, %rd6;
	cvt.u32.u64 	%r5, %rd4;
	div.u32 	%r6, %r5, %r4;
	mul.lo.s32 	%r7, %r6, %r4;
	sub.s32 	%r8, %r5, %r7;
	cvt.u64.u32 	%rd26, %r6;
	cvt.u64.u32 	%rd27, %r8;
$L__BB2_4:                              // %do.split
	shl.b64 	%rd18, %rd27, 3;
	add.s64 	%rd29, %rd2, %rd18;
	shl.b64 	%rd5, %rd6, 3;
	mul.lo.s64 	%rd19, %rd26, %rd7;
	shl.b64 	%rd20, %rd19, 3;
	add.s64 	%rd28, %rd1, %rd20;
	mov.b64 	%rd31, 0d0000000000000000;
	mov.b64 	%rd30, 0;
$L__BB2_5:                              // %lp
                                        // =>This Inner Loop Header: Depth=1
	setp.ge.s64 	%p3, %rd30, %rd7;
	@%p3 bra 	$L__BB2_7;
// %bb.6:                               // %body
                                        //   in Loop: Header=BB2_5 Depth=1
	ld.global.b64 	%rd23, [%rd28];
	ld.global.b64 	%rd24, [%rd29];
	mul.rn.f64 	%rd25, %rd23, %rd24;
	add.rn.f64 	%rd31, %rd31, %rd25;
	add.s64 	%rd30, %rd30, 1;
	add.s64 	%rd29, %rd29, %rd5;
	add.s64 	%rd28, %rd28, 8;
	bra.uni 	$L__BB2_5;
$L__BB2_7:                              // %done
	shl.b64 	%rd21, %rd4, 3;
	add.s64 	%rd22, %rd3, %rd21;
	st.global.b64 	[%rd22], %rd31;
$L__BB2_8:                              // %exit
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
	@%p1 bra 	$L__BB3_5;
// %bb.1:                               // %lp.preheader
	ld.param.b64 	%rd2, [sg_sum_param_2];
	mov.b64 	%rd8, 0d0000000000000000;
	mov.b64 	%rd7, 0;
$L__BB3_2:                              // %lp
                                        // =>This Inner Loop Header: Depth=1
	setp.ge.s64 	%p2, %rd7, %rd2;
	@%p2 bra 	$L__BB3_4;
// %bb.3:                               // %body
                                        //   in Loop: Header=BB3_2 Depth=1
	ld.global.b64 	%rd5, [%rd6];
	add.rn.f64 	%rd8, %rd8, %rd5;
	add.s64 	%rd7, %rd7, 1;
	add.s64 	%rd6, %rd6, 8;
	bra.uni 	$L__BB3_2;
$L__BB3_4:                              // %done
	st.global.b64 	[%rd1], %rd8;
$L__BB3_5:                              // %exit
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
	@%p1 bra 	$L__BB4_5;
// %bb.1:                               // %do
	or.b64 	%rd11, %rd3, %rd4;
	and.b64 	%rd12, %rd11, -4294967296;
	setp.ne.b64 	%p2, %rd12, 0;
	@!%p2 bra 	$L__BB4_2;
// %bb.3:
	div.s64 	%rd20, %rd3, %rd4;
	mul.lo.s64 	%rd13, %rd20, %rd4;
	sub.s64 	%rd21, %rd3, %rd13;
	bra.uni 	$L__BB4_4;
$L__BB4_2:
	cvt.u32.u64 	%r4, %rd4;
	cvt.u32.u64 	%r5, %rd3;
	div.u32 	%r6, %r5, %r4;
	mul.lo.s32 	%r7, %r6, %r4;
	sub.s32 	%r8, %r5, %r7;
	cvt.u64.u32 	%rd20, %r6;
	cvt.u64.u32 	%rd21, %r8;
$L__BB4_4:                              // %do.split
	mad.lo.s64 	%rd14, %rd21, %rd5, %rd20;
	shl.b64 	%rd15, %rd14, 3;
	add.s64 	%rd16, %rd1, %rd15;
	ld.global.b64 	%rd17, [%rd16];
	shl.b64 	%rd18, %rd3, 3;
	add.s64 	%rd19, %rd2, %rd18;
	st.global.b64 	[%rd19], %rd17;
$L__BB4_5:                              // %exit
	ret;
                                        // -- End function
}
)PTX";

// GPU 分派运行时：六个 sg_gpu_* 函数 ABI 与对应 sg_* 逐一相同。sg_gpu_ready
// 惰性初始化（LoadLibraryA nvcuda.dll → driver API（内存/拷贝类一律 _v2 导出）
// → cuInit → PrimaryCtx → cuModuleLoadData JIT 内嵌 PTX）。任何环节失败：
// sg_gpu_state=0 永久回退 CPU 实现（语义一致仅性能降级）；SCOURGE_NO_GPU=1
// 环境变量强制 CPU（逃生门兼回退路径测试钩子）。覆盖集 op 均 f64 且类型
// 检查器已保证形状合法（无 panic 路径），fn/file/line 形参仅为 ABI 对齐。
const char kGpuRuntimeC[] = R"GP(/* ==== CUDA 第 1 层：GPU 分派（f64 静态形状覆盖集） ==== */
#ifdef _WIN32
typedef int CUresult;
typedef struct CUctx_st *CUcontext;
typedef struct CUmod_st *CUmodule;
typedef struct CUfunc_st *CUfunction;
typedef unsigned long long CUdeviceptr;

static CUcontext sg_gpu_ctx = NULL;
static CUmodule sg_gpu_mod = NULL;
static CUfunction sg_gpu_f_tt, sg_gpu_f_ts, sg_gpu_f_mm, sg_gpu_f_sum,
    sg_gpu_f_tr;
static CUresult (*sg_cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*sg_cuMemFree)(CUdeviceptr);
static CUresult (*sg_cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
static CUresult (*sg_cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
static CUresult (*sg_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                     unsigned, unsigned, unsigned, unsigned,
                                     CUdeviceptr, void **, void ***);
static CUresult (*sg_cuCtxSynchronize)(void);
static int sg_gpu_state = -1; /* -1 未初始化 0 不可用 1 就绪 */

static void sg_gpu_fail(void) { sg_gpu_state = 0; }

static void sg_gpu_init(void) {
    sg_gpu_state = 0;
    if (getenv("SCOURGE_NO_GPU")) return; /* 逃生门：强制 CPU */
    HMODULE h = LoadLibraryA("nvcuda.dll");
    if (!h) return;
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
        return;
    if (cuInit(0)) return;
    int dev = 0;
    if (cuDeviceGet(&dev, 0)) return;
    if (cuRetain(&sg_gpu_ctx, dev)) return;
    if (cuSetCtx(sg_gpu_ctx)) return;
    if (cuModLoad(&sg_gpu_mod, sg_gpu_ptx)) return;
    if (cuModFn(&sg_gpu_f_tt, sg_gpu_mod, "sg_ew_tt") ||
        cuModFn(&sg_gpu_f_ts, sg_gpu_mod, "sg_ew_ts") ||
        cuModFn(&sg_gpu_f_mm, sg_gpu_mod, "sg_mm") ||
        cuModFn(&sg_gpu_f_sum, sg_gpu_mod, "sg_sum") ||
        cuModFn(&sg_gpu_f_tr, sg_gpu_mod, "sg_tr"))
        return;
    sg_gpu_state = 1;
}

static int sg_gpu_ready(void) {
    if (sg_gpu_state < 0) sg_gpu_init();
    return sg_gpu_state;
}

/* 一维启动（grid = ceil(n/256) x 256 线程）+ 同步；成功返回 0 */
static int sg_gpu_run1d(CUfunction f, int64_t n, void **args) {
    unsigned th = 256;
    unsigned bl = (unsigned)((n + 255) / 256);
    if (bl == 0) bl = 1;
    if (sg_cuLaunchKernel(f, bl, 1, 1, th, 1, 1, 0, 0, args, NULL))
        return 1;
    return sg_cuCtxSynchronize();
}

/* ---- GPU try 函数：单次尝试，失败置 state=0 并返回 NULL/!ok → 调用方 ----
 * 回退 CPU。args 数组逐项取局部变量地址，顺序与各 kernel 参数一一对应
 * （指针 .u64 / 整形维度 .u64 / op 与方向标志 .u32 / 标量 .f64）。 ---- */

/* 逐元素 tt（kernel sg_ew_tt(out,a,b,n,op)；op 0=+ 1=- 2=*） */
static SgTensor *sg_gpu_ew_tt_try(int op, const SgTensor *a,
                                  const SgTensor *b) {
    if (op < 0 || op > 2) return NULL; /* 防御：覆盖集外 → CPU */
    int64_t n = sg_tsize(a);
    SgTensor *t = sg_tensor_alloc(0, a->rank, a->shape);
    if (n == 0) return t; /* 空张量：与 CPU 同为空结果，免 launch */
    CUdeviceptr da = 0, db = 0, dout = 0;
    int32_t op32 = (int32_t)op;
    void *args[] = {&dout, &da, &db, &n, &op32};
    int bad = sg_cuMemAlloc(&da, (size_t)n * 8) ||
              sg_cuMemcpyHtoD(da, a->f64, (size_t)n * 8) ||
              sg_cuMemAlloc(&db, (size_t)n * 8) ||
              sg_cuMemcpyHtoD(db, b->f64, (size_t)n * 8) ||
              sg_cuMemAlloc(&dout, (size_t)n * 8) ||
              sg_gpu_run1d(sg_gpu_f_tt, n, args) ||
              sg_cuMemcpyDtoH(t->f64, dout, (size_t)n * 8);
    if (da) sg_cuMemFree(da);
    if (db) sg_cuMemFree(db);
    if (dout) sg_cuMemFree(dout);
    if (bad) {
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
    int64_t n = sg_tsize(a);
    SgTensor *t = sg_tensor_alloc(0, a->rank, a->shape);
    if (n == 0) return t;
    CUdeviceptr da = 0, dout = 0;
    int32_t op32 = (int32_t)op, sl32 = (int32_t)sleft;
    void *args[] = {&dout, &da, &s, &n, &op32, &sl32};
    int bad = sg_cuMemAlloc(&da, (size_t)n * 8) ||
              sg_cuMemcpyHtoD(da, a->f64, (size_t)n * 8) ||
              sg_cuMemAlloc(&dout, (size_t)n * 8) ||
              sg_gpu_run1d(sg_gpu_f_ts, n, args) ||
              sg_cuMemcpyDtoH(t->f64, dout, (size_t)n * 8);
    if (da) sg_cuMemFree(da);
    if (dout) sg_cuMemFree(dout);
    if (bad) {
        sg_gpu_fail();
        return NULL;
    }
    return t;
}

/* matmul（kernel sg_mm(out,a,b,m,n,k)，m=左行 n=右列 k=内维，grid=m*n；
 * k==0 时输入零长 → 8 字节哑分配占位，kernel 不读、累加器恒 0.0 与
 * CPU 一致；矩形 3×4 @ 4×2 与方形均覆盖） */
static SgTensor *sg_gpu_mm_try(const SgTensor *a, const SgTensor *b) {
    if (a->rank != 2 || b->rank != 2) return NULL; /* 防御：覆盖集外 → CPU */
    int64_t m = a->shape[0], k = a->shape[1], n = b->shape[1];
    const int64_t outShape[2] = {m, n};
    SgTensor *t = sg_tensor_alloc(0, 2, outShape);
    int64_t count = m * n;
    if (count == 0) return t; /* 空结果：与 CPU 同，免 launch */
    size_t ab = (size_t)(m * k) * 8, bb = (size_t)(k * n) * 8;
    if (ab == 0) ab = 8;
    if (bb == 0) bb = 8;
    CUdeviceptr da = 0, db = 0, dout = 0;
    void *args[] = {&dout, &da, &db, &m, &n, &k};
    int bad = sg_cuMemAlloc(&da, ab) ||
              sg_cuMemcpyHtoD(da, a->f64, ab) ||
              sg_cuMemAlloc(&db, bb) ||
              sg_cuMemcpyHtoD(db, b->f64, bb) ||
              sg_cuMemAlloc(&dout, (size_t)count * 8) ||
              sg_gpu_run1d(sg_gpu_f_mm, count, args) ||
              sg_cuMemcpyDtoH(t->f64, dout, (size_t)count * 8);
    if (da) sg_cuMemFree(da);
    if (db) sg_cuMemFree(db);
    if (dout) sg_cuMemFree(dout);
    if (bad) {
        sg_gpu_fail();
        return NULL;
    }
    return t;
}

/* sum（kernel sg_sum(out,a,n)：tid|ctaid==0 单线程 k 升序 fadd 累加，
 * 与 CPU 逐位一致；grid 取 1；空张量和为 0.0 免 launch） */
static double sg_gpu_sum_try(const SgTensor *t, int *ok) {
    *ok = 0;
    int64_t n = sg_tsize(t);
    if (n == 0) {
        *ok = 1;
        return 0.0;
    }
    CUdeviceptr da = 0, dout = 0;
    double out = 0.0;
    void *args[] = {&dout, &da, &n};
    int bad = sg_cuMemAlloc(&da, (size_t)n * 8) ||
              sg_cuMemcpyHtoD(da, t->f64, (size_t)n * 8) ||
              sg_cuMemAlloc(&dout, 8) ||
              sg_gpu_run1d(sg_gpu_f_sum, 1, args) ||
              sg_cuMemcpyDtoH(&out, dout, 8);
    if (da) sg_cuMemFree(da);
    if (dout) sg_cuMemFree(dout);
    if (bad) {
        sg_gpu_fail();
        return 0.0;
    }
    *ok = 1;
    return out;
}

/* 2D 转置（kernel sg_tr(out,a,m,n)：m=输入行数，out[i*m+j]=a[j*n+i]，
 * grid=m*n；输出形状 n×m） */
static SgTensor *sg_gpu_tr_try(const SgTensor *t) {
    if (t->rank != 2) return NULL; /* 防御：覆盖集外 → CPU */
    int64_t m = t->shape[0], n = t->shape[1];
    const int64_t outShape[2] = {n, m};
    SgTensor *r = sg_tensor_alloc(0, 2, outShape);
    int64_t count = m * n;
    if (count == 0) return r;
    CUdeviceptr da = 0, dout = 0;
    void *args[] = {&dout, &da, &m, &n};
    int bad = sg_cuMemAlloc(&da, (size_t)count * 8) ||
              sg_cuMemcpyHtoD(da, t->f64, (size_t)count * 8) ||
              sg_cuMemAlloc(&dout, (size_t)count * 8) ||
              sg_gpu_run1d(sg_gpu_f_tr, count, args) ||
              sg_cuMemcpyDtoH(r->f64, dout, (size_t)count * 8);
    if (da) sg_cuMemFree(da);
    if (dout) sg_cuMemFree(dout);
    if (bad) {
        sg_gpu_fail();
        return NULL;
    }
    return r;
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
#endif

/* ---- 公开分派（ABI 与对应 sg_* 逐一相同；覆盖集 op 由类型检查器保证 ----
 * f64 且形状合法、无 panic 路径，fn/file/line 形参仅为 ABI 对齐；
 * GPU 就绪则尝试 GPU，任何失败当次即回退 CPU，输出逐位一致） ---- */

SgTensor *sg_gpu_tensor_binop_tt(int op, SgTensor *a, SgTensor *b,
                                 const char *fn, const char *file,
                                 int64_t line) {
    (void)fn;
    (void)file;
    (void)line;
    if (sg_gpu_ready()) {
        SgTensor *r = sg_gpu_ew_tt_try(op, a, b);
        if (r) return r;
    }
    return sg_tensor_binop_tt(op, a, b, fn, file, line);
}

SgTensor *sg_gpu_tensor_binop_ts(int op, SgTensor *a, int si64, int64_t vi,
                                 double vf, const char *fn, const char *file,
                                 int64_t line) {
    (void)si64;
    (void)vi; /* f64 覆盖集：标量恒取 vf 浮点值 */
    if (sg_gpu_ready()) {
        SgTensor *r = sg_gpu_ew_ts_try(op, a, vf, 0);
        if (r) return r;
    }
    return sg_tensor_binop_ts(op, a, si64, vi, vf, fn, file, line);
}

SgTensor *sg_gpu_tensor_binop_st(int op, int si64, int64_t vi, double vf,
                                 SgTensor *b, const char *fn, const char *file,
                                 int64_t line) {
    (void)si64;
    (void)vi; /* 标量在左：张量侧为 b，sleft=1 → s op b[i] */
    if (sg_gpu_ready()) {
        SgTensor *r = sg_gpu_ew_ts_try(op, b, vf, 1);
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
/// kRuntimeC / kGpuRuntimeC / kGpuPtxC 内嵌于编译器二进制，文本哈希入键——
/// 开发期改动运行时源自动失效，无需手工递增版本号。
uint64_t rtObjKey(const std::string& tag, bool gpuMode) {
    uint64_t h = fnvKey(kObjCacheVer);
    h = fnvKey(tag, h);
    h = fnvKey(gpuMode ? "gpu" : "cpu", h);
    h = fnvKey(kRuntimeC, h);
    if (gpuMode) {
        h = fnvKey(kGpuRuntimeC, h);
        h = fnvKey(kGpuPtxC, h);
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
            /* GPU 分派层：内嵌 kernel PTX（Windows 分支内定义，供 JIT 加载）
             * + 六个 sg_gpu_* 分派函数（覆盖集 op，失败回退 CPU） */
            f << "\n#ifdef _WIN32\n/* 内嵌 kernel PTX（llc sm_120 产物，"
                 "cuModuleLoadData JIT） */\nstatic const char sg_gpu_ptx[] =\n\""
              << cStringLiteral(kGpuPtxC) << "\";\n#endif\n"
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
