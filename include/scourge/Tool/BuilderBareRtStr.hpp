// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
// ============================================================================
//  Scourge bare runtime — str/container segment (kBareRtStrC + kBareRtStrTailC)
// Appended in BuilderBare.cpp after base segment; fmt() split out to
// BuilderBareRtFmt.hpp.

namespace scourge {
namespace bare {

inline const char kBareRtStrC[] = R"BRS(/* Scourge 裸机运行时·str/容器（--freestanding 生成；勿手工改） */

/* ---- 前向声明：str 方法段引用 List 段（后文函数定义）与 crt TU 的 libc shim ---- */
typedef struct SgList {
    int64_t len;
    int etag;
    void* data; /* etag 0/3: int64_t*  1: double*  2: char** */
} SgList;
SgList* sg_list_empty(void);
static int64_t sg_list_norm(SgList* l, int64_t idx, const char* fn,
                            const char* file, int64_t line);
static void sg_normslice(int64_t* a, int64_t* b, int64_t hasA, int64_t hasB,
                         int64_t len);
char* strstr(const char* hay, const char* needle);
char* strchr(const char* s, int c);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, unsigned long long n);
unsigned long long strlen(const char* s);
void* memcpy(void* d, const void* s, unsigned long long n);
void* memmove(void* d, const void* s, unsigned long long n);
void* memset(void* d, int c, unsigned long long n);
int memcmp(const void* a, const void* b, unsigned long long n);

static int sg_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

const char* sg_str_upper(const char* s) {
    unsigned long long n = 0;
    while (s[n]) ++n;
    char* p = (char*)sg_balloc(n + 1);
    for (unsigned long long k = 0; k < n; ++k) {
        char c = s[k];
        p[k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    p[n] = 0;
    return p;
}

const char* sg_str_lower(const char* s) {
    unsigned long long n = 0;
    while (s[n]) ++n;
    char* p = (char*)sg_balloc(n + 1);
    for (unsigned long long k = 0; k < n; ++k) {
        char c = s[k];
        p[k] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    p[n] = 0;
    return p;
}

static const char* sg_trim_impl(const char* s, int left, int right) {
    unsigned long long b = 0, e = 0;
    while (s[e]) ++e;
    if (left)
        while (b < e && sg_is_space((unsigned char)s[b])) ++b;
    if (right)
        while (e > b && sg_is_space((unsigned char)s[e - 1])) --e;
    return sg_bdupn(s + b, e - b);
}

const char* sg_str_trim(const char* s) { return sg_trim_impl(s, 1, 1); }
const char* sg_str_trim_start(const char* s) { return sg_trim_impl(s, 1, 0); }
const char* sg_str_trim_end(const char* s) { return sg_trim_impl(s, 0, 1); }

/* Python 语义 split：无匹配 → 单元素整串；分隔符邻接 → 空串元素。 */
SgList* sg_str_split(const char* s, const char* sep, const char* fn,
                     const char* file, int64_t line) {
    if (!*sep) sg_panic(fn, file, line, "split() 分隔符不能为空串");
    unsigned long long sl = 0;
    while (sep[sl]) ++sl;
    int64_t n = 1;
    for (const char* p = s; (p = strstr(p, sep)) != 0; p += sl) ++n;
    SgList* out = sg_list_empty();
    out->len = n;
    out->etag = 2;
    out->data = sg_balloc(sizeof(char*) * (unsigned long long)n);
    int64_t k = 0;
    const char* start = s;
    for (const char* p = s; (p = strstr(p, sep)) != 0; p += sl) {
        ((char**)out->data)[k++] = sg_bdupn(start, (unsigned long long)(p - start));
        start = p + sl;
    }
    ((char**)out->data)[k] = sg_bdup(start);
    return out;
}

/* List[str].append：原地尾插（共享语义所有别名可见）；空列表 etag 定型。 */
void sg_list_append(SgList* l, const char* s, const char* fn,
                    const char* file, int64_t line) {
    if (l->len > 0 && l->etag != 2)
        sg_panic(fn, file, line, "append() 仅支持 List[str]");
    if (l->len == 0 && l->etag == 0) l->etag = 2;
    void* nd = sg_balloc(sizeof(char*) * (unsigned long long)(l->len + 1));
    for (int64_t i = 0; i < l->len; ++i) ((char**)nd)[i] = ((char**)l->data)[i];
    ((char**)nd)[l->len] = sg_bdup(s);
    l->data = nd;
    ++l->len;
}

/* self 为分隔符、parts 为字符串列表（与 VM StrJoin 语义一致）。 */
const char* sg_str_join(const char* sep, SgList* parts, const char* fn,
                        const char* file, int64_t line) {
    if (parts->len > 0 && parts->etag != 2)
        sg_panic(fn, file, line, "join() 列表元素必须是 str");
    unsigned long long sepl = 0;
    while (sep[sepl]) ++sepl;
    int64_t n = parts->len;
    char** es = (char**)parts->data;
    unsigned long long total = 0;
    for (int64_t k = 0; k < n; ++k) {
        const char* e = es[k];
        while (*e) {
            ++total;
            ++e;
        }
    }
    if (n > 0) total += sepl * (unsigned long long)(n - 1);
    char* p = (char*)sg_balloc(total + 1);
    unsigned long long off = 0;
    for (int64_t k = 0; k < n; ++k) {
        if (k) {
            for (unsigned long long i = 0; i < sepl; ++i) p[off++] = sep[i];
        }
        const char* e = es[k];
        while (*e) p[off++] = *e++;
    }
    p[off] = 0;
    return p;
}

const char* sg_str_replace(const char* s, const char* oldS, const char* newS,
                           const char* fn, const char* file, int64_t line) {
    if (!*oldS) sg_panic(fn, file, line, "replace() 旧子串不能为空串");
    unsigned long long ol = 0, nl = 0, sl = 0;
    while (oldS[ol]) ++ol;
    while (newS[nl]) ++nl;
    while (s[sl]) ++sl;
    int64_t cnt = 0;
    for (const char* p = s; (p = strstr(p, oldS)) != 0; p += ol) ++cnt;
    char* out = (char*)sg_balloc(
        (unsigned long long)((int64_t)sl + cnt * ((int64_t)nl - (int64_t)ol)) + 1);
    char* w = out;
    const char* start = s;
    for (const char* p = s; (p = strstr(p, oldS)) != 0; p += ol) {
        while (start < p) *w++ = *start++;
        for (unsigned long long i = 0; i < nl; ++i) *w++ = newS[i];
        start = p + ol;
    }
    while (*start) *w++ = *start++;
    *w = 0;
    return out;
}

int64_t sg_str_find(const char* hay, const char* needle) {
    const char* p = strstr(hay, needle);
    return p ? (int64_t)(p - hay) : -1;
}

int64_t sg_str_index(const char* hay, const char* needle, const char* fn,
                     const char* file, int64_t line) {
    int64_t r = sg_str_find(hay, needle);
    if (r < 0) sg_panic(fn, file, line, "index() 未找到子串");
    return r;
}

int sg_str_startswith(const char* s, const char* pre) {
    while (*pre) {
        if (*s != *pre) return 0;
        ++s;
        ++pre;
    }
    return 1;
}

int sg_str_endswith(const char* s, const char* suf) {
    unsigned long long sl = 0, xl = 0;
    while (s[sl]) ++sl;
    while (suf[xl]) ++xl;
    if (xl > sl) return 0;
    for (unsigned long long i = 0; i < xl; ++i)
        if (s[sl - xl + i] != suf[i]) return 0;
    return 1;
}

/* List 成员匹配（etag 0/3 i64 精确、1 位型、2 字节序；tag/etag 不符视为
 * 不匹配——检查器已静态拦截，此处兜底确定性）。 */
static int sg_list_match(const SgList* l, int64_t k, int32_t tag,
                         int64_t key) {
    switch (l->etag) {
    case 0:
    case 3:
        if (tag != 0 && tag != 3) return 0;
        return ((const int64_t*)l->data)[k] == (tag == 3 ? (key ? 1 : 0) : key);
    case 1: {
        if (tag != 1) return 0;
        double v;
        __builtin_memcpy(&v, &key, 8);
        return ((const double*)l->data)[k] == v;
    }
    case 2:
        if (tag != 2) return 0;
        return strcmp((const char*)(unsigned long long)key,
                      ((const char* const*)l->data)[k]) == 0;
    }
    return 0;
}

int64_t sg_list_index(SgList* l, int32_t tag, int64_t key, const char* fn,
                      const char* file, int64_t line) {
    for (int64_t k = 0; k < l->len; ++k)
        if (sg_list_match(l, k, tag, key)) return k;
    sg_panic(fn, file, line, "index() 未找到元素");
    return -1; /* 不可达（panic 不返回），避免告警 */
}

int64_t sg_list_count(SgList* l, int32_t tag, int64_t key) {
    int64_t n = 0;
    for (int64_t k = 0; k < l->len; ++k)
        if (sg_list_match(l, k, tag, key)) ++n;
    return n;
}

/* ==== list repr 格式化（与 VM toString(list, repr=true) 逐字一致） ==== */
static void sg_list_escape(const char* s, char** w) {
    *(*w)++ = '"';
    for (const char* p = s; *p; ++p) {
        switch (*p) {
        case '\\': *(*w)++ = '\\'; *(*w)++ = '\\'; break;
        case '"': *(*w)++ = '\\'; *(*w)++ = '"'; break;
        case '\n': *(*w)++ = '\\'; *(*w)++ = 'n'; break;
        case '\t': *(*w)++ = '\\'; *(*w)++ = 't'; break;
        case '\r': *(*w)++ = '\\'; *(*w)++ = 'r'; break;
        default: *(*w)++ = *p;
        }
    }
    *(*w)++ = '"';
}

char* sg_str_escape(const char* s) {
    unsigned long long cap = 3;
    for (const char* p = s; *p; ++p) cap += 2;
    char* out = (char*)sg_balloc(cap);
    char* w = out;
    sg_list_escape(s, &w);
    *w = 0;
    return out;
}

char* sg_list_format(SgList* l) {
    /* 两遍：先算总长（int ≤ 20 / float ≤ 32 / bool ≤ 5 / str = 2×len+2），
     * 再填——str 元素转义可达 2 倍长，估宽会溢出，必须精确。 */
    int64_t n = l->len;
    unsigned long long total = 2;
    for (int64_t k = 0; k < n; ++k) {
        if (k) total += 2;
        switch (l->etag) {
        case 1: total += 32; break;
        case 2: total += 2 * strlen(((char* const*)l->data)[k]) + 2; break;
        case 3: total += 5; break;
        default: total += 20;
        }
    }
    char* out = (char*)sg_balloc(total + 1);
    char* w = out;
    *w++ = '[';
    char buf[32];
    for (int64_t k = 0; k < n; ++k) {
        if (k) { *w++ = ','; *w++ = ' '; }
        switch (l->etag) {
        case 1: {
            const char* f = sg_fmt_f64(((const double*)l->data)[k]);
            while (*f) *w++ = *f++;
            break;
        }
        case 2:
            sg_list_escape(((char* const*)l->data)[k], &w);
            break;
        case 3:
            if (((const int64_t*)l->data)[k]) {
                *w++ = 'T'; *w++ = 'r'; *w++ = 'u'; *w++ = 'e';
            } else {
                *w++ = 'F'; *w++ = 'a'; *w++ = 'l'; *w++ = 's'; *w++ = 'e';
            }
            break;
        default: {
            char* t = sg_i64_str((long long)((const int64_t*)l->data)[k], buf);
            while (*t) *w++ = *t++;
        }
        }
    }
    *w++ = ']';
    *w = 0;
    return out;
}

void sg_print_list(SgList* l) { sg_uart_puts(sg_list_format(l)); }

/* F-22：List[Tuple[k,v]] repr（Dict.items() 打印闭环）——裸机白名单无
 * Dict，保留同构实现供编译器发射路径完整（不可达即无害）。 */
static unsigned long long sg_tpl_bound(const SgList* t, int idx, int tag) {
    int64_t slot = ((const int64_t*)t->data)[idx];
    switch (tag) {
    case 1: return 64;
    case 2: return 2 * strlen((const char*)(unsigned long long)slot) + 10;
    case 3: return 5;
    default: return 20;
    }
}

static void sg_tpl_write(char** wp, const SgList* t, int idx, int tag) {
    char* w = *wp;
    int64_t slot = ((const int64_t*)t->data)[idx];
    char buf[64];
    switch (tag) {
    case 1: {
        double dv;
        __builtin_memcpy(&dv, &slot, 8);
        const char* f = sg_fmt_f64(dv);
        while (*f) *w++ = *f++;
        break;
    }
    case 2:
        sg_list_escape((const char*)(unsigned long long)slot, &w);
        break;
    case 3:
        if (slot) {
            *w++ = 'T'; *w++ = 'r'; *w++ = 'u'; *w++ = 'e';
        } else {
            *w++ = 'F'; *w++ = 'a'; *w++ = 'l'; *w++ = 's'; *w++ = 'e';
        }
        break;
    default: {
        char* t = sg_i64_str((long long)slot, buf);
        while (*t) *w++ = *t++;
    }
    }
    *wp = w;
}

char* sg_list_format_tuples(SgList* l, int64_t ktag, int64_t vtag) {
    int64_t n = l->len;
    unsigned long long total = 2;
    for (int64_t k = 0; k < n; ++k) {
        if (k) total += 2;
        total += 4;
        const SgList* t = (const SgList*)(unsigned long long)((int64_t*)l->data)[k];
        total += sg_tpl_bound(t, 0, (int)ktag);
        total += 2;
        total += sg_tpl_bound(t, 1, (int)vtag);
    }
    char* out = (char*)sg_balloc(total + 1);
    char* w = out;
    *w++ = '[';
    for (int64_t k = 0; k < n; ++k) {
        if (k) { *w++ = ','; *w++ = ' '; }
        const SgList* t = (const SgList*)(unsigned long long)((int64_t*)l->data)[k];
        *w++ = '(';
        sg_tpl_write(&w, t, 0, (int)ktag);
        *w++ = ','; *w++ = ' ';
        sg_tpl_write(&w, t, 1, (int)vtag);
        *w++ = ')';
    }
    *w++ = ']';
    *w = 0;
    return out;
}

/* s.slice(a, b)：字节区间 [a, b)，两端 clamp，a >= b 恒空串。 */
const char* sg_str_slice(const char* s, int64_t a, int64_t b, const char* fn,
                         const char* file, int64_t line) {
    (void)fn; (void)file; (void)line;
    int64_t len = sg_len_str(s);
    if (a < 0) a = 0;
    if (b > len) b = len;
    if (a > b) a = b;
    return sg_bdupn(s + a, (unsigned long long)(b - a));
}

/* ---- 切片归一（与 kCollRuntimeC 的 sg_normslice 逐字同构） ---- */
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

const char* sg_str_nslice(const char* s, int64_t a, int64_t b, int64_t hasA,
                          int64_t hasB) {
    int64_t len = sg_len_str(s);
    sg_normslice(&a, &b, hasA, hasB, len);
    return sg_bdupn(s + a, (unsigned long long)(b - a));
}

/* ---- 类型转换 / 码点 / 计数（panic 消息与 VM doBuiltin 逐字一致） ----
 * int：十进制全消费解析（strtoll base10 语义：前导空白跳过、溢出拒绝）；
 * float：十进制/科学计数全消费（strtod 十进制路径语义；hex 浮点与
 * inf/nan 字面量亦识别——前两者裸机不支持的输入即 panic）。 */
int64_t sg_str_to_i64(const char* s, const char* fn, const char* file,
                      int64_t line) {
    const char* p = s;
    while (sg_is_space((unsigned char)*p)) ++p;
    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = *p == '-';
        ++p;
    }
    if (*p < '0' || *p > '9') goto bad;
    {
        uint64_t acc = 0;
        const char* digits = p;
        while (*p >= '0' && *p <= '9') {
            uint64_t d = (uint64_t)(*p - '0');
            if (acc > (0x7fffffffffffffffull + (neg ? 1 : 0) - d) / 10)
                goto bad; /* 上溢 → 与 ERANGE 同路拒绝 */
            acc = acc * 10 + d;
            ++p;
        }
        if (*p != '\0') goto bad;
        (void)digits;
        if (neg) return (int64_t)(0 - acc);
        return (int64_t)acc;
    }
bad:
    {
        char* buf = (char*)sg_balloc(strlen(s) + 32);
        char* w = buf;
        const char* t = "int() 无法解析 '";
        while (*t) *w++ = *t++;
        const char* q = s;
        while (*q) *w++ = *q++;
        *w++ = '\'';
        *w = 0;
        sg_panic(fn, file, line, buf);
    }
    return 0; /* 不可达 */
}

double sg_str_to_f64(const char* s, const char* fn, const char* file,
                     int64_t line) {
    const char* p = s;
    while (sg_is_space((unsigned char)*p)) ++p;
    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = *p == '-';
        ++p;
    }
    /* inf/nan 字面量（strtod 亦识别） */
    if ((p[0] == 'i' || p[0] == 'I') && (p[1] == 'n' || p[1] == 'N') &&
        (p[2] == 'f' || p[2] == 'F') && p[3] == '\0') {
        union { double d; uint64_t u; } r;
        r.u = 0x7ff0000000000000ull | (neg ? 0x8000000000000000ull : 0);
        return r.d;
    }
    if ((p[0] == 'n' || p[0] == 'N') && (p[1] == 'a' || p[1] == 'A') &&
        (p[2] == 'n' || p[2] == 'N') && p[3] == '\0') {
        union { double d; uint64_t u; } r;
        r.u = 0x7ff8000000000000ull;
        return r.d;
    }
    uint64_t mant = 0;
    int anyDigit = 0;
    int exp10 = 0;
    while (*p >= '0' && *p <= '9') {
        if (mant < 1000000000000000000ull) { /* < 1e18：窗口内精确吸收 */
            mant = mant * 10 + (uint64_t)(*p - '0');
        } else {
            ++exp10; /* 窗口外：降精度计数（mant 已 ≥ 17 位，足够） */
        }
        anyDigit = 1;
        ++p;
    }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            if (mant < 1000000000000000000ull) {
                mant = mant * 10 + (uint64_t)(*p - '0');
                --exp10;
            }
            anyDigit = 1;
            ++p;
        }
    }
    if (!anyDigit) goto badf;
    if (*p == 'e' || *p == 'E') {
        ++p;
        int eneg = 0;
        if (*p == '+' || *p == '-') {
            eneg = *p == '-';
            ++p;
        }
        if (*p < '0' || *p > '9') goto badf;
        long long ev = 0;
        while (*p >= '0' && *p <= '9') {
            ev = ev * 10 + (*p - '0');
            if (ev > 100000) ev = 100000; /* 钳制：越界必 ±inf/0 */
            ++p;
        }
        exp10 += eneg ? -(int)ev : (int)ev;
    }
    if (*p != '\0') goto badf;
    {
        union { double d; uint64_t u; } r;
        r.u = sg_bn_to_double_bits(mant, exp10, neg);
        return r.d;
    }
badf:
    {
        char* buf = (char*)sg_balloc(strlen(s) + 34);
        char* w = buf;
        const char* t = "float() 无法解析 '";
        while (*t) *w++ = *t++;
        const char* q = s;
        while (*q) *w++ = *q++;
        *w++ = '\'';
        *w = 0;
        sg_panic(fn, file, line, buf);
    }
    return 0; /* 不可达 */
}

/* ord(s)：UTF-8 首码点解码；非单码点 panic。 */
int64_t sg_ord(const char* s, const char* fn, const char* file,
               int64_t line) {
    unsigned long long n = strlen(s);
    if (n == 0) sg_panic(fn, file, line, "ord() 需要单字符字符串");
    unsigned char c0 = (unsigned char)s[0];
    int64_t cp = 0;
    unsigned long long len = 0;
    if (c0 < 0x80) {
        cp = c0;
        len = 1;
    } else if ((c0 & 0xE0) == 0xC0) {
        cp = c0 & 0x1F;
        len = 2;
    } else if ((c0 & 0xF0) == 0xE0) {
        cp = c0 & 0x0F;
        len = 3;
    } else if ((c0 & 0xF8) == 0xF0) {
        cp = c0 & 0x07;
        len = 4;
    } else {
        sg_panic(fn, file, line, "ord() 需要单字符字符串");
    }
    if (n != len) sg_panic(fn, file, line, "ord() 需要单字符字符串");
    for (unsigned long long k = 1; k < len; ++k) {
        unsigned char cc = (unsigned char)s[k];
        if ((cc & 0xC0) != 0x80)
            sg_panic(fn, file, line, "ord() 需要单字符字符串");
        cp = (cp << 6) | (cc & 0x3F);
    }
    return cp;
}

/* chr(cp)：代理区一并拒绝；UTF-8 编码 bump 持有。 */
const char* sg_chr(int64_t cp, const char* fn, const char* file,
                   int64_t line) {
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        char buf[48], nb[24];
        char* w = buf;
        const char* t = "chr() 码点越界： ";
        while (*t) *w++ = *t++;
        char* q = sg_i64_str((long long)cp, nb);
        while (*q) *w++ = *q++;
        *w = 0;
        sg_panic(fn, file, line, buf);
    }
    char* out = (char*)sg_balloc(5);
    if (cp < 0x80) {
        out[0] = (char)cp;
        out[1] = 0;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = 0;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = 0;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        out[4] = 0;
    }
    return out;
}

/* s.count(sub)：子串非重叠计数（空子串 = len+1）。 */
int64_t sg_str_count(const char* hay, const char* needle) {
    unsigned long long hl = strlen(hay), sl = strlen(needle);
    if (sl == 0) return (int64_t)hl + 1;
    int64_t n = 0;
    const char* p = hay;
    while ((p = strstr(p, needle)) != 0) {
        ++n;
        p += sl;
    }
    return n;
}

)BRS";

// str/List/Result 尾段；与 kBareRtStrC 按序拼接，fmt() 在 kBareRtFmtC。
inline const char kBareRtStrTailC[] = R"BRS(
/* ---- crc32（与 kStdRuntimeC 逐位一致：IEEE 反射 0xEDB88320） ---- */
int64_t sg_crc32(const char* s) {
    unsigned int c = 0xFFFFFFFFu;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        c ^= *p;
        for (int b = 0; b < 8; ++b)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
    }
    return (int64_t)(c ^ 0xFFFFFFFFu);
}

/* ==== List 运行时（SgList 句柄；etag 0=i64 1=f64 2=str 3=bool） ==== */

SgList* sg_list_empty(void) {
    SgList* l = (SgList*)sg_balloc(sizeof(SgList));
    l->len = 0;
    l->etag = 0;
    l->data = 0;
    return l;
}

SgList* sg_list_new_i(int64_t n, const int64_t* es) {
    SgList* l = sg_list_empty();
    l->len = n;
    l->etag = 0;
    l->data = sg_balloc(sizeof(int64_t) * (unsigned long long)(n > 0 ? n : 1));
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
    l->data = sg_balloc(sizeof(double) * (unsigned long long)(n > 0 ? n : 1));
    for (int64_t k = 0; k < n; ++k) ((double*)l->data)[k] = es[k];
    return l;
}

SgList* sg_list_new_s(int64_t n, const char* const* es) {
    SgList* l = sg_list_empty();
    l->len = n;
    l->etag = 2;
    l->data = sg_balloc(sizeof(char*) * (unsigned long long)(n > 0 ? n : 1));
    for (int64_t k = 0; k < n; ++k) {
        sg_str_mark(es[k]);
        ((char**)l->data)[k] = (char*)es[k];
    }
    return l;
}

int64_t sg_list_len(SgList* l) { return l->len; }

/* 负下标归一 + 越界 panic（与 VM normIndex(what="列表") 逐字一致）。 */
static int64_t sg_list_norm(SgList* l, int64_t idx, const char* fn,
                            const char* file, int64_t line) {
    int64_t orig = idx;
    if (idx < 0) idx += l->len;
    if (idx < 0 || idx >= l->len) {
        char buf[64], nb[24], lb[24];
        char* w = buf;
        const char* t = "列表索引越界：";
        while (*t) *w++ = *t++;
        char* q = sg_i64_str((long long)orig, nb);
        while (*q) *w++ = *q++;
        t = "（长度 ";
        while (*t) *w++ = *t++;
        q = sg_i64_str((long long)l->len, lb);
        while (*q) *w++ = *q++;
        t = "）";
        while (*t) *w++ = *t++;
        *w = 0;
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
void sg_list_store_s(SgList* l, int64_t idx, const char* s, const char* fn,
                     const char* file, int64_t line) {
    if (l->len > 0 && l->etag != 2)
        sg_panic(fn, file, line, "下标赋值类型与列表元素不符");
    ((char**)l->data)[sg_list_norm(l, idx, fn, file, line)] = sg_bdup(s);
}

/* append 标量域：原地尾插（bump 重分配 = 新块拷贝，共享语义不变） */
void sg_list_append_i(SgList* l, int64_t v, const char* fn, const char* file,
                      int64_t line) {
    if (l->len > 0 && l->etag != 0)
        sg_panic(fn, file, line, "append() 实参类型与列表元素不符");
    void* nd = sg_balloc(sizeof(int64_t) *
                         (unsigned long long)(l->len + 1));
    for (int64_t i = 0; i < l->len; ++i) ((int64_t*)nd)[i] = ((int64_t*)l->data)[i];
    ((int64_t*)nd)[l->len] = v;
    l->data = nd;
    ++l->len;
}

void sg_list_append_f(SgList* l, double v, const char* fn, const char* file,
                      int64_t line) {
    if (l->len > 0 && l->etag != 1)
        sg_panic(fn, file, line, "append() 实参类型与列表元素不符");
    if (l->len == 0) l->etag = 1;
    void* nd = sg_balloc(sizeof(double) *
                         (unsigned long long)(l->len + 1));
    for (int64_t i = 0; i < l->len; ++i) ((double*)nd)[i] = ((double*)l->data)[i];
    ((double*)nd)[l->len] = v;
    l->data = nd;
    ++l->len;
}

void sg_list_append_b(SgList* l, int v, const char* fn, const char* file,
                      int64_t line) {
    if (l->len > 0 && l->etag != 3)
        sg_panic(fn, file, line, "append() 实参类型与列表元素不符");
    if (l->len == 0) l->etag = 3;
    void* nd = sg_balloc(sizeof(int64_t) *
                         (unsigned long long)(l->len + 1));
    for (int64_t i = 0; i < l->len; ++i) ((int64_t*)nd)[i] = ((int64_t*)l->data)[i];
    ((int64_t*)nd)[l->len] = v ? 1 : 0;
    l->data = nd;
    ++l->len;
}

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

SgList* sg_list_nslice(SgList* l, int64_t a, int64_t b, int64_t hasA,
                       int64_t hasB) {
    sg_normslice(&a, &b, hasA, hasB, l->len);
    int64_t n = b - a;
    SgList* out = sg_list_empty();
    out->len = n;
    out->etag = l->etag;
    unsigned long long width = (l->etag == 1) ? sizeof(double)
                             : (l->etag == 2) ? sizeof(char*)
                                              : sizeof(int64_t);
    out->data = sg_balloc(width * (unsigned long long)(n > 0 ? n : 1));
    for (int64_t i = 0; i < n; ++i)
        for (unsigned long long j = 0; j < width; ++j)
            ((char*)out->data)[i * width + j] =
                ((char*)l->data)[(a + i) * width + j];
    return out;
}

/* List 归约：op 0=sum 1=min 2=max；空序列 sum=0，min/max panic。 */
int64_t sg_list_reduce_i(int32_t op, SgList* l, const char* name,
                         const char* fn, const char* file, int64_t line) {
    if (l->len == 0 && op != 0) {
        char buf[64], nb[32];
        char* w = buf;
        const char* t = name;
        while (*t) *w++ = *t++;
        t = "() 不支持空序列";
        while (*t) *w++ = *t++;
        *w = 0;
        (void)nb;
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
        char* w = buf;
        const char* t = name;
        while (*t) *w++ = *t++;
        t = "() 不支持空序列";
        while (*t) *w++ = *t++;
        *w = 0;
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

/* ==== Result 运行时（错误即值；载荷按位编码，与宿主 SgResult 同构） ==== */
typedef struct {
    int64_t isOk;
    int64_t bits;
    const char* err;
} SgResult;

static SgResult* sg_result_new(int isOk) {
    SgResult* r = (SgResult*)sg_balloc(sizeof(SgResult));
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
    __builtin_memcpy(&r->bits, &v, 8);
    return r;
}
SgResult* sg_result_ok_b(int v) {
    SgResult* r = sg_result_new(1);
    r->bits = v ? 1 : 0;
    return r;
}
SgResult* sg_result_ok_s(const char* s) {
    SgResult* r = sg_result_new(1);
    sg_str_mark(s);
    r->bits = (int64_t)(unsigned long long)s;
    return r;
}
SgResult* sg_result_ok_n(void) { return sg_result_new(1); }
SgResult* sg_result_err(const char* msg) {
    SgResult* r = sg_result_new(0);
    sg_str_mark(msg);
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
    __builtin_memcpy(&v, &r->bits, 8);
    return v;
}
int sg_result_unwrap_b(SgResult* r, int def) {
    return r->isOk ? (int)r->bits : def;
}
const char* sg_result_unwrap_s(SgResult* r, const char* def) {
    return r->isOk ? (const char*)(unsigned long long)r->bits : def;
}
)BRS";

} // namespace bare
} // namespace scourge
