// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  AOT C 运行时片段 — 系统编程内建（0.9.4 H 专项：args/env/时间/进程/信号）
//  跨平台支持（0.9.4）：Windows / POSIX 双平台单一契约（差异逐处注释）
// ----------------------------------------------------------------------------
//  职责边界：与 BuilderRTNet.hpp 同模式——build 时拼接在 kRuntimeC/kStrRuntimeC
//  之后落盘（复用其 SgList/sg_panic/sg_io_msg 定义），缓存键 rtObjKey 已计入
//  本片段文本。@extern 的 sg_extern_* 解析器也归本片段（FFI 与系统内建同为
//  "进程外能力"，VM 侧对应 VMSys.cpp/VMExtern.cpp）。
//  语义契约（与 src/BC/VMSys.cpp 逐字一致，双模差分由 verify 探针锁定）：
//    * args() 窄字节 = Windows ANSI 代码页解码 / POSIX /proc/self/cmdline
//      原始字节（UTF-8 环境）；不含程序名（argv[0]）；
//    * env 缺失返回 ""（getenv 语义）；set_env 返回 bool（名含 '=' 或空拒绝）；
//    * localtime/gmtime 返回 [年,月,日,时,分,秒,星期0=周日,年积日(1 起)]；
//      转换失败 panic（消息与 VM 逐字一致）；
//    * system(cmd) 直通 CRT system()（shell 直通，输出继承，返回退出码）；
//    * spawn_wait：Windows 用 CreateProcessW（引号规则 = MSVC 解析规则）；
//      POSIX 用 fork/execvp（argv 数组直传，无引号层）；启动失败 panic，
//      正常返回退出码（POSIX 被信号终止 = 128+信号号，shell 惯例）；
//    * 信号 = 原子标志轮询：Windows 捕获 CTRL_C/CTRL_BREAK；POSIX 注册
//      SIGINT/SIGTERM 处理器置标志（注册后默认终止行为被接管）；
//    * 时间返回串/列表 malloc（与 sg_read_text 同约定）。
//  本文件常量字符串仅含中文（panic 消息对齐 VM），与 kRuntimeC 内嵌源同
//  处理（cl /utf-8 由 Builder 编译参数保证）。
// ============================================================================
#pragma once

inline const char kSysRuntimeC[] = R"NT(/* Scourge AOT 运行时 — 系统编程片段（H 专项；语义对齐 VMSys.cpp）。 */
#ifdef _WIN32
#include <shellapi.h> /* CommandLineToArgvW（链接 -lshell32） */
#else
/* POSIX（Linux/macOS 与 BSD）：进程/信号/动态库的 POSIX 对应物（跨平台支持）。 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif
#include <time.h>

/* ---- 命令行参数（不含程序名；Windows = ANSI 代码页，POSIX = 原始字节） ---- */
#ifdef _WIN32
SgList* sg_args(void) {
    int nw = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &nw);
    if (!w) { /* 解析失败退化为仅程序名 → 空参数表 */
        SgList* out = sg_list_empty();
        out->etag = 2;
        return out;
    }
    int64_t n = (nw > 1) ? (nw - 1) : 0;
    SgList* out = sg_list_empty();
    out->len = n;
    out->etag = 2;
    out->data = malloc(sizeof(char*) * (size_t)(n > 0 ? n : 1));
    if (!out->data) exit(1);
    for (int64_t k = 0; k < n; ++k) {
        int bytes = WideCharToMultiByte(CP_ACP, 0, w[k + 1], -1, NULL, 0,
                                        NULL, NULL);
        char* s = (char*)malloc((size_t)(bytes > 0 ? bytes : 1));
        if (!s) exit(1);
        if (bytes > 0)
            WideCharToMultiByte(CP_ACP, 0, w[k + 1], -1, s, bytes, NULL, NULL);
        else
            s[0] = 0;
        ((char**)out->data)[k] = s;
    }
    LocalFree(w);
    return out;
}
#else
SgList* sg_args(void) {
    SgList* out = sg_list_empty();
    out->etag = 2;
    FILE* f = fopen("/proc/self/cmdline", "rb");
    if (!f) return out; /* 无 procfs 环境：空参数表（与解析失败退化一致） */
    size_t cap = 4096, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) exit(1);
    for (;;) {
        if (len == cap) {
            if (cap > (1u << 20)) break; /* 1MiB 上限（防病态输入） */
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) exit(1);
            buf = nb;
        }
        size_t r = fread(buf + len, 1, cap - len, f);
        if (r == 0) break;
        len += r;
    }
    fclose(f);
    if (len == 0) {
        free(buf);
        return out;
    }
    /* NUL 段拆分；段 0 = 程序名（跳过，与 Windows 不含 argv[0] 对齐）。 */
    int64_t n = -1;
    for (size_t k = 0; k < len;) {
        ++n;
        while (k < len && buf[k]) ++k;
        ++k;
    }
    out->len = n > 0 ? n : 0;
    out->data = malloc(sizeof(char*) * (size_t)(out->len > 0 ? out->len : 1));
    if (!out->data) exit(1);
    int64_t w = 0, idx = 0;
    for (size_t k = 0; k < len;) {
        size_t e = k;
        while (e < len && buf[e]) ++e;
        if (idx > 0) {
            char* s = (char*)malloc((size_t)(e - k) + 1);
            if (!s) exit(1);
            memcpy(s, buf + k, (size_t)(e - k));
            s[e - k] = 0;
            ((char**)out->data)[w++] = s;
        }
        ++idx;
        k = e + 1;
    }
    free(buf);
    return out;
}
#endif

/* ---- 环境变量（getenv 双平台同源；写入差异在平台分支） ---- */
const char* sg_env(const char* name) {
    const char* v = getenv(name);
    return v ? v : "";
}

int sg_has_env(const char* name) { return getenv(name) != NULL; }

#ifdef _WIN32
int sg_set_env(const char* name, const char* value) {
    if (!*name || strchr(name, '=')) return 0;
    char* kv = (char*)malloc(strlen(name) + strlen(value) + 2);
    if (!kv) exit(1);
    sprintf(kv, "%s=%s", name, value);
    int ok = _putenv(kv) == 0;
    free(kv);
    return ok;
}

void sg_unset_env(const char* name) {
    if (!*name || strchr(name, '=')) return; /* 与 VM 语义一致：静默无操作 */
    char* kv = (char*)malloc(strlen(name) + 2);
    if (!kv) exit(1);
    sprintf(kv, "%s=", name);
    _putenv(kv); /* "name=" 删除该变量 */
    free(kv);
}
#else
int sg_set_env(const char* name, const char* value) {
    if (!*name || strchr(name, '=')) return 0;
    return setenv(name, value, 1) == 0; /* 覆盖语义与 Windows _putenv 一致 */
}

void sg_unset_env(const char* name) {
    if (!*name || strchr(name, '=')) return; /* 与 VM 语义一致：静默无操作 */
    unsetenv(name);
}
#endif

/* ---- 时间与日期 ---- */
int64_t sg_epoch(void) { return (int64_t)time(NULL); }

#ifdef _WIN32
int64_t sg_epoch_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    /* 1601-01-01 → 1970-01-01 的 100ns 间隔（11644473600 秒） */
    int64_t t = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - (int64_t)11644473600ll * 10000000ll) / 10000ll;
}
#else
int64_t sg_epoch_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts); /* 墙钟毫秒，与 FILETIME 同语义 */
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

/* 日历拆解公共体：返回 [年,月,日,时,分,秒,星期0=周日,年积日(1 起)]。 */
static SgList* sg_cal_split(int64_t ep, int useLocal, const char* fn,
                            const char* file, int64_t line) {
    time_t t = (time_t)ep;
    struct tm tmv;
#ifdef _WIN32
    int rc = useLocal ? localtime_s(&tmv, &t) : gmtime_s(&tmv, &t);
    if (rc != 0)
#else
    struct tm* pr = useLocal ? localtime_r(&t, &tmv) : gmtime_r(&t, &tmv);
    if (!pr)
#endif
        sg_panic(fn, file, line,
                 useLocal ? "localtime() 时间值超出可表示范围"
                          : "gmtime() 时间值超出可表示范围");
    SgList* out = sg_list_empty();
    out->len = 8;
    out->etag = 0;
    out->data = malloc(sizeof(int64_t) * 8);
    if (!out->data) exit(1);
    int64_t* d = (int64_t*)out->data;
    d[0] = tmv.tm_year + 1900;
    d[1] = tmv.tm_mon + 1;
    d[2] = tmv.tm_mday;
    d[3] = tmv.tm_hour;
    d[4] = tmv.tm_min;
    d[5] = tmv.tm_sec;
    d[6] = tmv.tm_wday;
    d[7] = tmv.tm_yday + 1;
    return out;
}

SgList* sg_localtime(int64_t ep, const char* fn, const char* file,
                     int64_t line) {
    return sg_cal_split(ep, 1, fn, file, line);
}

SgList* sg_gmtime(int64_t ep, const char* fn, const char* file, int64_t line) {
    return sg_cal_split(ep, 0, fn, file, line);
}

/* ---- 进程 ---- */
int64_t sg_system(const char* cmd) { return (int64_t)system(cmd); }

#ifdef _WIN32

/* 引号规则：空串或含空格/制表符 → 加引号；内嵌引号翻倍（MSVC 解析规则，
 * 与 VM VMSys.cpp 逐字一致）。 */
static void sg_spawn_quote(const char* s, char** w) {
    int need = !*s || strchr(s, ' ') || strchr(s, '\t');
    if (need) *(*w)++ = '"';
    for (const char* p = s; *p; ++p) {
        if (*p == '"') *(*w)++ = '"';
        *(*w)++ = *p;
    }
    if (need) *(*w)++ = '"';
}

int64_t sg_spawn_wait(const char* path, SgList* args, const char* fn,
                      const char* file, int64_t line) {
    /* 命令行总长：path 引号化 ≤ 2*len+2，各参数同理 + 空格分隔 */
    int64_t n = args->len;
    char** es = (char**)args->data;
    size_t total = 2 * strlen(path) + 3;
    for (int64_t k = 0; k < n; ++k) total += 2 * strlen(es[k]) + 4;
    char* cmdline = (char*)malloc(total + 1);
    if (!cmdline) exit(1);
    char* w = cmdline;
    sg_spawn_quote(path, &w);
    for (int64_t k = 0; k < n; ++k) {
        *w++ = ' ';
        sg_spawn_quote(es[k], &w);
    }
    *w = 0;
    int wlen = MultiByteToWideChar(CP_ACP, 0, cmdline, -1, NULL, 0);
    wchar_t* wcmd = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)wlen);
    if (!wcmd) exit(1);
    MultiByteToWideChar(CP_ACP, 0, cmdline, -1, wcmd, wlen);
    free(cmdline);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, 0, NULL, NULL, &si,
                        &pi)) {
        char* msg = sg_io_msg("spawn_wait() 无法启动进程: ", path);
        free(wcmd);
        sg_panic(fn, file, line, msg);
    }
    free(wcmd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int64_t)code;
}

#else

int64_t sg_spawn_wait(const char* path, SgList* args, const char* fn,
                      const char* file, int64_t line) {
    /* POSIX: argv 数组直传（无 MSVC 引号层，与 VM VMSys.cpp 分支一致）。 */
    int64_t n = args->len;
    char** es = (char**)args->data;
    char** argv = (char**)malloc(sizeof(char*) * (size_t)(n + 2));
    if (!argv) exit(1);
    argv[0] = (char*)path;
    for (int64_t k = 0; k < n; ++k) argv[k + 1] = es[k];
    argv[n + 1] = NULL;
    /* exec 状态管道：exec 成功即关（FD_CLOEXEC）→ 父端 read 返回 0；
     * 失败则子进程写回 errno。用于区分"启动失败"与"退出码 127"。 */
    int pfd[2];
    if (pipe(pfd) != 0) {
        free(argv);
        sg_panic(fn, file, line, "spawn_wait() 无法创建进程状态管道");
    }
    fcntl(pfd[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        free(argv);
        char* msg = sg_io_msg("spawn_wait() 无法启动进程: ", path);
        sg_panic(fn, file, line, msg);
    }
    if (pid == 0) {
        close(pfd[0]);
        execvp(path, argv);
        int e = errno;
        ssize_t ignored = write(pfd[1], &e, sizeof(e));
        (void)ignored;
        _exit(127);
    }
    close(pfd[1]);
    free(argv);
    int err = 0;
    ssize_t r = read(pfd[0], &err, sizeof(err));
    close(pfd[0]);
    if (r == (ssize_t)sizeof(err)) {
        /* exec 失败：与 Windows CreateProcessW 失败同一 panic 消息 */
        char* msg = sg_io_msg("spawn_wait() 无法启动进程: ", path);
        sg_panic(fn, file, line, msg);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        sg_panic(fn, file, line, "spawn_wait() 等待进程失败");
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status); /* shell 惯例 */
    return 1;
}

#endif

/* ---- 信号（原子标志轮询；注册后默认终止行为被接管） ---- */
#ifdef _WIN32
static volatile LONG sg_sig_flag = 0;

static BOOL WINAPI sg_sig_handler(DWORD t) {
    if (t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT) {
        InterlockedExchange(&sg_sig_flag, 1);
        return TRUE; /* 已处理：默认终止行为被接管 */
    }
    return FALSE;
}

static void sg_signal_ensure(void) {
    static int done = 0;
    if (!done) {
        SetConsoleCtrlHandler(sg_sig_handler, TRUE);
        done = 1;
    }
}

int sg_signal_caught(void) {
    sg_signal_ensure();
    return InterlockedExchangeAdd(&sg_sig_flag, 0) != 0;
}

void sg_signal_reset(void) {
    sg_signal_ensure();
    InterlockedExchange(&sg_sig_flag, 0);
}
#else
static volatile sig_atomic_t sg_sig_flag = 0;

static void sg_sig_handler(int sig) {
    (void)sig; /* SIGINT/SIGTERM → 置标志（接管默认终止，语义对齐 Windows） */
    sg_sig_flag = 1;
}

static void sg_signal_ensure(void) {
    static int done = 0;
    if (!done) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sg_sig_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; /* 无 SA_RESTART：慢 syscall 返回 EINTR 可感知中断 */
        sigaction(SIGINT, &sa, NULL);  /* Ctrl+C，对齐 CTRL_C_EVENT */
        sigaction(SIGTERM, &sa, NULL); /* 对齐 CTRL_BREAK 的终止类信号 */
        done = 1;
    }
}

int sg_signal_caught(void) {
    sg_signal_ensure();
    return sg_sig_flag != 0;
}

void sg_signal_reset(void) {
    sg_signal_ensure();
    sg_sig_flag = 0;
}
#endif

/* ---- @extern（H 专项）：动态库符号解析 + str 返回深拷贝 ----
 * 缓存 dll!sym → 函数地址（cap 256，与 VMExtern.cpp 一致）；库句柄表防
 * 重复加载（cap 64）。Windows = LoadLibraryA/GetProcAddress；POSIX =
 * dlopen/dlsym（用户传 "libfoo.so" 或路径）。panic 消息与 VMExtern.cpp
 * 逐字一致。 */
#define SG_EXTERN_CAP 256

typedef struct {
    char* key; /* "dll!sym"（malloc） */
    void* fn;  /* GetProcAddress/dlsym 结果 */
} SgExternEnt;

static SgExternEnt sg_extern_cache[SG_EXTERN_CAP];
static int sg_extern_n = 0;

#ifdef _WIN32
static HMODULE sg_extern_mods[64];
#define SG_EXTERN_MODNAME_MAX MAX_PATH
#else
static void* sg_extern_mods[64];
#define SG_EXTERN_MODNAME_MAX 1024
#endif
static char sg_extern_modnames[64][SG_EXTERN_MODNAME_MAX]; /* 用户书写名（防重复加载） */
static int sg_extern_modn = 0;

void* sg_extern_resolve(const char* dll, const char* sym, const char* fn,
                        const char* file, int64_t line) {
    /* ffi 能力运行时门禁（0.9.7）：@extern 调用需 ffi 授权——what 传 sym
     * （Scourge 函数名，与 VM doExtern 的 fn.name 同源），消息与 VM 逐字
     * 一致。宏由 capRuntimeC 片段生成（kRuntimeC 之后、本片段之前）。 */
    sg_cap_check(SG_CAP_FFI_BIT, sym, fn, file, line);
    size_t dl = strlen(dll), sl = strlen(sym);
    for (int k = 0; k < sg_extern_n; ++k) {
        const char* key = sg_extern_cache[k].key;
        if (strncmp(key, dll, dl) == 0 && key[dl] == '!' &&
            strcmp(key + dl + 1, sym) == 0)
            return sg_extern_cache[k].fn;
    }
    if (sg_extern_n >= SG_EXTERN_CAP)
        sg_panic(fn, file, line, "extern_call() 动态库缓存已满");
    void* h = NULL;
    for (int k = 0; k < sg_extern_modn; ++k)
#ifdef _WIN32
        if (_stricmp(sg_extern_modnames[k], dll) == 0) {
#else
        if (strcasecmp(sg_extern_modnames[k], dll) == 0) {
#endif
            h = sg_extern_mods[k];
            break;
        }
    if (!h) {
#ifdef _WIN32
        h = LoadLibraryA(dll);
#else
        h = dlopen(dll, RTLD_NOW);
#endif
        if (!h) {
            char* msg = sg_io_msg("extern_call() 无法加载动态库: ", dll);
            sg_panic(fn, file, line, msg);
        }
        if (sg_extern_modn < 64) {
            sg_extern_mods[sg_extern_modn] = h;
            strncpy(sg_extern_modnames[sg_extern_modn], dll,
                    SG_EXTERN_MODNAME_MAX - 1);
            sg_extern_modnames[sg_extern_modn][SG_EXTERN_MODNAME_MAX - 1] = 0;
            ++sg_extern_modn;
        }
    }
#ifdef _WIN32
    void* p = (void*)GetProcAddress((HMODULE)h, sym);
#else
    void* p = dlsym(h, sym);
#endif
    if (!p) {
        char* m1 = sg_io_msg("extern_call() 符号未找到: ", dll);
        char* m2 = sg_io_msg(m1, "!");
        char* msg = sg_io_msg(m2, sym);
        free(m1);
        free(m2);
        sg_panic(fn, file, line, msg);
    }
    char* key = (char*)malloc(dl + sl + 2);
    if (!key) exit(1);
    sprintf(key, "%s!%s", dll, sym);
    sg_extern_cache[sg_extern_n].key = key;
    sg_extern_cache[sg_extern_n].fn = p;
    ++sg_extern_n;
    return p;
}

/* 外部返回的 char* 深拷贝（NULL → ""；外部串生命周期不归运行时管理）。 */
const char* sg_extern_wrap_str(const char* s) { return sg_dup(s ? s : ""); }
)NT";
