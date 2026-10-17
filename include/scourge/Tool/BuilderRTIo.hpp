// ============================================================================
//  Scourge 编译器 — AOT C 运行时 · 排序与 IO 段（Builder 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：kRuntimeC 第 4 段——sorted 稳定升序（(值, 原下标) 装饰 + qsort，
//  NaN 显式拒绝）与文件 IO（write/exists/listdir + IO 错误消息构造）。
//  依赖 kRuntimeC 的 sg_panic 与 kCollRuntimeC 的 SgList。
// ============================================================================
#pragma once

namespace scourge {

inline const char kIoRuntimeC[] = R"RT(
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

/* ---- IO 内建（0.9.4）：函数式文件/目录/标准输入。与 VM 的约定逐字对齐：
 * 路径窄字节串（ANSI 代码页语义）；文本一律二进制模式（禁 CRT 文本转换，
 * 否则 CRLF 差异破坏双模逐字节一致）；失败 panic 消息与 VM panicHere 逐字
 * 一致；list_dir 结果按字节序排序（枚举顺序不可依赖——确定性红线）。 ---- */
static char* sg_io_msg(const char* pre, const char* path) {
    size_t a = strlen(pre), b = strlen(path);
    char* m = (char*)malloc(a + b + 1);
    if (!m) exit(1);
    memcpy(m, pre, a);
    memcpy(m + a, path, b + 1);
    return m;
}

/* fgetc 逐字节到 '\n'（与 VM 同构）；去行尾 '\r'（二进制重定向时出现）。 */
const char* sg_input(const char* fn, const char* file, int64_t line) {
    size_t cap = 256, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) exit(1);
    int c, got = 0;
    while ((c = fgetc(stdin)) != EOF) {
        got = 1;
        if (c == '\n') break;
        if (len + 1 >= cap) {
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) exit(1);
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    if (!got)
        sg_panic(fn, file, line, "input() 输入流已结束");
    while (len > 0 && buf[len - 1] == '\r') buf[--len] = 0;
    buf[len] = 0;
    return buf;
}

const char* sg_read_text(const char* path, const char* fn, const char* file,
                         int64_t line) {
    FILE* f = fopen(path, "rb");
    if (!f)
        sg_panic(fn, file, line,
                 sg_io_msg("read_text() 无法打开文件: ", path));
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        sg_panic(fn, file, line,
                 sg_io_msg("read_text() 无法读取文件: ", path));
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        sg_panic(fn, file, line,
                 sg_io_msg("read_text() 无法读取文件: ", path));
    }
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) exit(1);
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if ((long)rd != n)
        sg_panic(fn, file, line,
                 sg_io_msg("read_text() 无法读取文件: ", path));
    buf[n] = 0;
    return buf;
}

/* write/append 公共尾：mode "wb"/"ab"；写入或关闭失败 panic（消息与 VM 一致）。 */
static int64_t sg_write_file(const char* path, const char* data,
                             const char* mode, const char* name,
                             const char* fn, const char* file, int64_t line) {
    char pre[64];
    snprintf(pre, sizeof pre, "%s() 无法打开文件: ", name);
    FILE* f = fopen(path, mode);
    if (!f)
        sg_panic(fn, file, line, sg_io_msg(pre, path));
    size_t len = strlen(data);
    int ok = fwrite(data, 1, len, f) == len;
    if (ok)
        ok = fclose(f) == 0;
    else
        fclose(f);
    if (!ok) {
        snprintf(pre, sizeof pre, "%s() 写入失败: ", name);
        sg_panic(fn, file, line, sg_io_msg(pre, path));
    }
    return (int64_t)len;
}

int64_t sg_write_text(const char* path, const char* data, const char* fn,
                      const char* file, int64_t line) {
    return sg_write_file(path, data, "wb", "write_text", fn, file, line);
}

int64_t sg_append_text(const char* path, const char* data, const char* fn,
                       const char* file, int64_t line) {
    return sg_write_file(path, data, "ab", "append_text", fn, file, line);
}

int sg_exists(const char* path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    return access(path, F_OK) == 0;
#endif
}

static int sg_sort_names(const void* x, const void* y) {
    return strcmp(*(const char* const*)x, *(const char* const*)y);
}

SgList* sg_list_dir(const char* path, const char* fn, const char* file,
                    int64_t line) {
    /* 收集（跳过 "." ".."，与 VM 显式过滤对齐）→ qsort 字节序 → SgList。 */
    size_t cap = 16, cnt = 0;
    char** names = (char**)malloc(cap * sizeof(char*));
    if (!names) exit(1);
#ifdef _WIN32
    /* 通配拼接：尾分隔符直接加 '*'，否则补 '\\'（与 VM directory_iterator
     * 枚举"直接子项"语义一致）。 */
    size_t pl = strlen(path);
    int hasSep = pl > 0 && (path[pl - 1] == '\\' || path[pl - 1] == '/');
    char* pattern = (char*)malloc(pl + 3);
    if (!pattern) exit(1);
    memcpy(pattern, path, pl);
    size_t k = pl;
    if (!hasSep) pattern[k++] = '\\';
    pattern[k++] = '*';
    pattern[k] = 0;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE)
        sg_panic(fn, file, line,
                 sg_io_msg("list_dir() 无法打开目录: ", path));
    do {
        const char* nm = fd.cFileName;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
            continue;
        if (cnt + 1 >= cap) {
            cap *= 2;
            char** nb = (char**)realloc(names, cap * sizeof(char*));
            if (!nb) exit(1);
            names = nb;
        }
        names[cnt++] = sg_dup(nm);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    /* POSIX: opendir/readdir 直接枚举"直接子项"（无需通配拼接）。 */
    DIR* d = opendir(path);
    if (!d)
        sg_panic(fn, file, line,
                 sg_io_msg("list_dir() 无法打开目录: ", path));
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        const char* nm = de->d_name;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0)))
            continue;
        if (cnt + 1 >= cap) {
            cap *= 2;
            char** nb = (char**)realloc(names, cap * sizeof(char*));
            if (!nb) exit(1);
            names = nb;
        }
        names[cnt++] = sg_dup(nm);
    }
    closedir(d);
#endif
    qsort(names, cnt, sizeof(char*), sg_sort_names);

    SgList* out = sg_list_empty();
    out->len = (int64_t)cnt;
    out->etag = 2;
    out->data = malloc(sizeof(char*) * (cnt > 0 ? cnt : 1));
    if (!out->data) exit(1);
    for (size_t j = 0; j < cnt; ++j) ((char**)out->data)[j] = names[j];
    free(names);
    return out;
}
)RT";

} // namespace scourge
