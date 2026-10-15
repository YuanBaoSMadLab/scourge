// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 字节码 VM — 网络 / 睡眠内建（F/G 专项）
// ----------------------------------------------------------------------------
//  职责边界：只实现 net_* 五内建与 sleep；doBuiltin 按 "net_" 前缀 / "sleep"
//  分派进来（VM.cpp 尾段一行路由，本文件是唯一实现处）。
//  语义契约（与 src/Tool/BuilderRTNet.hpp 的 sg_net_* / sg_sleep 逐字一致）：
//    * panic 消息逐字相同，双模差分由 verify 探针锁定；
//    * 句柄 = 1 起始的表下标（不透明 int，双模各自编号、行为一致）；
//    * net_recv 返回 str（原始字节，可能非 UTF-8——str 即字节串约定）；
//    * net_send 循环发送直到全部发出或出错（返回总字节数 = data 长度）；
//    * net_close 幂等拒绝：重复 close / 使用已 close 句柄均 panic；
//    * net_http_get 仅 http:// 明文，返回完整原始响应（状态行+头+体）；
//    * sleep(ms) ms<0 panic；winsock 懒初始化一次。
// ============================================================================
#include "scourge/BC/VM.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <string>
#include <vector>

namespace scourge {
namespace bc {

namespace {

/// 进程级 winsock 懒初始化（首次 net_* 调用时执行一次）。
bool wsInit() {
    static bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}

/// 解析 host（点分 IPv4 或可解析域名）→ in_addr；失败返回 false。
bool resolveHost(const std::string& host, in_addr& out) {
    if (inet_pton(AF_INET, host.c_str(), &out) == 1) return true;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
        return false;
    out = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

/// http://host[:port]/path 拆分（v1 仅明文 http；https 显式拒绝即文档）。
bool parseHttpUrl(const std::string& url, std::string& host, int& port,
                  std::string& path) {
    static const char kScheme[] = "http://";
    if (url.rfind(kScheme, 0) != 0) return false;
    std::string rest = url.substr(sizeof(kScheme) - 1);
    size_t slash = rest.find('/');
    std::string hostPort = slash == std::string::npos ? rest
                                                      : rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (hostPort.empty()) return false;
    // IPv6 字面量不在 v1 覆盖集（'[' 显式拒绝，报 URL 不合法）。
    if (hostPort[0] == '[') return false;
    size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        host = hostPort.substr(0, colon);
        std::string p = hostPort.substr(colon + 1);
        if (p.empty()) return false;
        port = 0;
        for (char c : p) {
            if (c < '0' || c > '9') return false;
            port = port * 10 + (c - '0');
            if (port > 65535) return false;
        }
        if (port == 0) return false;
    } else {
        host = hostPort;
        port = 80;
    }
    if (host.empty()) return false;
    return true;
}

} // namespace

void VM::netBuiltin(const std::string& n, std::vector<Value>& a) {
    auto need = [&](size_t want) {
        if (a.size() != want)
            panicHere("内建 " + n + "() 需要 " + std::to_string(want) +
                      " 个参数");
    };
    auto needInt = [&](size_t k, const char* what) -> int64_t {
        if (a[k].kind != Value::Kind::Int)
            panicHere(n + "() " + what + "必须是 int");
        return a[k].i;
    };
    auto needStr = [&](size_t k, const char* what) -> const std::string& {
        if (a[k].kind != Value::Kind::Str)
            panicHere(n + "() " + what + "必须是 str");
        return a[k].s;
    };
    // 句柄校验：1 起始、在表内、槽非空（close 后槽清零）。
    auto sockOf = [&](int64_t h) -> SOCKET {
        if (h < 1 || static_cast<size_t>(h) > netSocks_.size() ||
            netSocks_[static_cast<size_t>(h) - 1] == 0)
            panicHere(n + "() 无效的套接字句柄");
        return static_cast<SOCKET>(netSocks_[static_cast<size_t>(h) - 1]);
    };

    if (!wsInit()) panicHere("网络初始化失败（WSAStartup）");

    if (n == "net_connect") {
        need(2);
        const std::string& host = needStr(0, "主机参数");
        int64_t port = needInt(1, "端口参数");
        if (port < 1 || port > 65535)
            panicHere("net_connect() 端口超出 1..65535");
        in_addr addr;
        if (!resolveHost(host, addr))
            panicHere("net_connect() 无法解析主机: " + host);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) panicHere("net_connect() 创建套接字失败");
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<u_short>(port));
        sa.sin_addr = addr;
        if (connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
            closesocket(s);
            panicHere("net_connect() 连接失败: " + host + ":" +
                      std::to_string(port));
        }
        // 句柄分配：优先复用空槽，否则追加。
        size_t idx = netSocks_.size();
        for (size_t k = 0; k < netSocks_.size(); ++k)
            if (netSocks_[k] == 0) { idx = k; break; }
        if (idx == netSocks_.size())
            netSocks_.push_back(static_cast<uintptr_t>(s));
        else
            netSocks_[idx] = static_cast<uintptr_t>(s);
        push(Value::makeInt(static_cast<int64_t>(idx) + 1));
        return;
    }
    if (n == "net_send") {
        need(2);
        SOCKET s = sockOf(needInt(0, "句柄参数"));
        const std::string& data = needStr(1, "数据参数");
        size_t sent = 0;
        while (sent < data.size()) {
            int chunk = static_cast<int>(
                (data.size() - sent > 0x40000000) ? 0x40000000
                                                  : data.size() - sent);
            int r = send(s, data.data() + sent, chunk, 0);
            if (r == SOCKET_ERROR)
                panicHere("net_send() 发送失败（连接已断开？）");
            sent += static_cast<size_t>(r);
        }
        push(Value::makeInt(static_cast<int64_t>(sent)));
        return;
    }
    if (n == "net_recv") {
        need(2);
        SOCKET s = sockOf(needInt(0, "句柄参数"));
        int64_t maxb = needInt(1, "上限参数");
        if (maxb < 1 || maxb > (1 << 20))
            panicHere("net_recv() 上限必须是 1..1048576 字节");
        std::string buf(static_cast<size_t>(maxb), '\0');
        int r = recv(s, &buf[0], static_cast<int>(maxb), 0);
        if (r == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAETIMEDOUT)
                panicHere("net_recv() 接收超时");
            panicHere("net_recv() 接收失败（连接已断开？）");
        }
        if (r == 0) { // 对端正常关闭 → 空串（与 AOT 契约一致）
            push(Value::makeStr(""));
            return;
        }
        buf.resize(static_cast<size_t>(r));
        push(Value::makeStr(std::move(buf)));
        return;
    }
    if (n == "net_close") {
        need(1);
        int64_t h = needInt(0, "句柄参数");
        SOCKET s = sockOf(h);
        closesocket(s);
        netSocks_[static_cast<size_t>(h) - 1] = 0;
        push(Value::makeNone());
        return;
    }
    if (n == "net_bind") {
        need(2);
        const std::string& host = needStr(0, "主机参数");
        int64_t port = needInt(1, "端口参数");
        if (port < 1 || port > 65535)
            panicHere("net_bind() 端口超出 1..65535");
        // 绑定地址："0.0.0.0" 表示任意网卡，其余解析为 IPv4。
        in_addr addr;
        if (host != "0.0.0.0" && !resolveHost(host, addr))
            panicHere("net_bind() 无法解析主机: " + host);
        else if (host == "0.0.0.0")
            addr.s_addr = INADDR_ANY;
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) panicHere("net_bind() 创建套接字失败");
        // Windows 语义：SO_REUSEADDR 允许劫持已绑定端口（双绑），必须改用
        // 独占绑定，端口冲突按 WSAEADDRINUSE 显式 panic（与 AOT 一致）。
        BOOL excl = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl,
                   sizeof(excl));
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<u_short>(port));
        sa.sin_addr = addr;
        if (bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 ||
            listen(s, 16) != 0) {
            int err = WSAGetLastError();
            closesocket(s);
            if (err == WSAEADDRINUSE)
                panicHere("net_bind() 端口已被占用: " + std::to_string(port));
            panicHere("net_bind() 绑定/监听失败: " + host + ":" +
                      std::to_string(port));
        }
        size_t idx = netSocks_.size();
        for (size_t k = 0; k < netSocks_.size(); ++k)
            if (netSocks_[k] == 0) { idx = k; break; }
        if (idx == netSocks_.size())
            netSocks_.push_back(static_cast<uintptr_t>(s));
        else
            netSocks_[idx] = static_cast<uintptr_t>(s);
        push(Value::makeInt(static_cast<int64_t>(idx) + 1));
        return;
    }
    if (n == "net_accept") {
        need(1);
        SOCKET l = sockOf(needInt(0, "句柄参数"));
        SOCKET s = accept(l, nullptr, nullptr);
        if (s == INVALID_SOCKET)
            panicHere("net_accept() 接受连接失败");
        size_t idx = netSocks_.size();
        for (size_t k = 0; k < netSocks_.size(); ++k)
            if (netSocks_[k] == 0) { idx = k; break; }
        if (idx == netSocks_.size())
            netSocks_.push_back(static_cast<uintptr_t>(s));
        else
            netSocks_[idx] = static_cast<uintptr_t>(s);
        push(Value::makeInt(static_cast<int64_t>(idx) + 1));
        return;
    }
    if (n == "net_set_timeout") {
        need(2);
        SOCKET s = sockOf(needInt(0, "句柄参数"));
        int64_t ms = needInt(1, "毫秒参数");
        if (ms <= 0)
            panicHere("net_set_timeout() 毫秒数必须为正（0 语义平台分歧，"
                      "不支持）");
        DWORD t = static_cast<DWORD>(ms);
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t,
                       sizeof(t)) != 0 ||
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t,
                       sizeof(t)) != 0)
            panicHere("net_set_timeout() 设置失败");
        push(Value::makeNone());
        return;
    }
    if (n == "net_udp_send") {
        need(3);
        const std::string& host = needStr(0, "主机参数");
        int64_t port = needInt(1, "端口参数");
        const std::string& data = needStr(2, "数据参数");
        if (port < 1 || port > 65535)
            panicHere("net_udp_send() 端口超出 1..65535");
        if (data.size() > 65507)
            panicHere("net_udp_send() 数据报超过 65507 字节上限");
        in_addr addr;
        if (!resolveHost(host, addr))
            panicHere("net_udp_send() 无法解析主机: " + host);
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) panicHere("net_udp_send() 创建套接字失败");
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<u_short>(port));
        sa.sin_addr = addr;
        int r = sendto(s, data.data(), static_cast<int>(data.size()), 0,
                       reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
        closesocket(s); // 一次性数据报：发送即关，无句柄暴露
        if (r == SOCKET_ERROR)
            panicHere("net_udp_send() 发送失败");
        push(Value::makeInt(static_cast<int64_t>(r)));
        return;
    }
    if (n == "net_http_get") {
        need(1);
        const std::string& url = needStr(0, "URL参数");
        std::string host, path;
        int port = 80;
        if (!parseHttpUrl(url, host, port, path))
            panicHere("net_http_get() 仅支持 http://host[:port]/path 形式的"
                      "URL: " + url);
        in_addr addr;
        if (!resolveHost(host, addr))
            panicHere("net_http_get() 无法解析主机: " + host);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) panicHere("net_http_get() 创建套接字失败");
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<u_short>(port));
        sa.sin_addr = addr;
        if (connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
            closesocket(s);
            panicHere("net_http_get() 连接失败: " + host + ":" +
                      std::to_string(port));
        }
        std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host +
                          "\r\nUser-Agent: scourge/0.9.4\r\nAccept: */*\r\n"
                          "Connection: close\r\n\r\n";
        size_t sent = 0;
        while (sent < req.size()) {
            int r = send(s, req.data() + sent,
                         static_cast<int>(req.size() - sent), 0);
            if (r == SOCKET_ERROR) {
                closesocket(s);
                panicHere("net_http_get() 请求发送失败");
            }
            sent += static_cast<size_t>(r);
        }
        std::string resp;
        char buf[8192];
        for (;;) {
            int r = recv(s, buf, sizeof(buf), 0);
            if (r == 0) break;
            if (r == SOCKET_ERROR) {
                closesocket(s);
                panicHere("net_http_get() 响应接收失败");
            }
            resp.append(buf, static_cast<size_t>(r));
            if (resp.size() > (1u << 26)) { // 64 MiB 上限防失控
                closesocket(s);
                panicHere("net_http_get() 响应超过 64MiB 上限");
            }
        }
        closesocket(s);
        push(Value::makeStr(std::move(resp)));
        return;
    }
    if (n == "sleep") {
        need(1);
        int64_t ms = needInt(0, "毫秒参数");
        if (ms < 0) panicHere("sleep() 毫秒数不能为负");
        Sleep(static_cast<DWORD>(ms));
        push(Value::makeNone());
        return;
    }
    panicHere("VM 内部错误：未知网络内建 '" + n + "'");
}

} // namespace bc
} // namespace scourge
