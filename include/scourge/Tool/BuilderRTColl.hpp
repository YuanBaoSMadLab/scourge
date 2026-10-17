// ============================================================================
//  Scourge 编译器 — AOT C 运行时 · 容器段（Builder 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：kRuntimeC 第 3 段——Result（错误即值，载荷按位编码）/
//  List（etag 0-3 键槽）/ Dict/Set（开放寻址哈希 + 64 位键编码），方法语义
//  与 SG-BC VM callValueMethod 逐字一致。依赖 kRuntimeC 的 sg_panic/sg_dup；
//  成员方法与 sorted 分别在 kStrRuntimeC / kIoRuntimeC。
// ============================================================================
#pragma once

namespace scourge {

inline const char kCollRuntimeC[] = R"RT(
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
    sg_str_mark(s); /* str 捕获 → 别名登记（见 mark 契约） */
    r->bits = (int64_t)(intptr_t)s;
    return r;
}
SgResult* sg_result_ok_n(void) { return sg_result_new(1); }
SgResult* sg_result_err(const char* msg) {
    SgResult* r = sg_result_new(0);
    sg_str_mark(msg); /* str 捕获 → 别名登记 */
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
    for (int64_t k = 0; k < n; ++k) {
        sg_str_mark(es[k]); /* str 捕获 → 别名登记（见 mark 契约） */
        ((char**)l->data)[k] = (char*)es[k];
    }
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

/* ---- 下标写（批 7 前置）：与 VM StoreIndex list 分支对齐——单下标、
 * 负下标归一、越界 panic 消息逐字一致；原地覆盖（共享语义所有别名可见）。
 * etag 防御与 append 同款（检查器保证不可达，仅兜编译器漏洞）。
 * str 版在 kStrRuntimeC（sg_dup 依赖）。 ---- */
void sg_list_store_i(SgList* l, int64_t idx, int64_t v, const char* fn,
                     const char* file, int64_t line) {
    if (l->len > 0 && l->etag != 0)
        sg_panic(fn, file, line, "下标赋值类型与列表元素不符");
    ((int64_t*)l->data)[sg_list_norm(l, idx, fn, file, line)] = v;
}
void sg_list_store_f(SgList* l, int64_t idx, double v, const char* fn,
                     const char* file, int64_t line) {
    if (l->len > 0 && l->etag != 1)
        sg_panic(fn, file, line, "下标赋值类型与列表元素不符");
    ((double*)l->data)[sg_list_norm(l, idx, fn, file, line)] = v;
}
void sg_list_store_b(SgList* l, int64_t idx, int v, const char* fn,
                     const char* file, int64_t line) {
    if (l->len > 0 && l->etag != 3)
        sg_panic(fn, file, line, "下标赋值类型与列表元素不符");
    ((int64_t*)l->data)[sg_list_norm(l, idx, fn, file, line)] = v ? 1 : 0;
}

/* ---- append 标量域（0.9.8）：原地尾插（共享语义所有别名可见，与 VM
 * shared_ptr push_back 对齐）。SgList 无容量字段，每次 realloc(len+1)——
 * 推导式/循环累积 O(n) 摊销可接受。空列表首插定型 etag；非空 etag 不符
 * 为防御 panic（检查器保证不可达）。 ---- */
void sg_list_append_i(SgList* l, int64_t v, const char* fn, const char* file,
                      int64_t line) {
    if (l->len > 0 && l->etag != 0)
        sg_panic(fn, file, line, "append() 实参类型与列表元素不符");
    l->data = realloc(l->data, sizeof(int64_t) * (size_t)(l->len + 1));
    if (!l->data) exit(1);
    ((int64_t*)l->data)[l->len] = v;
    ++l->len;
}

void sg_list_append_f(SgList* l, double v, const char* fn, const char* file,
                      int64_t line) {
    if (l->len > 0 && l->etag != 1)
        sg_panic(fn, file, line, "append() 实参类型与列表元素不符");
    if (l->len == 0) l->etag = 1;
    l->data = realloc(l->data, sizeof(double) * (size_t)(l->len + 1));
    if (!l->data) exit(1);
    ((double*)l->data)[l->len] = v;
    ++l->len;
}

void sg_list_append_b(SgList* l, int v, const char* fn, const char* file,
                      int64_t line) {
    if (l->len > 0 && l->etag != 3)
        sg_panic(fn, file, line, "append() 实参类型与列表元素不符");
    if (l->len == 0) l->etag = 3;
    l->data = realloc(l->data, sizeof(int64_t) * (size_t)(l->len + 1));
    if (!l->data) exit(1);
    ((int64_t*)l->data)[l->len] = v ? 1 : 0;
    ++l->len;
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

/* ---- 切片 s[a:b]（0.9.9）：归一公式与 VM callValueMethod Slice2 逐字
 * 一致（单一事实源，注释互指）：缺省端（has=0）补 0/len；负下标加 len
 * 后钳 0；越界钳 len；b<a 收敛为空切片。置于 kCollRuntimeC（kStrRuntimeC
 * 在其后，sg_str_nslice 复用本函数）。纯函数无 panic 路径。 ---- */
static void sg_normslice(int64_t* a, int64_t* b, int64_t hasA, int64_t hasB,
                         int64_t len) {
    if (!hasA)
        *a = 0;
    else if (*a < 0) {
        *a += len;
        if (*a < 0) *a = 0;
    }
    if (!hasB)
        *b = len;
    else if (*b < 0) {
        *b += len;
        if (*b < 0) *b = 0;
    }
    if (*a > len) *a = len;
    if (*b > len) *b = len;
    if (*b < *a) *b = *a;
}

/* ---- 切片 l[a:b]（0.9.9）：浅拷贝子列表（复用 sg_normslice）。按原 etag
 * 拷槽（句柄值原样透传 = 共享语义），纯函数无 panic 路径。 ---- */
SgList* sg_list_nslice(SgList* l, int64_t a, int64_t b, int64_t hasA,
                       int64_t hasB) {
    sg_normslice(&a, &b, hasA, hasB, l->len);
    int64_t n = b - a;
    SgList* out = sg_list_empty();
    out->len = n;
    out->etag = l->etag;
    size_t width = (l->etag == 1) ? sizeof(double)
                 : (l->etag == 2) ? sizeof(char*)
                                  : sizeof(int64_t);
    out->data = malloc(width * (size_t)(n > 0 ? n : 1));
    if (!out->data) exit(1);
    memcpy(out->data, (char*)l->data + a * (int64_t)width,
           (size_t)n * width);
    return out;
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
    /* str 槽位（tag 2）捕获 → 别名登记（见 mark 契约）；dict_new 逐元素
     * 复用本函数，字面量与 d[k]=v 两路同一埋点。 */
    if (d->ktag == 2) sg_str_mark((const char*)(uintptr_t)k);
    if (d->vtag == 2) sg_str_mark((const char*)(uintptr_t)v);
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

/* ---- 集合（0.9.3）：仅键的插入序哈希表。完整复用 SgDict 链式哈希实现
 * （值槽恒 0、vtag=4 哨兵）——单一哈希实现零重复，插入序/去重/扩容语义
 * 与 VM SetData 完全一致；NaN 元素在此显式拒绝（消息与 VM 逐字一致，
 * 内层 sg_dict_check_key 不会先触发，因为 NaN 不会到达它）。 ---- */
SgDict* sg_set_new(int32_t ktag, int64_t n, const int64_t* keys,
                   const char* fn, const char* file, int64_t line) {
    for (int64_t i = 0; i < n; ++i) {
        if (ktag == 1) {
            double dv;
            memcpy(&dv, &keys[i], sizeof dv);
            if (dv != dv) sg_panic(fn, file, line, "集合元素不能为 NaN");
        }
    }
    SgDict* d = (SgDict*)sg_xmalloc(sizeof(SgDict));
    d->len = 0;
    d->bcap = 8;
    d->ktag = ktag;
    d->vtag = 4; /* 哨兵：仅键表（值槽恒 0，无观察面） */
    d->ec_cap = n > 4 ? n : 4;
    d->ents = (SgDictEnt*)sg_xmalloc(sizeof(SgDictEnt) * (size_t)d->ec_cap);
    d->buckets = (int32_t*)sg_xmalloc(sizeof(int32_t) * (size_t)d->bcap);
    for (int64_t i = 0; i < d->bcap; ++i) d->buckets[i] = 0;
    for (int64_t i = 0; i < n; ++i)
        sg_dict_set(d, (uint64_t)keys[i], 0, fn, file, line);
    return d;
}

int64_t sg_set_len(SgDict* d) { return d->len; }

int sg_set_in(SgDict* d, uint64_t k) { return sg_dict_find(d, k) >= 0; }

uint64_t sg_set_elem_at(SgDict* d, int64_t idx, const char* fn,
                        const char* file, int64_t line) {
    if (idx < 0 || idx >= d->len)
        sg_panic(fn, file, line, "集合迭代下标越界");
    return d->ents[idx].k;
}

/* ---- List 归约（0.9.9）：op 0=sum 1=min 2=max；AOT 列表恒同质（i64/
 * f64/bool 三 etag），bool 槽值 0/1 按 i64 参与运算。空序列：sum=0（与
 * 张量空和一致），min/max panic（消息与 VM 逐字一致，name 供消息）。 ---- */
int64_t sg_list_reduce_i(int32_t op, SgList* l, const char* name,
                         const char* fn, const char* file, int64_t line) {
    if (l->len == 0 && op != 0) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s() 不支持空序列", name);
        sg_panic(fn, file, line, buf);
    }
    int64_t acc = op == 0 ? 0 : ((int64_t*)l->data)[0];
    for (int64_t k = op == 0 ? 0 : 1; k < l->len; ++k) {
        int64_t x = ((int64_t*)l->data)[k];
        if (op == 0) {
            int64_t out;
            if (__builtin_add_overflow(acc, x, &out))
                sg_panic(fn, file, line, "整数求和溢出");
            acc = out;
        } else if (op == 1) {
            if (x < acc) acc = x;
        } else {
            if (x > acc) acc = x;
        }
    }
    return acc;
}

double sg_list_reduce_f(int32_t op, SgList* l, const char* name,
                        const char* fn, const char* file, int64_t line) {
    if (l->len == 0 && op != 0) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s() 不支持空序列", name);
        sg_panic(fn, file, line, buf);
    }
    double acc = op == 0 ? 0.0 : ((double*)l->data)[0];
    for (int64_t k = op == 0 ? 0 : 1; k < l->len; ++k) {
        double x = ((double*)l->data)[k];
        if (op == 0)
            acc += x;
        else if (op == 1) {
            if (x < acc) acc = x;
        } else {
            if (x > acc) acc = x;
        }
    }
    return acc;
}

/* ---- Dict 视图（0.9.9）：keys/values/items 只读快照（新列表，插入序，
 * 与 VM entries 遍历一致）。槽编码与 d[k] 读取位直通同构：float 位模式、
 * str 指针（补 mark 别名登记）、bool 0/1。items 槽存 2 元组句柄指针位
 * （SgList*，与 AOT genTupleLit 元组同构——下标/for-in 侧 inttoptr 还原）。 ---- */
static SgList* sg_dict_collect(const SgDict* d, int which) {
    if (which == 2) {
        SgList* out = sg_list_empty();
        out->len = d->len;
        out->data = malloc(sizeof(int64_t) * (size_t)(d->len > 0 ? d->len : 1));
        if (!out->data) exit(1);
        for (int64_t k = 0; k < d->len; ++k) {
            uint64_t kv[2] = {d->ents[k].k, d->ents[k].v};
            if (d->ktag == 2) sg_str_mark((const char*)(uintptr_t)kv[0]);
            if (d->vtag == 2) sg_str_mark((const char*)(uintptr_t)kv[1]);
            SgList* t = sg_list_new_i(2, (const int64_t*)kv);
            ((int64_t*)out->data)[k] = (int64_t)(intptr_t)t;
        }
        return out;
    }
    int32_t tag = which == 0 ? d->ktag : d->vtag;
    SgList* out = sg_list_empty();
    out->len = d->len;
    if (tag == 1) {
        out->etag = 1;
        out->data =
            malloc(sizeof(double) * (size_t)(d->len > 0 ? d->len : 1));
        if (!out->data) exit(1);
        for (int64_t k = 0; k < d->len; ++k) {
            uint64_t bits = which == 0 ? d->ents[k].k : d->ents[k].v;
            memcpy(&((double*)out->data)[k], &bits, 8);
        }
    } else {
        // etag 对齐槽语义：str=2、bool=3（sg_list_format 按 etag repr；
        // i64 槽复用为 str 指针/bool 0-1 位，与 d[k] 读取位直通同构）。
        out->etag = tag == 2 ? 2 : (tag == 3 ? 3 : 0);
        out->data =
            malloc(sizeof(int64_t) * (size_t)(d->len > 0 ? d->len : 1));
        if (!out->data) exit(1);
        for (int64_t k = 0; k < d->len; ++k) {
            uint64_t bits = which == 0 ? d->ents[k].k : d->ents[k].v;
            if (tag == 2) sg_str_mark((const char*)(uintptr_t)bits);
            ((int64_t*)out->data)[k] = (int64_t)bits;
        }
    }
    return out;
}

/* 检查器保证 self 为 Dict，防御分支不可达；签名带上下文仅为 ABI 统一。 */
SgList* sg_dict_keys(SgDict* d, const char* fn, const char* file,
                     int64_t line) {
    (void)fn;
    (void)file;
    (void)line;
    return sg_dict_collect(d, 0);
}

SgList* sg_dict_values(SgDict* d, const char* fn, const char* file,
                       int64_t line) {
    (void)fn;
    (void)file;
    (void)line;
    return sg_dict_collect(d, 1);
}

SgList* sg_dict_items(SgDict* d, const char* fn, const char* file,
                      int64_t line) {
    (void)fn;
    (void)file;
    (void)line;
    return sg_dict_collect(d, 2);
}

/* get 带缺省：缺键不 panic（VM DictGet 同哲学）；键相等按槽位语义精确
 * 比较（检查器统一键域，与 sg_dict_find 同一事实源）。 */
uint64_t sg_dict_get_or(SgDict* d, uint64_t k, uint64_t def) {
    int64_t e = sg_dict_find(d, k);
    return e >= 0 ? d->ents[e].v : def;
}

/* Set.add：NaN 先查（与 VM SetAdd 逐字一致），去重/插入序复用 sg_dict_set
 * （Set = vtag 4 仅键表，值槽恒 0；str 元素由其 ktag==2 分支登记）。 */
void sg_set_add(SgDict* d, uint64_t k, const char* fn, const char* file,
                int64_t line) {
    if (d->ktag == 1) {
        double dv;
        memcpy(&dv, &k, sizeof dv);
        if (dv != dv) sg_panic(fn, file, line, "集合元素不能为 NaN");
    }
    sg_dict_set(d, k, 0, fn, file, line);
}

)RT";

} // namespace scourge
