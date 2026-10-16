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
//    * sleep(ms) ms<0 panic；winsock 懒初始化一次（POSIX 占位 fd 0）。
//  跨平台（0.9.4）：POSIX 兼容层单一实现双平台，语义差异逐处注释。
// ============================================================================
#include "scourge/BC/VM.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
// POSIX 兼容层（跨平台支持 0.9.4）：Windows 套接字 API 名 → POSIX 映射，
// 单一实现双平台编译；语义差异与 AOT BuilderRTNet.hpp 逐处一致。
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

using SOCKET = int;
constexpr SOCKET kInvalidSocket = -1;
constexpr int kSocketError = -1;
#define INVALID_SOCKET kInvalidSocket
#define SOCKET_ERROR kSocketError
inline void closesocket(int s) { ::close(s); }
inline int WSAGetLastError() { return errno; }
// 阻塞套接字 recv 超时 = EAGAIN（对齐 AOT 的映射）。
constexpr int WSAETIMEDOUT = EAGAIN;
constexpr int WSAEADDRINUSE = EADDRINUSE;
// Windows 的 SO_REUSEADDR 允许劫持在用端口，须独占绑定；POSIX 的
// SO_REUSEADDR 仅影响 TIME_WAIT 复用——独占语义天然成立。
#define SO_EXCLUSIVEADDRUSE SO_REUSEADDR
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace scourge {
namespace bc {

namespace {

/// 进程级 winsock 懒初始化（首次 net_* 调用时执行一次；POSIX 无 WSA，
/// 仅占位 fd 0——POSIX 上 stdin 关闭时 socket() 可返回 0，与"0 = 空槽"
/// 哨兵冲突，先占住保证句柄表判断式双平台一致）。
bool wsInit() {
    static bool ok = [] {
#ifdef _WIN32
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
#else
        while (fcntl(0, F_GETFD) == -1 && errno == EBADF) {
            if (open("/dev/null", O_RDONLY) == -1) break;
        }
        return true;
#endif
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
        sa.sin_port = htons(static_cast<uint16_t>(port));
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
        // 独占绑定，端口冲突按 WSAEADDRINUSE 显式 panic（与 AOT 一致）；
        // POSIX 的 SO_REUSEADDR 仅 TIME_WAIT 复用，独占语义天然成立。
#ifdef _WIN32
        BOOL excl = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl,
                   sizeof(excl));
#else
        int reuse = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<uint16_t>(port));
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
#ifdef _WIN32
        DWORD t = static_cast<DWORD>(ms);
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t,
                       sizeof(t)) != 0 ||
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t,
                       sizeof(t)) != 0)
#else
        struct timeval tv; // POSIX: 超时选项取 struct timeval（秒+微秒）
        tv.tv_sec = static_cast<time_t>(ms / 1000);
        tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0)
#endif
            panicHere("net_set_timeout() 设置失败");
        push(Value::makeNone());
        return;
    }
    // ---- 0.9.8 HTTP 服务端：读请求 → Json / 写响应 → 发送字节数 ----
    // 解析语义与 AOT BuilderRTHttp.hpp 逐字一致：请求行三段空格分隔、
    // header 名小写化、body 按 Content-Length 收满；EOF 零字节 → Json
    // null（keep-alive 退出条件），中途断开 → panic。
    if (n == "net_http_read_request") {
        need(1);
        SOCKET s = sockOf(needInt(0, "句柄参数"));
        const size_t kMaxHdr = 65536, kMaxBody = 16777216;
        std::string buf;
        size_t hdrEnd = std::string::npos;
        char tmp[4096];
        while (hdrEnd == std::string::npos) {
            if (buf.size() > kMaxHdr)
                panicHere("net_http_read_request() 请求头超过 65536 字节上限");
            int r = recv(s, tmp, static_cast<int>(sizeof(tmp)), 0);
            if (r == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAETIMEDOUT)
                    panicHere("net_http_read_request() 接收超时");
                panicHere("net_http_read_request() 接收失败（连接已断开？）");
            }
            if (r == 0) {
                if (buf.empty()) { // 对端关闭且零字节：keep-alive 正常退出
                    push(Value::makeJsonNull());
                    return;
                }
                panicHere("net_http_read_request() 连接在请求中途关闭");
            }
            buf.append(tmp, static_cast<size_t>(r));
            hdrEnd = buf.find("\r\n\r\n");
        }
        // 请求行：METHOD SP PATH SP VERSION（恰好两个空格）
        size_t eol = buf.find("\r\n");
        std::string reqLine = buf.substr(0, eol);
        size_t sp1 = reqLine.find(' ');
        size_t sp2 = (sp1 == std::string::npos) ? std::string::npos
                                                : reqLine.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos ||
            reqLine.find(' ', sp2 + 1) != std::string::npos || sp2 == sp1 + 1 ||
            sp2 + 1 == reqLine.size())
            panicHere("net_http_read_request() 请求行格式非法");
        auto jstr = [](std::string v) {
            Value j;
            j.kind = Value::Kind::Json;
            j.json = std::make_shared<JsonData>();
            j.json->jk = JsonData::JK::Str;
            j.json->s = std::move(v);
            return j;
        };
        Value out;
        out.kind = Value::Kind::Json;
        out.json = std::make_shared<JsonData>();
        out.json->jk = JsonData::JK::Obj;
        out.json->obj = std::make_shared<DictData>();
        out.json->obj->set(Value::makeStr("method"),
                           jstr(reqLine.substr(0, sp1)));
        out.json->obj->set(Value::makeStr("path"),
                           jstr(reqLine.substr(sp1 + 1, sp2 - sp1 - 1)));
        out.json->obj->set(Value::makeStr("version"),
                           jstr(reqLine.substr(sp2 + 1)));
        Value hdrs;
        hdrs.kind = Value::Kind::Json;
        hdrs.json = std::make_shared<JsonData>();
        hdrs.json->jk = JsonData::JK::Obj;
        hdrs.json->obj = std::make_shared<DictData>();
        // header 区逐行 "Name: Value"；名字规范化小写（HTTP 语义大小写
        // 不敏感，小写化让语言层 get("host") 无需猜测大小写）
        size_t pos = eol + 2;
        int64_t contentLen = -1;
        while (pos < hdrEnd) {
            size_t e = buf.find("\r\n", pos);
            if (e == std::string::npos || e > hdrEnd) break;
            std::string line = buf.substr(pos, e - pos);
            pos = e + 2;
            size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0)
                panicHere("net_http_read_request() 请求头行格式非法");
            std::string name = line.substr(0, colon);
            for (char& c : name)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            size_t vb = colon + 1;
            while (vb < line.size() && (line[vb] == ' ' || line[vb] == '\t'))
                ++vb;
            size_t ve = line.size();
            while (ve > vb && (line[ve - 1] == ' ' || line[ve - 1] == '\t'))
                --ve;
            bool isLen = (name == "content-length") && contentLen < 0;
            hdrs.json->obj->set(Value::makeStr(std::move(name)),
                                jstr(line.substr(vb, ve - vb)));
            if (isLen)
                contentLen = strtoll(line.c_str() + colon + 1, nullptr, 10);
        }
        out.json->obj->set(Value::makeStr("headers"), std::move(hdrs));
        // body：按 Content-Length 收满（16MB 护栏）
        std::string body;
        if (contentLen > 0) {
            if (contentLen > static_cast<int64_t>(kMaxBody))
                panicHere(
                    "net_http_read_request() 请求体超过 16777216 字节上限");
            body = buf.substr(hdrEnd + 4);
            while (static_cast<int64_t>(body.size()) < contentLen) {
                int r = recv(s, tmp, static_cast<int>(sizeof(tmp)), 0);
                if (r == SOCKET_ERROR) {
                    if (WSAGetLastError() == WSAETIMEDOUT)
                        panicHere("net_http_read_request() 接收超时");
                    panicHere("net_http_read_request() 接收失败（连接已断开？）");
                }
                if (r == 0)
                    panicHere("net_http_read_request() 连接在请求中途关闭");
                body.append(tmp, static_cast<size_t>(r));
            }
            body.resize(static_cast<size_t>(contentLen));
        }
        out.json->obj->set(Value::makeStr("body"), jstr(std::move(body)));
        push(std::move(out));
        return;
    }
    if (n == "net_http_write_response") {
        need(4);
        SOCKET s = sockOf(needInt(0, "句柄参数"));
        int64_t status = needInt(1, "状态码参数");
        const std::string& ctype = needStr(2, "内容类型参数");
        const std::string& body = needStr(3, "响应体参数");
        if (status < 100 || status > 599)
            panicHere("net_http_write_response() 状态码超出 100..599");
        static const struct { int code; const char* reason; } kReasons[] = {
            {100, "Continue"}, {101, "Switching Protocols"},
            {200, "OK"}, {201, "Created"}, {202, "Accepted"},
            {204, "No Content"}, {301, "Moved Permanently"}, {302, "Found"},
            {304, "Not Modified"}, {400, "Bad Request"},
            {401, "Unauthorized"}, {403, "Forbidden"}, {404, "Not Found"},
            {405, "Method Not Allowed"}, {409, "Conflict"},
            {415, "Unsupported Media Type"}, {429, "Too Many Requests"},
            {500, "Internal Server Error"}, {501, "Not Implemented"},
            {502, "Bad Gateway"}, {503, "Service Unavailable"},
            {504, "Gateway Timeout"},
        };
        const char* reason = "Unknown";
        for (const auto& k : kReasons)
            if (k.code == status) { reason = k.reason; break; }
        // One Obvious Way：固定 Connection: close（阻塞单线程模型下
        // keep-alive 语义复杂，语言层每请求 accept 一连接）
        std::string resp = "HTTP/1.1 " + std::to_string(status) + " " +
                           reason + "\r\nContent-Type: " + ctype +
                           "\r\nContent-Length: " +
                           std::to_string(body.size()) +
                           "\r\nConnection: close\r\n\r\n";
        resp += body;
        size_t sent = 0;
        while (sent < resp.size()) {
            int chunk = static_cast<int>(
                (resp.size() - sent > 0x40000000) ? 0x40000000
                                                  : resp.size() - sent);
            int r = send(s, resp.data() + sent, chunk, 0);
            if (r == SOCKET_ERROR)
                panicHere("net_http_write_response() 发送失败（连接已断开？）");
            sent += static_cast<size_t>(r);
        }
        push(Value::makeInt(static_cast<int64_t>(sent)));
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
        sa.sin_port = htons(static_cast<uint16_t>(port));
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
        sa.sin_port = htons(static_cast<uint16_t>(port));
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
#ifdef _WIN32
        Sleep(static_cast<DWORD>(ms));
#else
        struct timespec ts;
        ts.tv_sec = static_cast<time_t>(ms / 1000);
        ts.tv_nsec = static_cast<long>((ms % 1000) * 1000000L);
        nanosleep(&ts, nullptr);
#endif
        push(Value::makeNone());
        return;
    }
    panicHere("VM 内部错误：未知网络内建 '" + n + "'");
}

} // namespace bc
} // namespace scourge
