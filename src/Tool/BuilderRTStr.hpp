// ============================================================================
//  Scourge 编译器 — AOT C 运行时 · str / List 成员方法片段（Builder 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：kRuntimeC 追加片段，build 时拼接写入 <out>_rt.c——必须置于
//  kRuntimeC 之后（依赖其 SgList / sg_dup / sg_panic 定义），缓存键
//  rtObjKey 已计入本片段文本。仅供 Builder.cpp 引用，不置于 include/。
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

/* self 为分隔符、parts 为字符串列表（与 VM StrJoin 语义一致）。空列表无
 * 元素类型（AOT list_empty 句柄 etag=0、VM 空向量同义）→ 恒返回 ""；
 * etag 校验仅对非空列表生效。 */
const char* sg_str_join(const char* sep, SgList* parts, const char* fn,
                        const char* file, int64_t line) {
    if (parts->len > 0 && parts->etag != 2)
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
)RT";
} // namespace scourge