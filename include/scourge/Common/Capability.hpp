// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 能力模型 — 域表单一事实源（0.9.7）
// ----------------------------------------------------------------------------
//  安全模型：默认零能力。内建按"是否触达程序外世界"二分——纯计算内建
//  （数学/哈希/容器/JSON）无需授权；环境交互内建（IO/网络/环境/进程/时钟/
//  信号/FFI）必须经模块顶层 `capability <域, ...>` 显式声明，否则检查期
//  E5004 拒绝。运行时另有第二道门禁（VM doBuiltin / AOT sg_* 入口查位），
//  防绕过检查器的执行路径（纵深防御，双模 panic 消息逐字一致）。
//
//  为什么 sha256/random 不在表内：两者是确定性纯函数（splitmix64 内定种子、
//  SHA-256 无密钥无熵源）——可复现性是本语言的安全卖点，授权它们反而破坏
//  "能力 = 环境交互"的单义性。
//
//  为什么 print 不在表内：stdout 是诊断通道而非环境资源（WASI 同款约定）；
//  若 print 需授权，Hello World 也要能力，摩擦大于收益。
//
//  本表是唯一权威：TypeChecker（静态门禁）、VM doBuiltin（运行时门禁）、
//  生成 _rt.c 的 SG_CAP 宏注入（AOT 运行时门禁）均查此表，禁止平行名单。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "scourge/Syntax/AST.hpp"

namespace scourge {

// ---- 能力域（位掩码即 Program.capMask / SG_CAPS 布局，序列化按位存取） ----
enum class Cap : uint8_t {
    Io = 1 << 0,     ///< 文件/目录/stdin：read_text write_text append_text
                     ///<      list_dir exists input
    Net = 1 << 1,    ///< TCP/UDP/HTTP：net_connect net_send net_recv net_close
                     ///<      net_udp_send net_http_get net_bind net_accept
                     ///<      net_set_timeout
    Env = 1 << 2,    ///< 参数与环境变量：args env has_env set_env unset_env
    Proc = 1 << 3,   ///< 进程：system spawn_wait
    Time = 1 << 4,   ///< 挂钟：now epoch epoch_ms localtime gmtime sleep
    Signal = 1 << 5, ///< 控制台信号标志：signal_caught signal_reset
    Ffi = 1 << 6,    ///< 外部代码：@extern("dll") 与 data_ptr（数据地址导出）
};

/// 域 → 掩码位。枚举值本身即位掩码（Ffi=1<<6=64），此处恒等透传——
/// 若再按"索引"左移（1 << 64）会回绕成 1，ffi 门禁将错认成 io 位。
constexpr uint64_t capBit(Cap c) { return uint64_t(c); }

/// 域的规范名（capability 声明与 panic 消息共用，双模逐字一致）。
constexpr std::string_view capName(Cap c) {
    switch (c) {
    case Cap::Io: return "io";
    case Cap::Net: return "net";
    case Cap::Env: return "env";
    case Cap::Proc: return "proc";
    case Cap::Time: return "time";
    case Cap::Signal: return "signal";
    case Cap::Ffi: return "ffi";
    }
    return "";
}

/// 全域清单（capFromName 与 Builder 生成 rt.c 门禁表共用，禁止平行名单）。
inline constexpr Cap kAllCaps[] = {Cap::Io,    Cap::Net,   Cap::Env,
                                   Cap::Proc,  Cap::Time,  Cap::Signal,
                                   Cap::Ffi};

/// 解析声明中的域名；失败返回 false（Parser 报 E1005）。
inline bool capFromName(std::string_view n, Cap& out) {
    for (Cap c : kAllCaps)
        if (capName(c) == n) { out = c; return true; }
    return false;
}

/// 需授权内建 → 所属域。纯计算内建不在此表（查不到 = 零能力可用）。
inline bool capOfBuiltin(std::string_view n, Cap& out) {
    // io
    if (n == "input" || n == "read_text" || n == "write_text" ||
        n == "append_text" || n == "exists" || n == "list_dir") {
        out = Cap::Io; return true;
    }
    // net
    if (n == "net_connect" || n == "net_send" || n == "net_recv" ||
        n == "net_close" || n == "net_udp_send" || n == "net_http_get" ||
        n == "net_bind" || n == "net_accept" || n == "net_set_timeout") {
        out = Cap::Net; return true;
    }
    // env
    if (n == "args" || n == "env" || n == "has_env" || n == "set_env" ||
        n == "unset_env") {
        out = Cap::Env; return true;
    }
    // proc
    if (n == "system" || n == "spawn_wait") { out = Cap::Proc; return true; }
    // time
    if (n == "now" || n == "epoch" || n == "epoch_ms" || n == "localtime" ||
        n == "gmtime" || n == "sleep") {
        out = Cap::Time; return true;
    }
    // signal
    if (n == "signal_caught" || n == "signal_reset") {
        out = Cap::Signal; return true;
    }
    // ffi（内建名域；@extern 装饰器不经此查，直接用 Cap::Ffi）
    if (n == "data_ptr") { out = Cap::Ffi; return true; }
    return false;
}

/// 运行时/AOT panic 消息（VM panicHere 与 sg_cap_check 逐字共用）：
///   capability 'io' not granted (read_text)
inline std::string capPanicMsg(Cap c, std::string_view what) {
    std::string s = "capability '";
    s += capName(c);
    s += "' not granted (";
    s += what;
    s += ")";
    return s;
}

/// 单模块顶层 capability 声明集。会话层（Runner/Repl/TestRunner/Builder）
/// 在 compileModule 循环外按依赖序对全部模块求并——闭包并集即
/// Program.capMask / SG_CAPS。Compiler 层不参与（字节码后端不感知声明）。
inline uint64_t capMaskOf(const Module& m) {
    uint64_t mask = 0;
    for (const auto& s : m.body) {
        if (s->kind != Stmt::Kind::Capability) continue;
        for (const auto& it :
             static_cast<const Capability&>(*s).caps) {
            Cap cap;
            if (capFromName(it.name, cap)) mask |= capBit(cap);
        }
    }
    return mask;
}

} // namespace scourge
