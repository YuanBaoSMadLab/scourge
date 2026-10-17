// ============================================================================
//  Scourge 编译器 — AOT C 运行时 · 张量段（Builder 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：kRuntimeC 第 2 段——SgTensor 类型、设备同步钩子、逐元素/矩阵/
//  归约/转置/slice/get/set、格式化打印（SG-BC VM TensorData 语义移植，panic
//  消息逐字对齐）。依赖第 1 段（kRuntimeC）的 sg_panic/sg_xmalloc；GPU 设备
//  驻留由 kGpuRuntimeC 分派层在初始化成功后安装同步钩子，其余构建恒空操作。
// ============================================================================
#pragma once

namespace scourge {

inline const char kTensorRuntimeC[] = R"RT(
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

/* data_ptr（0.9.6 O 节 FFI 宽类型域）：张量连续原生缓冲首地址。
 * 空张量 panic 消息与 VM doBuiltin 逐字一致（双模差分锁定点）；
 * str 路径不走此函数——AOT str 是全局常量，MLIRGen 就地 ptrtoint。 */
int64_t sg_data_ptr(SgTensor* t, const char* fn, const char* file,
                    int64_t line) {
    if (sg_tsize(t) == 0) {
        char msg[64];
        snprintf(msg, sizeof msg, "data_ptr() 不支持空张量");
        sg_panic(fn, file, line, msg);
    }
    sg_tsync(t); /* 设备驻留先回拷（回拷后地址才是宿主侧有效缓冲） */
    return (int64_t)(intptr_t)(t->isInt ? (const void*)t->i64
                                        : (const void*)t->f64);
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

)RT";

} // namespace scourge
