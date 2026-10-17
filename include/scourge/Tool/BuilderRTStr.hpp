// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
// ============================================================================
//  Scourge 编译器 — AOT C 运行时 · str / List 成员方法片段（Builder 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：kRuntimeC 追加片段，build 时拼接写入 <out>_rt.c——必须置于
//  kRuntimeC 之后（依赖 kCollRuntimeC 的 SgList 与 kRuntimeC 的
//  sg_dup / sg_panic 定义），缓存键
//  rtObjKey 已计入本片段文本。仅供 Builder.cpp 引用（内部共享头）。
//  语义契约：与 VM callValueMethod（VMMethod.cpp）panic 消息与比较规则
//  逐字一致，双模差分由 verify 探针锁定：
//    * upper/lower 仅 ASCII；trim 空白集 = 空格 \t \r \n \f \v；
//    * find 未找到 -1；index 未找到 panic；split 空分隔符 panic；
//    * join 元素必须 etag 2（str）；replace 旧子串为空 panic；
//    * List 键 64 位槽 tag：0=i64 1=f64(位型) 2=str 3=bool(zext)，
//      与 dict 键编码一致；etag 0/3 按 i64 精确比较，etag 1 位型比较，
//      etag 2 按字节序 strcmp（tag 与 etag 不符即视为不匹配）。
// ============================================================================
#pragma once

namespace scourge {

inline const char kStrRuntimeC[] = R"RT(
/* ==== str / List 成员方法（0.9.4；与 VM callValueMethod 语义逐字一致） ==== */

static int sg_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

static char* sg_dupn(const char* s, size_t n) {
    char* p = (char*)malloc(n + 1);
    if (!p) exit(1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

const char* sg_str_upper(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (!p) exit(1);
    for (size_t k = 0; k < n; ++k) {
        char c = s[k];
        p[k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    p[n] = 0;
    return p;
}

const char* sg_str_lower(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (!p) exit(1);
    for (size_t k = 0; k < n; ++k) {
        char c = s[k];
        p[k] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    p[n] = 0;
    return p;
}

static const char* sg_trim_impl(const char* s, int left, int right) {
    size_t b = 0, e = strlen(s);
    if (left)
        while (b < e && sg_is_space((unsigned char)s[b])) ++b;
    if (right)
        while (e > b && sg_is_space((unsigned char)s[e - 1])) --e;
    return sg_dupn(s + b, e - b);
}

const char* sg_str_trim(const char* s) { return sg_trim_impl(s, 1, 1); }
const char* sg_str_trim_start(const char* s) { return sg_trim_impl(s, 1, 0); }
const char* sg_str_trim_end(const char* s) { return sg_trim_impl(s, 0, 1); }

/* Python 语义 split：无匹配 → 单元素整串；分隔符邻接 → 空串元素。 */
SgList* sg_str_split(const char* s, const char* sep, const char* fn,
                     const char* file, int64_t line) {
    if (!*sep) sg_panic(fn, file, line, "split() 分隔符不能为空串");
    size_t sl = strlen(sep);
    int64_t n = 1;
    for (const char* p = s; (p = strstr(p, sep)) != NULL; p += sl) ++n;
    SgList* out = sg_list_empty();
    out->len = n;
    out->etag = 2;
    out->data = malloc(sizeof(char*) * (size_t)n);
    if (!out->data) exit(1);
    int64_t k = 0;
    const char* start = s;
    for (const char* p = s; (p = strstr(p, sep)) != NULL; p += sl) {
        ((char**)out->data)[k++] = sg_dupn(start, (size_t)(p - start));
        start = p + sl;
    }
    ((char**)out->data)[k] = sg_dup(start);
    return out;
}

/* List[str].append(str)：原地尾插（共享语义所有别名可见，与 VM
 * shared_ptr push_back 对齐）。SgList 无容量字段，每次 realloc(len+1)——
 * 生态工具（sget）的列表规模小，O(n) 摊销可接受。空列表 etag=0 → 首插
 * 时定型为 2（str）。与 VM 消息逐字一致。 */
void sg_list_append(SgList* l, const char* s, const char* fn,
                    const char* file, int64_t line) {
    if (l->len > 0 && l->etag != 2)
        sg_panic(fn, file, line, "append() 仅支持 List[str]");
    if (l->len == 0 && l->etag == 0) l->etag = 2;
    l->data = realloc(l->data, sizeof(char*) * (size_t)(l->len + 1));
    if (!l->data) exit(1);
    ((char**)l->data)[l->len] = sg_dup(s);
    ++l->len;
}

/* List[str][i] = s（批 7 前置）：下标写 str 版，sg_dup 独立副本（与
 * append / VM std::string 值拷贝同构）；旧槽不回收（arena 纪律）。 */
void sg_list_store_s(SgList* l, int64_t idx, const char* s, const char* fn,
                     const char* file, int64_t line) {
    if (l->len > 0 && l->etag != 2)
        sg_panic(fn, file, line, "下标赋值类型与列表元素不符");
    ((char**)l->data)[sg_list_norm(l, idx, fn, file, line)] = sg_dup(s);
}

/* self 为分隔符、parts 为字符串列表（与 VM StrJoin 语义一致）。空列表无
 * 元素类型（AOT list_empty 句柄 etag=0、VM 空向量同义）→ 恒返回 ""；
 * etag 校验仅对非空列表生效。 */
const char* sg_str_join(const char* sep, SgList* parts, const char* fn,
                        const char* file, int64_t line) {    if (parts->len > 0 && parts->etag != 2)
        sg_panic(fn, file, line, "join() 列表元素必须是 str");
    size_t sepl = strlen(sep);
    int64_t n = parts->len;
    char** es = (char**)parts->data;
    size_t total = 0;
    for (int64_t k = 0; k < n; ++k) total += strlen(es[k]);
    if (n > 0) total += sepl * (size_t)(n - 1);
    char* p = (char*)malloc(total + 1);
    if (!p) exit(1);
    size_t off = 0;
    for (int64_t k = 0; k < n; ++k) {
        if (k) {
            memcpy(p + off, sep, sepl);
            off += sepl;
        }
        size_t el = strlen(es[k]);
        memcpy(p + off, es[k], el);
        off += el;
    }
    p[off] = 0;
    return p;
}

const char* sg_str_replace(const char* s, const char* oldS, const char* newS,
                           const char* fn, const char* file, int64_t line) {
    if (!*oldS) sg_panic(fn, file, line, "replace() 旧子串不能为空串");
    size_t ol = strlen(oldS), nl = strlen(newS);
    int64_t cnt = 0;
    for (const char* p = s; (p = strstr(p, oldS)) != NULL; p += ol) ++cnt;
    char* out = (char*)malloc(
        (size_t)((int64_t)strlen(s) + cnt * ((int64_t)nl - (int64_t)ol)) + 1);
    if (!out) exit(1);
    char* w = out;
    const char* start = s;
    for (const char* p = s; (p = strstr(p, oldS)) != NULL; p += ol) {
        memcpy(w, start, (size_t)(p - start));
        w += p - start;
        memcpy(w, newS, nl);
        w += nl;
        start = p + ol;
    }
    strcpy(w, start);
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
    return strncmp(s, pre, strlen(pre)) == 0;
}

int sg_str_endswith(const char* s, const char* suf) {
    size_t sl = strlen(s), xl = strlen(suf);
    return xl <= sl && memcmp(s + sl - xl, suf, xl) == 0;
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
        memcpy(&v, &key, 8);
        return ((const double*)l->data)[k] == v;
    }
    case 2:
        if (tag != 2) return 0;
        return strcmp((const char*)(intptr_t)key,
                      ((const char* const*)l->data)[k]) == 0;
    }
    return 0;
}

int64_t sg_list_index(SgList* l, int32_t tag, int64_t key, const char* fn,
                      const char* file, int64_t line) {
    for (int64_t k = 0; k < l->len; ++k)
        if (sg_list_match(l, k, tag, key)) return k;
    sg_panic(fn, file, line, "index() 未找到元素");
}

int64_t sg_list_count(SgList* l, int32_t tag, int64_t key) {
    int64_t n = 0;
    for (int64_t k = 0; k < l->len; ++k)
        if (sg_list_match(l, k, tag, key)) ++n;
    return n;
}

/* ==== list repr 格式化（与 VM toString(list, repr=true) 逐字一致） ====
 * "[" + 元素（", " 连接）+ "]"：int %lld / float 最短往返补 ".0"（sg_fmt_f64）
 * / bool True·False / str 带引号转义（仅 \\ " \n \t \r，其余字节原样）。
 * 结果 malloc 持有（程序生命周期）；空列表 data 可为 NULL → "[]"。 */
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

/* 元组/通用 repr 的 str 元素（0.9.6）：外层引号 + 转义（复用
 * sg_list_escape 规则），结果 malloc 持有（程序生命周期）。非 static：
 * lang.str_escape 经 LangToLLVM 声明为跨 TU 符号。上界 = 每字节 2 +
 * 引号 2 + NUL 1。 */
char* sg_str_escape(const char* s) {
    size_t cap = 3;
    for (const char* p = s; *p; ++p) cap += 2;
    char* out = (char*)malloc(cap);
    if (!out) exit(1);
    char* w = out;
    sg_list_escape(s, &w);
    *w = 0;
    return out;
}

char* sg_list_format(SgList* l) {
    /* 两遍：先算总长，再填（元素文本长度有界：int ≤ 20、float ≤ 64、
     * str = 2 + 转义后长度、bool = 4/5）。 */
    int64_t n = l->len;
    size_t total = 2;
    for (int64_t k = 0; k < n; ++k) {
        if (k) total += 2; /* ", " */
        switch (l->etag) {
        case 1: total += 64; break;
        case 2: total += 2 * strlen(((char* const*)l->data)[k]) + 2 + 8; break;
        case 3: total += 5; break;
        default: total += 20;
        }
    }
    char* out = (char*)malloc(total + 1);
    if (!out) exit(1);
    char* w = out;
    *w++ = '[';
    char buf[64];
    for (int64_t k = 0; k < n; ++k) {
        if (k) { *w++ = ','; *w++ = ' '; }
        switch (l->etag) {
        case 1:
            snprintf(buf, sizeof buf, "%s", sg_fmt_f64(((const double*)l->data)[k]));
            memcpy(w, buf, strlen(buf));
            w += strlen(buf);
            break;
        case 2:
            sg_list_escape(((char* const*)l->data)[k], &w);
            break;
        case 3:
            memcpy(w, ((const int64_t*)l->data)[k] ? "True" : "False",
                   ((const int64_t*)l->data)[k] ? 4 : 5);
            w += ((const int64_t*)l->data)[k] ? 4 : 5;
            break;
        default:
            snprintf(buf, sizeof buf, "%lld",
                     (long long)((const int64_t*)l->data)[k]);
            memcpy(w, buf, strlen(buf));
            w += strlen(buf);
        }
    }
    *w++ = ']';
    *w = 0;
    return out;
}

void sg_print_list(SgList* l) { fputs(sg_list_format(l), stdout); }

/* F-22：List[Tuple[k,v]] repr（Dict.items() 打印闭环，与 VM stringify 逐字
 * 一致）：外层列表 i64 槽存 2 元组句柄（SgList*，sg_dict_items 同构），k/v
 * 位按静态 tag 还原 repr（tag 编码同 etag：int=0 float=1 str=2 bool=3；
 * 容器槽位由编译器编译期拒绝，运行时不可达）。两遍分配同 sg_list_format。 */
static size_t sg_tpl_bound(const SgList* t, int idx, int tag) {
    int64_t slot = ((const int64_t*)t->data)[idx];
    switch (tag) {
    case 1: return 64;
    case 2: return 2 * strlen((const char*)(uintptr_t)slot) + 10;
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
        memcpy(&dv, &slot, sizeof dv);
        snprintf(buf, sizeof buf, "%s", sg_fmt_f64(dv));
        memcpy(w, buf, strlen(buf));
        w += strlen(buf);
        break;
    }
    case 2:
        sg_list_escape((const char*)(uintptr_t)slot, &w);
        break;
    case 3:
        memcpy(w, slot ? "True" : "False", slot ? 4 : 5);
        w += slot ? 4 : 5;
        break;
    default:
        snprintf(buf, sizeof buf, "%lld", (long long)slot);
        memcpy(w, buf, strlen(buf));
        w += strlen(buf);
    }
    *wp = w;
}

char* sg_list_format_tuples(SgList* l, int64_t ktag, int64_t vtag) {
    int64_t n = l->len;
    size_t total = 2;
    for (int64_t k = 0; k < n; ++k) {
        if (k) total += 2; /* ", " */
        total += 4;        /* "(" ")" */
        const SgList* t = (const SgList*)(uintptr_t)((int64_t*)l->data)[k];
        total += sg_tpl_bound(t, 0, (int)ktag);
        total += 2; /* ", " 元组分隔 */
        total += sg_tpl_bound(t, 1, (int)vtag);
    }
    char* out = (char*)malloc(total + 1);
    if (!out) exit(1);
    char* w = out;
    *w++ = '[';
    for (int64_t k = 0; k < n; ++k) {
        if (k) { *w++ = ','; *w++ = ' '; }
        const SgList* t = (const SgList*)(uintptr_t)((int64_t*)l->data)[k];
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

/* s.slice(a, b)：字节区间 [a, b)，两端 clamp 到 [0, len]，a >= b 恒空串
 * （与 VM StrSlice 语义逐字一致）。结果 arena 持有（程序生命周期）。 */
const char* sg_str_slice(const char* s, int64_t a, int64_t b, const char* fn,
                         const char* file, int64_t line) {
    (void)fn; (void)file; (void)line;
    int64_t len = (int64_t)strlen(s);
    if (a < 0) a = 0;
    if (b > len) b = len;
    if (a > b) a = b;
    size_t n = (size_t)(b - a);
    char* p = (char*)malloc(n + 1);
    if (!p) exit(1);
    memcpy(p, s + a, n);
    p[n] = 0;
    return p;
}

/* ---- 类型转换 / 码点 / 计数（0.9.9；panic 消息与 VM doBuiltin 逐字一致）。
 * int 判 ERANGE（strtoll 上溢拒绝）；float 不判 errno（strtod 上溢 → ±inf
 * 与 Python 同语义）；两者均全消费校验（前导空白由 strtoll/strtod 自跳，
 * 与 VM std::strtoll/std::strtod 路径同一行为）。 ---- */
int64_t sg_str_to_i64(const char* s, const char* fn, const char* file,
                      int64_t line) {
    char* end = NULL;
    errno = 0;
    long long r = strtoll(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0') {
        char* buf = (char*)sg_xmalloc(strlen(s) + 32);
        snprintf(buf, strlen(s) + 32, "int() 无法解析 '%s'", s);
        sg_panic(fn, file, line, buf);
    }
    return (int64_t)r;
}

double sg_str_to_f64(const char* s, const char* fn, const char* file,
                     int64_t line) {
    char* end = NULL;
    double r = strtod(s, &end);
    if (end == s || *end != '\0') {
        char* buf = (char*)sg_xmalloc(strlen(s) + 34);
        snprintf(buf, strlen(s) + 34, "float() 无法解析 '%s'", s);
        sg_panic(fn, file, line, buf);
    }
    return r;
}

/* ord(s)：UTF-8 首码点解码；非单码点（空串/多字符/坏续字节）panic。 */
int64_t sg_ord(const char* s, const char* fn, const char* file,
               int64_t line) {
    size_t n = strlen(s);
    if (n == 0) sg_panic(fn, file, line, "ord() 需要单字符字符串");
    unsigned char c0 = (unsigned char)s[0];
    int64_t cp = 0;
    size_t len = 0;
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
    for (size_t k = 1; k < len; ++k) {
        unsigned char cc = (unsigned char)s[k];
        if ((cc & 0xC0) != 0x80)
            sg_panic(fn, file, line, "ord() 需要单字符字符串");
        cp = (cp << 6) | (cc & 0x3F);
    }
    return cp;
}

/* chr(cp)：代理区（D800-DFFF）非标量码点一并拒绝；UTF-8 编码 arena 持有。 */
const char* sg_chr(int64_t cp, const char* fn, const char* file,
                   int64_t line) {
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        char buf[48];
        snprintf(buf, sizeof buf, "chr() 码点越界： %lld", (long long)cp);
        sg_panic(fn, file, line, buf);
    }
    char* out = (char*)sg_xmalloc(5);
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

/* s.count(sub)：子串非重叠计数（空子串 = len+1，与 VM/Python 一致）。 */
int64_t sg_str_count(const char* hay, const char* needle) {
    size_t hl = strlen(hay), sl = strlen(needle);
    if (sl == 0) return (int64_t)hl + 1;
    int64_t n = 0;
    const char* p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        ++n;
        p += sl;
    }
    return n;
}

/* ---- 切片 s[a:b]（0.9.9）：归一复用 kCollRuntimeC 的 sg_normslice（在前
 * 片段，单一事实源）；str 按字节索引（P1 语义）。纯函数无 panic 路径；
 * 结果 malloc 持有（程序生命周期）。 ---- */
const char* sg_str_nslice(const char* s, int64_t a, int64_t b, int64_t hasA,
                          int64_t hasB) {
    int64_t len = (int64_t)strlen(s);
    sg_normslice(&a, &b, hasA, hasB, len);
    size_t n = (size_t)(b - a);
    char* p = (char*)malloc((size_t)n + 1);
    if (!p) exit(1);
    memcpy(p, s + a, n);
    p[n] = 0;
    return p;
}

/* ==== fmt() 格式化（0.9.9；f-string :spec 降糖目标）====
 * 与 VM doBuiltin fmt 分支（VMBuiltin.cpp 匿名 namespace）逐字同构：
 * spec 文法静态约束见 Sema validateFmtSpec（非法 spec 已在编译期拒绝，
 * 此处不设防）。snprintf 产主体 + UTF-8 码点计宽填充——同库同机下
 * 双模逐字节一致。结果 malloc 持有（程序生命周期）。 ---- */
typedef struct {
    char fill;  /* 缺省 ' ' */
    char align; /* 0 = 类型默认（数值右对齐、str 左对齐） */
    int plus;
    int zero;
    int width;
    int prec; /* -1 = 未指定 */
    char type;
} SgFmtSpec;

static SgFmtSpec sg_fmtspec_parse(const char* s) {
    SgFmtSpec f;
    f.fill = ' ';
    f.align = 0;
    f.plus = 0;
    f.zero = 0;
    f.width = 0;
    f.prec = -1;
    f.type = 0;
    size_t i = 0, n = strlen(s);
    if (i < n && (s[i] == '<' || s[i] == '>' || s[i] == '^')) {
        f.align = s[i++];
    } else if (n - i >= 2 &&
               (s[i + 1] == '<' || s[i + 1] == '>' || s[i + 1] == '^')) {
        f.fill = s[i];
        f.align = s[i + 1];
        i += 2;
    }
    if (i < n && s[i] == '+') {
        f.plus = 1;
        ++i;
    }
    if (i < n && s[i] == '0' && i + 1 < n && s[i + 1] >= '0' &&
        s[i + 1] <= '9') {
        f.zero = 1;
        ++i;
    }
    /* width/prec 上限与 Sema validateFmtSpec 同值（1000000 / 100）；
     * 静态校验是唯一事实源，此处饱和累加仅防篡改输入引发有符号溢出。 */
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        int d = s[i++] - '0';
        f.width = f.width > (1000000 - d) / 10 ? 1000000 : f.width * 10 + d;
    }
    if (i < n && s[i] == '.') {
        f.prec = 0;
        ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            int d = s[i++] - '0';
            f.prec = f.prec > (100 - d) / 10 ? 100 : f.prec * 10 + d;
        }
    }
    if (i < n) f.type = s[i];
    return f;
}

/* UTF-8 码点数（续字节 10xxxxxx 不计）。 */
static size_t sg_cp_count(const char* s) {
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
        if ((*p & 0xC0) != 0x80) ++n;
    return n;
}

/* 截断到前 prec 个码点的字节长度（fmt_str 专用）。 */
static size_t sg_cp_trunc_bytes(const char* s, size_t prec) {
    size_t n = 0;
    for (size_t k = 0; s[k]; ++k) {
        if (((unsigned char)s[k] & 0xC0) != 0x80) {
            if (n == prec) return k;
            ++n;
        }
    }
    return strlen(s);
}

/* 码点计宽填充：'>' 左填充 / '<' 右填充 / '^' 居中（余数偏右，与 Python
 * 一致）；缺省对齐由调用方按类型传（数值 '>'、str '<'）。fill 取单字节。
 * 结果 malloc 持有。 */
static char* sg_cp_pad(const char* body, SgFmtSpec f, char defAlign) {
    char align = f.align ? f.align : defAlign;
    size_t cps = sg_cp_count(body);
    size_t pad =
        f.width > (int)cps ? (size_t)f.width - cps : 0;
    size_t bl = strlen(body);
    char* out = (char*)malloc(bl + pad + 1);
    if (!out) exit(1);
    char* w = out;
    if (align == '<') {
        memcpy(w, body, bl);
        w += bl;
        memset(w, f.fill, pad);
        w += pad;
    } else if (align == '^') {
        size_t left = pad / 2;
        memset(w, f.fill, left);
        w += left;
        memcpy(w, body, bl);
        w += bl;
        memset(w, f.fill, pad - left);
        w += pad - left;
    } else {
        memset(w, f.fill, pad);
        w += pad;
        memcpy(w, body, bl);
        w += bl;
    }
    *w = 0;
    return out;
}

/* 数值主体统一处理：符号剥离 → 零填充（旗标时插符号后）→ 宽度填充。 */
static char* sg_fmt_signed(const char* num, int neg, SgFmtSpec f) {
    const char* sign = neg ? "-" : (f.plus ? "+" : "");
    if (f.zero && !f.align) {
        size_t body = strlen(sign) + strlen(num);
        size_t zeros = f.width > (int)body ? (size_t)f.width - body : 0;
        char* out = (char*)malloc(strlen(sign) + zeros + strlen(num) + 1);
        if (!out) exit(1);
        strcpy(out, sign);
        memset(out + strlen(sign), '0', zeros);
        strcpy(out + strlen(sign) + zeros, num);
        return out;
    }
    char* joined = (char*)malloc(strlen(sign) + strlen(num) + 1);
    if (!joined) exit(1);
    strcpy(joined, sign);
    strcat(joined, num);
    char* out = sg_cp_pad(joined, f, '>');
    free(joined);
    return out;
}

const char* sg_format_i64(int64_t v, const char* spec) {
    SgFmtSpec f = sg_fmtspec_parse(spec);
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    return sg_fmt_signed(buf + (buf[0] == '-' ? 1 : 0), v < 0, f);
}

const char* sg_format_f64(double v, const char* spec) {
    SgFmtSpec f = sg_fmtspec_parse(spec);
    int prec = f.prec >= 0 ? f.prec : 6;
    char type = f.type ? f.type : 'f';
    char buf[512];
    if (type == '%') {
        snprintf(buf, sizeof buf, "%.*f%%", prec, v * 100.0);
    } else {
        char fs[16];
        snprintf(fs, sizeof fs, "%%.%d%c", prec, type); /* "%.Nf/e/g" */
        snprintf(buf, sizeof buf, fs, v);
    }
    return sg_fmt_signed(buf + (buf[0] == '-' ? 1 : 0), v < 0, f);
}

const char* sg_format_str(const char* v, const char* spec) {
    SgFmtSpec f = sg_fmtspec_parse(spec);
    char* body;
    int owned = 0;
    if (f.prec >= 0) {
        size_t nb = sg_cp_trunc_bytes(v, (size_t)f.prec);
        body = (char*)malloc(nb + 1);
        if (!body) exit(1);
        memcpy(body, v, nb);
        body[nb] = 0;
        owned = 1;
    } else {
        body = (char*)v;
    }
    char* out = sg_cp_pad(body, f, '<');
    if (owned) free(body);
    return out;
}

const char* sg_format_bool(int v, const char* spec) {
    return sg_format_str(v ? "True" : "False", spec);
}
)RT";
} // namespace scourge