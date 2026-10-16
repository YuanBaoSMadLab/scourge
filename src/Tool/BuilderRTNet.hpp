// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  AOT C 运行时片段 — 网络/睡眠内建（0.9.4 F/G 专项）
// ----------------------------------------------------------------------------
//  职责边界：与 BuilderRTStr.hpp 同模式——build 时拼接在 kRuntimeC 之后落盘
//  （复用其 sg_panic/sg_io_msg/windows.h 定义），缓存键已计入本片段文本。
//  语义契约（与 src/BC/VMNet.cpp 逐字一致，双模差分由 verify 探针锁定）：
//    * 句柄 = 1 起始的表下标（静态表，close 置 0 空槽，可复用）；
//    * net_send 循环发送直到全部发出；net_recv 对端关闭返回 ""；
//    * net_close / 使用已 close 句柄 / 非法句柄均 panic；
//    * net_http_get 仅 http://（IPv6 字面量显式拒绝），返回完整原始响应，
//      64MiB 上限；net_udp_send 单数据报发送即关（无句柄暴露）；
//    * winsock 懒初始化一次（POSIX 无操作，仅占位 fd 0）；返回串 malloc；
//    * POSIX 兼容层单一实现双平台（Windows 语义为准，差异逐处注释）。
//  本文件保持 ASCII-only 注释规范之外仅常量字符串含中文（panic 消息对齐
//  VM），与 kRuntimeC 内嵌源同处理（cl /utf-8 由 Builder 编译参数保证）。
// ============================================================================
#pragma once

inline const char kNetRuntimeC[] = R"NT(/* Scourge AOT 运行时 — 网络/睡眠片段（F/G 专项；语义对齐 VMNet.cpp）。 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
/* POSIX（Linux/macOS 与 BSD）兼容层：把 Windows 套接字 API 名映射到 POSIX，
 * 单一实现双平台编译——语义差异逐处注释（跨平台支持 0.9.4）。
 * fd 0 边界：POSIX 上 stdin 关闭时 socket() 可返回 0，sg_net_wsinit 会
 * 占住 fd 0 保证 0 恒为空槽哨兵（与 Windows 同一判断式）。 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket(s) close(s)
static int WSAGetLastError(void) { return errno; }
/* 阻塞套接字上 recv 超时返回 EAGAIN/EWOULDBLOCK（=11），映射对齐语义。 */
#define WSAETIMEDOUT EAGAIN
#define WSAEADDRINUSE EADDRINUSE
/* Windows 的 SO_REUSEADDR 允许劫持在用端口（历史行为）故须独占绑定；
 * POSIX 的 SO_REUSEADDR 仅影响 TIME_WAIT 复用，绑定占用端口同样报
 * EADDRINUSE——独占语义天然成立。 */
#define SO_EXCLUSIVEADDRUSE SO_REUSEADDR
#endif

#define SG_NET_MAX_SOCKS 1024
#define SG_NET_EMPTY 0 /* 空槽哨兵（POSIX 由 wsinit 占住 fd 0 保证恒不冲突） */

static SOCKET sg_net_table[SG_NET_MAX_SOCKS]; /* SG_NET_EMPTY = 空槽（回收复用） */

static int sg_net_wsinit(void) {
    static int done = 0;
    if (!done) {
#ifdef _WIN32
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0)
            return 0;
#else
        /* 占住 fd 0（若 stdin 已关），保证 socket() 永不返回空槽哨兵 0。 */
        while (fcntl(0, F_GETFD) == -1 && errno == EBADF)
            if (open("/dev/null", O_RDONLY) == -1)
                break;
#endif
        done = 1;
    }
    return 1;
}

/* host（点分 IPv4 或域名）→ in_addr；失败返回 0。 */
static int sg_net_resolve(const char* host, struct in_addr* out) {
    if (inet_pton(AF_INET, host, out) == 1) return 1;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;
    *out = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 1;
}

/* 句柄校验：1 起始、在表内、槽非空；失败 panic（消息对齐 VM）。 */
static SOCKET sg_net_sockof(int64_t h, const char* fn, const char* file,
                            int64_t line, const char* call) {
    if (h < 1 || h > SG_NET_MAX_SOCKS ||
        sg_net_table[h - 1] == SG_NET_EMPTY)
        sg_panic(fn, file, line,
                 sg_io_msg(call, "() 无效的套接字句柄"));
    return sg_net_table[h - 1];
}

/* "host:port" 拼接（panic 消息用）。 */
static char* sg_net_hostport(const char* host, int64_t port) {
    char* out = (char*)malloc(strlen(host) + 24);
    if (!out) exit(1);
    sprintf(out, "%s:%lld", host, (long long)port);
    return out;
}

int64_t sg_net_connect(const char* host, int64_t port, const char* fn,
                       const char* file, int64_t line) {
    if (!sg_net_wsinit()) sg_panic(fn, file, line, "网络初始化失败（WSAStartup）");
    if (port < 1 || port > 65535)
        sg_panic(fn, file, line, "net_connect() 端口超出 1..65535");
    struct in_addr addr;
    if (!sg_net_resolve(host, &addr))
        sg_panic(fn, file, line,
                 sg_io_msg("net_connect() 无法解析主机: ", host));
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) sg_panic(fn, file, line, "net_connect() 创建套接字失败");
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr = addr;
    if (connect(s, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        char* hp = sg_net_hostport(host, port);
        char* msg = sg_io_msg("net_connect() 连接失败: ", hp);
        free(hp);
        closesocket(s);
        sg_panic(fn, file, line, msg);
    }
    for (int64_t k = 0; k < SG_NET_MAX_SOCKS; ++k) {
        if (sg_net_table[k] == SG_NET_EMPTY) {
            sg_net_table[k] = s;
            return k + 1;
        }
    }
    closesocket(s);
    sg_panic(fn, file, line, "net_connect() 句柄表已满（1024）");
    return 0;
}

int64_t sg_net_send(int64_t h, const char* data, const char* fn,
                    const char* file, int64_t line) {
    SOCKET s = sg_net_sockof(h, fn, file, line, "net_send");
    size_t len = strlen(data), sent = 0;
    while (sent < len) {
        int chunk = (len - sent > 0x40000000) ? 0x40000000 : (int)(len - sent);
        int r = send(s, data + sent, chunk, 0);
        if (r == SOCKET_ERROR)
            sg_panic(fn, file, line, "net_send() 发送失败（连接已断开？）");
        sent += (size_t)r;
    }
    return (int64_t)sent;
}

const char* sg_net_recv(int64_t h, int64_t maxb, const char* fn,
                        const char* file, int64_t line) {
    SOCKET s = sg_net_sockof(h, fn, file, line, "net_recv");
    if (maxb < 1 || maxb > (1 << 20))
        sg_panic(fn, file, line, "net_recv() 上限必须是 1..1048576 字节");
    char* buf = (char*)malloc((size_t)maxb + 1);
    if (!buf) exit(1);
    int r = recv(s, buf, (int)maxb, 0);
    if (r == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAETIMEDOUT)
            sg_panic(fn, file, line, "net_recv() 接收超时");
        sg_panic(fn, file, line, "net_recv() 接收失败（连接已断开？）");
    }
    buf[r] = '\0'; /* 对端关闭 r=0 → 空串（与 VM 契约一致） */
    return buf;
}

void sg_net_close(int64_t h, const char* fn, const char* file, int64_t line) {
    SOCKET s = sg_net_sockof(h, fn, file, line, "net_close");
    closesocket(s);
    sg_net_table[h - 1] = SG_NET_EMPTY;
}

int64_t sg_net_udp_send(const char* host, int64_t port, const char* data,
                        const char* fn, const char* file, int64_t line) {
    if (!sg_net_wsinit()) sg_panic(fn, file, line, "网络初始化失败（WSAStartup）");
    if (port < 1 || port > 65535)
        sg_panic(fn, file, line, "net_udp_send() 端口超出 1..65535");
    size_t dl = strlen(data);
    if (dl > 65507)
        sg_panic(fn, file, line, "net_udp_send() 数据报超过 65507 字节上限");
    struct in_addr addr;
    if (!sg_net_resolve(host, &addr))
        sg_panic(fn, file, line,
                 sg_io_msg("net_udp_send() 无法解析主机: ", host));
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) sg_panic(fn, file, line, "net_udp_send() 创建套接字失败");
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr = addr;
    int r = sendto(s, data, (int)dl, 0, (struct sockaddr*)&sa, sizeof(sa));
    closesocket(s); /* 一次性数据报：发送即关，无句柄暴露 */
    if (r == SOCKET_ERROR) sg_panic(fn, file, line, "net_udp_send() 发送失败");
    return (int64_t)r;
}

/* http://host[:port]/path 拆分；非法返回 0（消息对齐 VM）。 */
static int sg_net_parse_url(const char* url, char** host, int* port,
                            const char** path) {
    static const char sch[] = "http://";
    if (strncmp(url, sch, sizeof(sch) - 1) != 0) return 0;
    const char* rest = url + sizeof(sch) - 1;
    const char* slash = strchr(rest, '/');
    const char* hpEnd = slash ? slash : rest + strlen(rest);
    *path = slash ? slash : "/";
    size_t hpl = (size_t)(hpEnd - rest);
    if (hpl == 0 || rest[0] == '[') return 0; /* IPv6 字面量不在 v1 覆盖集 */
    char* hpb = (char*)malloc(hpl + 1);
    if (!hpb) exit(1);
    memcpy(hpb, rest, hpl);
    hpb[hpl] = '\0';
    char* colon = strrchr(hpb, ':');
    if (colon) {
        *colon = '\0';
        long p = 0;
        if (!*++colon) { free(hpb); return 0; }
        for (const char* q = colon; *q; ++q) {
            if (*q < '0' || *q > '9') { free(hpb); return 0; }
            p = p * 10 + (*q - '0');
            if (p > 65535) { free(hpb); return 0; }
        }
        if (p == 0) { free(hpb); return 0; }
        *port = (int)p;
    } else {
        *port = 80;
    }
    if (!*hpb) { free(hpb); return 0; }
    *host = hpb;
    return 1;
}

const char* sg_net_http_get(const char* url, const char* fn,
                            const char* file, int64_t line) {
    if (!sg_net_wsinit()) sg_panic(fn, file, line, "网络初始化失败（WSAStartup）");
    char* host = NULL;
    int port = 80;
    const char* path = NULL;
    if (!sg_net_parse_url(url, &host, &port, &path)) {
        char* msg = sg_io_msg(
            "net_http_get() 仅支持 http://host[:port]/path 形式的URL: ", url);
        sg_panic(fn, file, line, msg);
    }
    struct in_addr addr;
    if (!sg_net_resolve(host, &addr))
        sg_panic(fn, file, line,
                 sg_io_msg("net_http_get() 无法解析主机: ", host));
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) sg_panic(fn, file, line, "net_http_get() 创建套接字失败");
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr = addr;
    if (connect(s, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        char* hp = sg_net_hostport(host, port);
        char* msg = sg_io_msg("net_http_get() 连接失败: ", hp);
        free(hp);
        closesocket(s);
        sg_panic(fn, file, line, msg);
    }
    char* req = (char*)malloc(strlen(path) + strlen(host) + 128);
    if (!req) exit(1);
    sprintf(req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: scourge/0.9.4\r\n"
                 "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
    size_t reql = strlen(req), sent = 0;
    while (sent < reql) {
        int r = send(s, req + sent, (int)(reql - sent), 0);
        if (r == SOCKET_ERROR) {
            free(req);
            closesocket(s);
            sg_panic(fn, file, line, "net_http_get() 请求发送失败");
        }
        sent += (size_t)r;
    }
    free(req);
    size_t cap = 65536, len = 0;
    char* resp = (char*)malloc(cap);
    if (!resp) exit(1);
    for (;;) {
        char buf[8192];
        int r = recv(s, buf, sizeof(buf), 0);
        if (r == 0) break;
        if (r == SOCKET_ERROR) {
            free(resp);
            closesocket(s);
            sg_panic(fn, file, line, "net_http_get() 响应接收失败");
        }
        if (len + (size_t)r + 1 > cap) {
            while (len + (size_t)r + 1 > cap) cap *= 2;
            if (cap > (1u << 26)) {
                free(resp);
                closesocket(s);
                sg_panic(fn, file, line, "net_http_get() 响应超过 64MiB 上限");
            }
            char* nb = (char*)realloc(resp, cap);
            if (!nb) exit(1);
            resp = nb;
        }
        memcpy(resp + len, buf, (size_t)r);
        len += (size_t)r;
    }
    closesocket(s);
    resp[len] = '\0';
    return resp;
}

void sg_sleep(int64_t ms, const char* fn, const char* file, int64_t line) {
    if (ms < 0) sg_panic(fn, file, line, "sleep() 毫秒数不能为负");
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
#endif
}

/* ---- F/G 迭代 2：TCP 服务端与收发超时（语义对齐 VMNet.cpp） ---- */

int64_t sg_net_bind(const char* host, int64_t port, const char* fn,
                    const char* file, int64_t line) {
    if (!sg_net_wsinit()) sg_panic(fn, file, line, "网络初始化失败（WSAStartup）");
    if (port < 1 || port > 65535)
        sg_panic(fn, file, line, "net_bind() 端口超出 1..65535");
    struct in_addr addr;
    if (strcmp(host, "0.0.0.0") == 0) {
        addr.s_addr = INADDR_ANY;
    } else if (!sg_net_resolve(host, &addr)) {
        sg_panic(fn, file, line,
                 sg_io_msg("net_bind() 无法解析主机: ", host));
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) sg_panic(fn, file, line, "net_bind() 创建套接字失败");
#ifdef _WIN32
    { BOOL excl = TRUE;
      /* Windows: SO_REUSEADDR 允许劫持已绑定端口，须独占绑定（对齐 VM）。 */
      setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl)); }
#else
    { int reuse = 1;
      /* POSIX: 仅 TIME_WAIT 复用，不劫持活跃监听（语义对齐 VM 独占绑定）。 */
      setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)); }
#endif
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr = addr;
    if (bind(s, (struct sockaddr*)&sa, sizeof(sa)) != 0 ||
        listen(s, 16) != 0) {
        int err = WSAGetLastError();
        char* hp = sg_net_hostport(host, port);
        char* msg = sg_io_msg("net_bind() 绑定/监听失败: ", hp);
        free(hp);
        closesocket(s);
        if (err == WSAEADDRINUSE) {
            char pmsg[64];
            sprintf(pmsg, "net_bind() 端口已被占用: %lld", (long long)port);
            free(msg);
            sg_panic(fn, file, line, pmsg);
        }
        sg_panic(fn, file, line, msg);
    }
    for (int64_t k = 0; k < SG_NET_MAX_SOCKS; ++k) {
        if (sg_net_table[k] == SG_NET_EMPTY) {
            sg_net_table[k] = s;
            return k + 1;
        }
    }
    closesocket(s);
    sg_panic(fn, file, line, "net_bind() 句柄表已满（1024）");
    return 0;
}

int64_t sg_net_accept(int64_t h, const char* fn, const char* file,
                      int64_t line) {
    SOCKET l = sg_net_sockof(h, fn, file, line, "net_accept");
    SOCKET s = accept(l, NULL, NULL);
    if (s == INVALID_SOCKET)
        sg_panic(fn, file, line, "net_accept() 接受连接失败");
    for (int64_t k = 0; k < SG_NET_MAX_SOCKS; ++k) {
        if (sg_net_table[k] == SG_NET_EMPTY) {
            sg_net_table[k] = s;
            return k + 1;
        }
    }
    closesocket(s);
    sg_panic(fn, file, line, "net_accept() 句柄表已满（1024）");
    return 0;
}

void sg_net_set_timeout(int64_t h, int64_t ms, const char* fn,
                        const char* file, int64_t line) {
    SOCKET s = sg_net_sockof(h, fn, file, line, "net_set_timeout");
    if (ms <= 0)
        sg_panic(fn, file, line,
                 "net_set_timeout() 毫秒数必须为正（0 语义平台分歧，不支持）");
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t)) != 0 ||
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t)) != 0)
#else
    struct timeval tv; /* POSIX: 超时选项取 struct timeval（秒+微秒） */
    tv.tv_sec = (time_t)(ms / 1000);
    tv.tv_usec = (long)((ms % 1000) * 1000);
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
#endif
        sg_panic(fn, file, line, "net_set_timeout() 设置失败");
}
)NT";
