// ============================================================================
//  Scourge 编译器 — AOT C 运行时 · GPU 分派层 · 覆盖集 try 函数与 CPU 桩
//  （Builder 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：kGpuRuntimeC 第 2 段——逐元素/matmul/归约/转置/一元/slice/
//  get/set 的单次尝试函数（失败置 state=0 并返回 NULL/!ok → 调用方回退
//  CPU，动态形状放宽后 dtype/秩/形状一致与内维由本守卫运行时校验，panic
//  消息与 CPU 路径逐字一致；int 覆盖集溢出由 kernel 状态缓冲上报）+
//  SCOURGE_NO_GPU / 初始化失败的 CPU 桩分支（函数名同实装版）。
//  依赖 kGpuRuntimeC 第 1 段的分派层与设备表。
// ============================================================================
#pragma once

namespace scourge {

inline const char kGpuOpsRuntimeC[] = R"GP(
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

} // namespace scourge
