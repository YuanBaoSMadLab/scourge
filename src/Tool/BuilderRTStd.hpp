// ============================================================================
//  Scourge 编译器 — AOT C 运行时 · 标准库广度片段（Builder 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：kRuntimeC / kStrRuntimeC 之后的追加片段，build 时拼接写入
//  <out>_rt.c——依赖其 SgTensor / SgList / sg_panic / sg_tsync / sg_tsize
//  定义，缓存键 rtObjKey 已计入本片段文本。仅供 Builder.cpp 引用，不置于
//  include/。
//  语义契约：与 VM stdBuiltin（VMStd.cpp）panic 消息与数值结果逐字一致，
//  双模差分由 verify 探针锁定：
//    * mean/var/std 接受张量与数值列表（etag 0/1），恒返回 f64；
//      空输入 panic；方差为总体方差（÷N）；NaN 按 IEEE 传播不拦截；
//      求和顺序固定为左→右单遍（双模 IEEE 逐位一致的依据）。
//    * median 同输入域恒返回 f64；排序取中（奇取中位、偶取中间两值平均），
//      NaN 破坏全序显式 panic（同 sorted 契约：拒绝而非未定义）。
//    * reversed：列表元素倒序新列表（str 元素复制指针，程序生命周期持有）；
//      str 按字节倒序（非 Unicode 码点感知，文档化）。
//    * sha256(str) → 64 位小写 hex（公有领域参考实现，FIPS 180-4）；
//      crc32(str) → CRC-32/ISO-HDLC（IEEE 802.3，zlib 同款参数：
//      poly 0xEDB88320 反射、初值 0xFFFFFFFF、终异或 0xFFFFFFFF）。
// ============================================================================
#pragma once

namespace scourge {

inline const char kStdRuntimeC[] = R"RT(
/* ==== 标准库广度：统计 / 倒序 / 哈希（0.9.4 M 专项；与 VM stdBuiltin 一致） ==== */

/* ---- 数值序列内部统一：把张量/列表元素按左→右序喂给累加器 ---- */

static double sg_tensor_avg(SgTensor* t, int mode, const char* fn,
                            const char* file, int64_t line) {
    /* mode: 0=mean 1=var 2=std。两遍算法（mean→偏差平方和），遍历顺序固定。 */
    static const char* const kNm[3] = {"mean", "var", "std"};
    const char* nm = kNm[mode];
    char pmsg[64];
    sg_tsync(t); /* 设备驻留先回拷（GPU try 失败回退 CPU 时） */
    int64_t n = sg_tsize(t);
    if (n == 0) {
        sprintf(pmsg, "%s() 不支持空序列", nm);
        sg_panic(fn, file, line, pmsg);
    }
    double sum = 0.0;
    if (t->isInt) {
        for (int64_t k = 0; k < n; ++k) sum += (double)t->i64[k];
    } else {
        for (int64_t k = 0; k < n; ++k) sum += t->f64[k];
    }
    double m = sum / (double)n;
    if (mode == 0) return m;
    double acc = 0.0;
    if (t->isInt) {
        for (int64_t k = 0; k < n; ++k) {
            double d = (double)t->i64[k] - m;
            acc += d * d;
        }
    } else {
        for (int64_t k = 0; k < n; ++k) {
            double d = t->f64[k] - m;
            acc += d * d;
        }
    }
    double v = acc / (double)n; /* 总体方差（÷N） */
    if (mode == 1) return v;
    return sqrt(v);
}

static double sg_list_avg(SgList* l, int mode, const char* fn,
                          const char* file, int64_t line) {
    static const char* const kNm[3] = {"mean", "var", "std"};
    const char* nm = kNm[mode];
    char pmsg[64];
    if (l->len == 0) {
        sprintf(pmsg, "%s() 不支持空序列", nm);
        sg_panic(fn, file, line, pmsg);
    }
    if (l->etag != 0 && l->etag != 1) {
        sprintf(pmsg, "%s() 仅支持数值列表（int/float 元素）", nm);
        sg_panic(fn, file, line, pmsg);
    }
    const int64_t* iv = (const int64_t*)l->data;
    const double* fv = (const double*)l->data;
    double sum = 0.0;
    for (int64_t k = 0; k < l->len; ++k)
        sum += (l->etag == 1) ? fv[k] : (double)iv[k];
    double m = sum / (double)l->len;
    if (mode == 0) return m;
    double acc = 0.0;
    for (int64_t k = 0; k < l->len; ++k) {
        double x = (l->etag == 1) ? fv[k] : (double)iv[k];
        double d = x - m;
        acc += d * d;
    }
    double v = acc / (double)l->len; /* 总体方差（÷N） */
    if (mode == 1) return v;
    return sqrt(v);
}

double sg_tensor_mean(SgTensor* t, const char* fn, const char* file,
                      int64_t line) {
    return sg_tensor_avg(t, 0, fn, file, line);
}
double sg_tensor_var(SgTensor* t, const char* fn, const char* file,
                     int64_t line) {
    return sg_tensor_avg(t, 1, fn, file, line);
}
double sg_tensor_std(SgTensor* t, const char* fn, const char* file,
                     int64_t line) {
    return sg_tensor_avg(t, 2, fn, file, line);
}
double sg_list_mean(SgList* l, const char* fn, const char* file,
                    int64_t line) {
    return sg_list_avg(l, 0, fn, file, line);
}
double sg_list_var(SgList* l, const char* fn, const char* file,
                   int64_t line) {
    return sg_list_avg(l, 1, fn, file, line);
}
double sg_list_std(SgList* l, const char* fn, const char* file,
                   int64_t line) {
    return sg_list_avg(l, 2, fn, file, line);
}

/* ---- median：排序取中（NaN 破坏全序，显式 panic 拒绝——同 sorted 契约）。
   双模一致依据：无 NaN 时排序后的值序列跨实现确定（值相等元素次序不影响
   中位数取值），奇偶取法与平均运算与 VM seqMedian（VMStd.cpp）IEEE 一致。 */
static int sg_dbl_cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

static double sg_median_of(double* xs, int64_t n) {
    qsort(xs, (size_t)n, sizeof(double), sg_dbl_cmp);
    if (n % 2 == 1) return xs[n / 2];
    return (xs[n / 2 - 1] + xs[n / 2]) / 2.0;
}

double sg_tensor_median(SgTensor* t, const char* fn, const char* file,
                        int64_t line) {
    sg_tsync(t); /* 设备驻留先回拷（GPU try 失败回退 CPU 时） */
    int64_t n = sg_tsize(t);
    if (n == 0) sg_panic(fn, file, line, "median() 不支持空序列");
    double* xs = (double*)malloc(sizeof(double) * (size_t)n);
    if (!xs) exit(1);
    for (int64_t k = 0; k < n; ++k)
        xs[k] = t->isInt ? (double)t->i64[k] : t->f64[k];
    for (int64_t k = 0; k < n; ++k) {
        if (xs[k] != xs[k]) {
            free(xs);
            sg_panic(fn, file, line, "median() 不支持含 NaN 的元素");
        }
    }
    double r = sg_median_of(xs, n);
    free(xs);
    return r;
}

double sg_list_median(SgList* l, const char* fn, const char* file,
                      int64_t line) {
    if (l->len == 0) sg_panic(fn, file, line, "median() 不支持空序列");
    if (l->etag != 0 && l->etag != 1)
        sg_panic(fn, file, line, "median() 仅支持数值列表（int/float 元素）");
    const int64_t* iv = (const int64_t*)l->data;
    const double* fv = (const double*)l->data;
    double* xs = (double*)malloc(sizeof(double) * (size_t)l->len);
    if (!xs) exit(1);
    for (int64_t k = 0; k < l->len; ++k)
        xs[k] = (l->etag == 1) ? fv[k] : (double)iv[k];
    for (int64_t k = 0; k < l->len; ++k) {
        if (xs[k] != xs[k]) {
            free(xs);
            sg_panic(fn, file, line, "median() 不支持含 NaN 的元素");
        }
    }
    double r = sg_median_of(xs, l->len);
    free(xs);
    return r;
}

/* ---- reversed：列表倒序新列表（元素槽宽 8B：etag 0/1/3 值、2 为指针） ---- */
SgList* sg_list_reversed(SgList* l) {
    SgList* out = sg_list_empty();
    out->len = l->len;
    out->etag = l->etag;
    int64_t n = l->len;
    if (n <= 0) return out;
    const int64_t* src = (const int64_t*)l->data;
    int64_t* dst = (int64_t*)malloc(sizeof(int64_t) * (size_t)n);
    if (!dst) exit(1);
    for (int64_t k = 0; k < n; ++k) dst[k] = src[n - 1 - k];
    out->data = dst;
    return out;
}

const char* sg_str_reversed(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (!p) exit(1);
    for (size_t k = 0; k < n; ++k) p[k] = s[n - 1 - k];
    p[n] = 0;
    return p;
}

/* ---- SHA-256（FIPS 180-4；算法公有领域，参考实现对拍 NIST 测试向量） ---- */
typedef struct {
    unsigned int st[8];
    unsigned long long bits;
    unsigned char buf[64];
    size_t buflen;
} SgSha256;

static unsigned int sg_sha_rotr(unsigned int x, int r) {
    return (x >> r) | (x << (32 - r));
}

static void sg_sha_block(SgSha256* c, const unsigned char* p) {
    static const unsigned int K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
        0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
        0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
        0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    unsigned int w[64];
    for (int k = 0; k < 16; ++k)
        w[k] = ((unsigned int)p[k * 4] << 24) | ((unsigned int)p[k * 4 + 1] << 16) |
               ((unsigned int)p[k * 4 + 2] << 8) | (unsigned int)p[k * 4 + 3];
    for (int k = 16; k < 64; ++k) {
        unsigned int s0 = sg_sha_rotr(w[k - 15], 7) ^ sg_sha_rotr(w[k - 15], 18) ^
                          (w[k - 15] >> 3);
        unsigned int s1 = sg_sha_rotr(w[k - 2], 17) ^ sg_sha_rotr(w[k - 2], 19) ^
                          (w[k - 2] >> 10);
        w[k] = w[k - 16] + s0 + w[k - 7] + s1;
    }
    unsigned int a = c->st[0], b = c->st[1], cc = c->st[2], d = c->st[3];
    unsigned int e = c->st[4], f = c->st[5], g = c->st[6], h = c->st[7];
    for (int k = 0; k < 64; ++k) {
        unsigned int S1 = sg_sha_rotr(e, 6) ^ sg_sha_rotr(e, 11) ^
                          sg_sha_rotr(e, 25);
        unsigned int ch = (e & f) ^ (~e & g);
        unsigned int t1 = h + S1 + ch + K[k] + w[k];
        unsigned int S0 = sg_sha_rotr(a, 2) ^ sg_sha_rotr(a, 13) ^
                          sg_sha_rotr(a, 22);
        unsigned int mj = (a & b) ^ (a & cc) ^ (b & cc);
        unsigned int t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->st[0] += a; c->st[1] += b; c->st[2] += cc; c->st[3] += d;
    c->st[4] += e; c->st[5] += f; c->st[6] += g; c->st[7] += h;
}

static void sg_sha_init(SgSha256* c) {
    c->st[0] = 0x6a09e667; c->st[1] = 0xbb67ae85;
    c->st[2] = 0x3c6ef372; c->st[3] = 0xa54ff53a;
    c->st[4] = 0x510e527f; c->st[5] = 0x9b05688c;
    c->st[6] = 0x1f83d9ab; c->st[7] = 0x5be0cd19;
    c->bits = 0;
    c->buflen = 0;
}

static void sg_sha_update(SgSha256* c, const unsigned char* p, size_t n) {
    c->bits += (unsigned long long)n * 8;
    while (n > 0) {
        size_t take = 64 - c->buflen;
        if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        n -= take;
        if (c->buflen == 64) {
            sg_sha_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

static void sg_sha_final(SgSha256* c, unsigned char out[32]) {
    unsigned long long bitlen = c->bits;
    unsigned char pad = 0x80;
    sg_sha_update(c, &pad, 1);
    unsigned char z = 0;
    while (c->buflen != 56) sg_sha_update(c, &z, 1);
    unsigned char lenb[8];
    for (int k = 0; k < 8; ++k)
        lenb[k] = (unsigned char)(bitlen >> (56 - k * 8));
    /* 长度注入不改变 bits 计数语义（final 只补块） */
    memcpy(c->buf + 56, lenb, 8);
    sg_sha_block(c, c->buf);
    c->buflen = 0;
    for (int k = 0; k < 8; ++k) {
        out[k * 4] = (unsigned char)(c->st[k] >> 24);
        out[k * 4 + 1] = (unsigned char)(c->st[k] >> 16);
        out[k * 4 + 2] = (unsigned char)(c->st[k] >> 8);
        out[k * 4 + 3] = (unsigned char)(c->st[k]);
    }
}

const char* sg_sha256(const char* s) {
    SgSha256 c;
    sg_sha_init(&c);
    sg_sha_update(&c, (const unsigned char*)s, strlen(s));
    unsigned char dig[32];
    sg_sha_final(&c, dig);
    static const char* hexd = "0123456789abcdef";
    char* out = (char*)malloc(65);
    if (!out) exit(1);
    for (int k = 0; k < 32; ++k) {
        out[k * 2] = hexd[dig[k] >> 4];
        out[k * 2 + 1] = hexd[dig[k] & 15];
    }
    out[64] = 0;
    return out;
}

/* ---- CRC-32/ISO-HDLC（IEEE 802.3；zlib/gzip 同款参数） ---- */
static unsigned int sg_crc32_table[256];
static int sg_crc32_ready = 0;

static void sg_crc32_init(void) {
    for (unsigned int k = 0; k < 256; ++k) {
        unsigned int c = k;
        for (int b = 0; b < 8; ++b)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        sg_crc32_table[k] = c;
    }
    sg_crc32_ready = 1;
}

int64_t sg_crc32(const char* s) {
    if (!sg_crc32_ready) sg_crc32_init();
    unsigned int c = 0xFFFFFFFFu;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
        c = sg_crc32_table[(c ^ *p) & 0xFF] ^ (c >> 8);
    return (int64_t)(c ^ 0xFFFFFFFFu); /* 恒非负，int64 可精确承载 */
}

/* ==== dict repr 格式化（0.9.7；与 VM toString(dict, repr=true) 逐字一致） ====
 * "{" + "k: v"（", " 连接，插入序）+ "}"；str 键/值带引号转义（sg_list_escape
 * 规则，置于本片段之前可见）。tag：0=int 1=f64(位型) 2=str 3=bool；
 * tag 4 为容器句柄槽（64 位无类型标签）运行时无法 repr——调用方（检查器/
 * MLIRGen）已在编译期拒绝，此处 panic 为防御分支（同 data_ptr 空张量）。 */
static size_t sg_slot_len(int32_t tag, uint64_t slot) {
    switch (tag) {
    case 1: return 64;
    case 2: return 2 * strlen((const char*)(size_t)slot) + 10;
    case 3: return 5;
    default: return 20;
    }
}

static void sg_slot_write(int32_t tag, uint64_t slot, char** w) {
    char buf[64];
    switch (tag) {
    case 1: {
        double dv;
        memcpy(&dv, &slot, sizeof dv);
        snprintf(buf, sizeof buf, "%s", sg_fmt_f64(dv));
        memcpy(*w, buf, strlen(buf));
        *w += strlen(buf);
        return;
    }
    case 2:
        sg_list_escape((const char*)(size_t)slot, w);
        return;
    case 3:
        memcpy(*w, slot ? "True" : "False", slot ? 4 : 5);
        *w += slot ? 4 : 5;
        return;
    default:
        snprintf(buf, sizeof buf, "%lld", (long long)slot);
        memcpy(*w, buf, strlen(buf));
        *w += strlen(buf);
    }
}

char* sg_dict_format(SgDict* d) {
    /* 两遍：先算总长，再填（槽位文本长度上界同 sg_list_format）。 */
    int64_t n = d->len;
    size_t total = 2;
    for (int64_t k = 0; k < n; ++k) {
        if (k) total += 2; /* ", " */
        total += sg_slot_len(d->ktag, d->ents[k].k);
        total += 2; /* ": " */
        total += sg_slot_len(d->vtag, d->ents[k].v);
    }
    char* out = (char*)malloc(total + 1);
    if (!out) exit(1);
    char* w = out;
    *w++ = '{';
    for (int64_t k = 0; k < n; ++k) {
        if (k) { *w++ = ','; *w++ = ' '; }
        sg_slot_write(d->ktag, d->ents[k].k, &w);
        *w++ = ':'; *w++ = ' ';
        sg_slot_write(d->vtag, d->ents[k].v, &w);
    }
    *w++ = '}';
    *w = 0;
    return out;
}

void sg_print_dict(SgDict* d) {
    if (d->ktag == 4 || d->vtag == 4)
        sg_panic("sg_print_dict", "<rt>", 0, "print 容器值字典（防御分支，编译期已拒绝）");
    fputs(sg_dict_format(d), stdout);
}
)RT";

} // namespace scourge
