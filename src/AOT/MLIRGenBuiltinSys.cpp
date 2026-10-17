// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT MLIR 生成器 · 内建 IO/网络/系统域分派
// ----------------------------------------------------------------------------
//  职责边界：genBuiltinSysCall——input/文件 IO/网络/系统编程（0.9.4
//  F/G/H 专项 + 0.9.8 HTTP 服务端）内建的 lang 发射，自 MLIRGenBuiltin.cpp
//  按功能域拆出（文件分块纪律）。未命中返回 nullopt 交回主链；命中块
//  语义与拆分前逐字节不变。其余域见同目录 MLIRGen*.cpp。
// ============================================================================
#include "scourge/AOT/MLIRGenDetail.hpp"

#include <optional>

namespace scourge {
namespace aot {
namespace mlirgen {

std::optional<Value> Gen::genBuiltinSysCall(const std::string& name,
                                            const Call& c) {
    auto need = [&](size_t want) {
        if (c.args.size() != want) unsupported(c.range, "内建 " + name + "() 参数个数");
    };

    // ---- IO 内建（0.9.4）：input/read_text/write_text/append_text/exists/
    // list_dir。路径窄字节串；失败 panic 的内建挂 opLoc 上下文（exists 纯
    // 查询不 panic 不挂）；list_dir 元素静态记 Str（驱动 list_get_s/print）。
    if (name == "input") {
        std::string ssa = fresh();
        emit(ssa + " = \"lang.input\"() : () -> !llvm.ptr " + opLoc());
        return Value{VK::Str, ssa};
    }
    if (name == "read_text" || name == "list_dir") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, name + "() 路径参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang." + name + "\"(" + v.ssa +
             ") : (!llvm.ptr) -> !llvm.ptr " + opLoc());
        if (name == "read_text") return Value{VK::Str, ssa};
        return Value{VK::List, ssa, false, 0, static_cast<uint8_t>(VK::Str)};
    }
    if (name == "write_text" || name == "append_text") {
        need(2);
        Value p = genExpr(*c.args[0]);
        Value s = genExpr(*c.args[1]);
        if (p.vk != VK::Str)
            unsupported(c.args[0]->range, name + "() 路径参数必须是 str");
        if (s.vk != VK::Str)
            unsupported(c.args[1]->range, name + "() 内容参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang." + name + "\"(" + p.ssa + ", " + s.ssa +
             ") : (!llvm.ptr, !llvm.ptr) -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "exists") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, "exists() 路径参数必须是 str");
        std::string ssa = fresh();
        // 能力行：挂 opLoc——sg_cap_check 拒绝时 panic 上下文与 VM 一致
        emit(ssa + " = \"lang.exists\"(" + v.ssa + ") : (!llvm.ptr) -> i1 " +
             opLoc());
        return Value{VK::Bool, ssa};
    }
    // ---- 网络/睡眠内建（0.9.4 F/G 专项）：句柄不透明 I64；失败 panic 的
    // 内建挂 opLoc 上下文（与 sg_net_* 运行时消息逐字一致）；仅 http 明文。
    if (name == "net_connect") {
        need(2);
        Value h = genExpr(*c.args[0]);
        Value p = genExpr(*c.args[1]);
        if (h.vk != VK::Str)
            unsupported(c.args[0]->range, "net_connect() 主机参数必须是 str");
        if (p.vk != VK::Int)
            unsupported(c.args[1]->range, "net_connect() 端口参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.net_connect\"(" + h.ssa + ", " + p.ssa +
             ") : (!llvm.ptr, i64) -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "net_send" || name == "net_recv") {
        need(2);
        Value s = genExpr(*c.args[0]);
        Value d = genExpr(*c.args[1]);
        if (s.vk != VK::Int)
            unsupported(c.args[0]->range, name + "() 句柄参数必须是 int");
        if (name == "net_send" && d.vk != VK::Str)
            unsupported(c.args[1]->range, "net_send() 数据参数必须是 str");
        if (name == "net_recv" && d.vk != VK::Int)
            unsupported(c.args[1]->range, "net_recv() 上限参数必须是 int");
        std::string ssa = fresh();
        if (name == "net_send") {
            emit(ssa + " = \"lang.net_send\"(" + s.ssa + ", " + d.ssa +
                 ") : (i64, !llvm.ptr) -> i64 " + opLoc());
            return Value{VK::Int, ssa};
        }
        emit(ssa + " = \"lang.net_recv\"(" + s.ssa + ", " + d.ssa +
             ") : (i64, i64) -> !llvm.ptr " + opLoc());
        return Value{VK::Str, ssa};
    }
    if (name == "net_close" || name == "sleep") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Int)
            unsupported(c.args[0]->range,
                        name + "() 参数必须是 int");
        emit("\"lang." + name + "\"(" + v.ssa + ") : (i64) -> () " +
             opLoc());
        return Value{VK::None, ""};
    }
    if (name == "net_udp_send") {
        need(3);
        Value h = genExpr(*c.args[0]);
        Value p = genExpr(*c.args[1]);
        Value d = genExpr(*c.args[2]);
        if (h.vk != VK::Str)
            unsupported(c.args[0]->range, "net_udp_send() 主机参数必须是 str");
        if (p.vk != VK::Int)
            unsupported(c.args[1]->range, "net_udp_send() 端口参数必须是 int");
        if (d.vk != VK::Str)
            unsupported(c.args[2]->range, "net_udp_send() 数据参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.net_udp_send\"(" + h.ssa + ", " + p.ssa + ", " +
             d.ssa + ") : (!llvm.ptr, i64, !llvm.ptr) -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "net_http_get") {
        need(1);
        Value u = genExpr(*c.args[0]);
        if (u.vk != VK::Str)
            unsupported(c.args[0]->range, "net_http_get() URL参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.net_http_get\"(" + u.ssa +
             ") : (!llvm.ptr) -> !llvm.ptr " + opLoc());
        return Value{VK::Str, ssa};
    }
    // 0.9.10：HTTP GET → Json（status/body 两键；分帧解析在运行时 C，
    // 与 VM 侧镜像）
    if (name == "net_http_get_json") {
        need(1);
        Value u = genExpr(*c.args[0]);
        if (u.vk != VK::Str)
            unsupported(c.args[0]->range,
                        "net_http_get_json() URL参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.net_http_get_json\"(" + u.ssa +
             ") : (!llvm.ptr) -> !llvm.ptr " + opLoc());
        return Value{VK::Json, ssa};
    }
    // F/G 迭代 2：TCP 服务端与收发超时（句柄不透明 I64；失败 panic 挂
    // opLoc，与 sg_net_* 运行时消息逐字一致）。
    if (name == "net_bind") {
        need(2);
        Value h = genExpr(*c.args[0]);
        Value p = genExpr(*c.args[1]);
        if (h.vk != VK::Str)
            unsupported(c.args[0]->range, "net_bind() 主机参数必须是 str");
        if (p.vk != VK::Int)
            unsupported(c.args[1]->range, "net_bind() 端口参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.net_bind\"(" + h.ssa + ", " + p.ssa +
             ") : (!llvm.ptr, i64) -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "net_accept") {
        need(1);
        Value s = genExpr(*c.args[0]);
        if (s.vk != VK::Int)
            unsupported(c.args[0]->range, "net_accept() 句柄参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.net_accept\"(" + s.ssa + ") : (i64) -> i64 " +
             opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "net_set_timeout") {
        need(2);
        Value s = genExpr(*c.args[0]);
        Value m = genExpr(*c.args[1]);
        if (s.vk != VK::Int)
            unsupported(c.args[0]->range,
                        "net_set_timeout() 句柄参数必须是 int");
        if (m.vk != VK::Int)
            unsupported(c.args[1]->range,
                        "net_set_timeout() 毫秒参数必须是 int");
        emit("\"lang.net_set_timeout\"(" + s.ssa + ", " + m.ssa +
             ") : (i64, i64) -> () " + opLoc());
        return Value{VK::None, ""};
    }
    // ---- 0.9.8 HTTP 服务端：读请求 → Json / 写响应 → 发送字节数 ----
    if (name == "net_http_read_request") {
        need(1);
        Value s = genExpr(*c.args[0]);
        if (s.vk != VK::Int)
            unsupported(c.args[0]->range,
                        "net_http_read_request() 句柄参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.net_http_read_request\"(" + s.ssa +
             ") : (i64) -> !llvm.ptr " + opLoc());
        return Value{VK::Json, ssa};
    }
    if (name == "net_http_write_response") {
        need(4);
        Value s = genExpr(*c.args[0]);
        Value st = genExpr(*c.args[1]);
        Value ct = genExpr(*c.args[2]);
        Value bd = genExpr(*c.args[3]);
        if (s.vk != VK::Int)
            unsupported(c.args[0]->range,
                        "net_http_write_response() 句柄参数必须是 int");
        if (st.vk != VK::Int)
            unsupported(c.args[1]->range,
                        "net_http_write_response() 状态码参数必须是 int");
        if (ct.vk != VK::Str)
            unsupported(c.args[2]->range,
                        "net_http_write_response() 内容类型参数必须是 str");
        if (bd.vk != VK::Str)
            unsupported(c.args[3]->range,
                        "net_http_write_response() 响应体参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.net_http_write_response\"(" + s.ssa + ", " +
             st.ssa + ", " + ct.ssa + ", " + bd.ssa +
             ") : (i64, i64, !llvm.ptr, !llvm.ptr) -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    // ---- 真并发 M1（0.9.11，thread 域）：线程/锁/通道。句柄不透明 I64；
    // 全部挂 opLoc（能力行 + panic 上下文与 VMThread.cpp 逐字一致）。
    // thread_spawn：Sema 已限定实参为字符串字面量且指向当前模块 0 参函数，
    // 此处从 AST 直接取字面量文本，附 sg_target 符号属性供降级层取地址。
    if (name == "thread_spawn") {
        need(1);
        Value f = genExpr(*c.args[0]);
        if (f.vk != VK::Str)
            unsupported(c.args[0]->range,
                        "thread_spawn() 实参必须是字符串字面量");
        auto& lit = static_cast<const StrLit&>(*c.args[0]);
        std::string ssa = fresh();
        emit(ssa + " = \"lang.thread_spawn\"(" + f.ssa + ") {sg_target = " +
             funcSym(curModule_, lit.strValue) + "} : (!llvm.ptr) -> i64 " +
             opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "thread_join" || name == "lock_acquire" ||
        name == "lock_release" || name == "chan_close") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Int)
            unsupported(c.args[0]->range,
                        name + "() 句柄参数必须是 int");
        emit("\"lang." + name + "\"(" + v.ssa + ") : (i64) -> () " +
             opLoc());
        return Value{VK::None, ""};
    }
    if (name == "lock_new" || name == "channel_new") {
        need(0);
        std::string ssa = fresh();
        emit(ssa + " = \"lang." + name + "\"() : () -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "chan_send") {
        need(2);
        Value h = genExpr(*c.args[0]);
        Value m = genExpr(*c.args[1]);
        if (h.vk != VK::Int)
            unsupported(c.args[0]->range, "chan_send() 句柄参数必须是 int");
        if (m.vk != VK::Str)
            unsupported(c.args[1]->range, "chan_send() 消息参数必须是 str");
        emit("\"lang.chan_send\"(" + h.ssa + ", " + m.ssa +
             ") : (i64, !llvm.ptr) -> () " + opLoc());
        return Value{VK::None, ""};
    }
    if (name == "chan_recv") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Int)
            unsupported(c.args[0]->range, "chan_recv() 句柄参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.chan_recv\"(" + v.ssa +
             ") : (i64) -> !llvm.ptr " + opLoc());
        return Value{VK::Str, ssa};
    }
    // ---- 系统编程内建（0.9.4 H 专项）：args/env/时间日期/进程/信号。
    // panic 的 localtime/gmtime/spawn_wait 挂 opLoc 上下文（与 sg_* 运行时
    // 消息逐字一致）；能力行（0.9.7）也一律挂 opLoc——sg_cap_check 拒绝时
    // panic 上下文与 VM panicHere 逐字一致；列表返回静态记元素 VK（驱动
    // list_get_*/print 分派）。
    if (name == "args") {
        std::string ssa = fresh();
        emit(ssa + " = \"lang.args\"() : () -> !llvm.ptr " + opLoc());
        return Value{VK::List, ssa, false, 0, static_cast<uint8_t>(VK::Str)};
    }
    if (name == "env" || name == "has_env" || name == "unset_env") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, name + "() 名称参数必须是 str");
        std::string ssa = fresh();
        if (name == "env") {
            emit(ssa + " = \"lang.env\"(" + v.ssa +
                 ") : (!llvm.ptr) -> !llvm.ptr " + opLoc());
            return Value{VK::Str, ssa};
        }
        if (name == "has_env") {
            emit(ssa + " = \"lang.has_env\"(" + v.ssa +
                 ") : (!llvm.ptr) -> i1 " + opLoc());
            return Value{VK::Bool, ssa};
        }
        emit("\"lang.unset_env\"(" + v.ssa + ") : (!llvm.ptr) -> () " +
             opLoc());
        return Value{VK::None, ""};
    }
    if (name == "set_env") {
        need(2);
        Value n = genExpr(*c.args[0]);
        Value v = genExpr(*c.args[1]);
        if (n.vk != VK::Str)
            unsupported(c.args[0]->range, "set_env() 名称参数必须是 str");
        if (v.vk != VK::Str)
            unsupported(c.args[1]->range, "set_env() 值参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.set_env\"(" + n.ssa + ", " + v.ssa +
             ") : (!llvm.ptr, !llvm.ptr) -> i1 " + opLoc());
        return Value{VK::Bool, ssa};
    }
    if (name == "epoch" || name == "epoch_ms" || name == "signal_caught") {
        std::string ssa = fresh();
        if (name == "signal_caught") {
            emit(ssa + " = \"lang.signal_caught\"() : () -> i1 " + opLoc());
            return Value{VK::Bool, ssa};
        }
        emit(ssa + " = \"lang." + name + "\"() : () -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "signal_reset") {
        emit("\"lang.signal_reset\"() : () -> () " + opLoc());
        return Value{VK::None, ""};
    }
    if (name == "localtime" || name == "gmtime") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Int)
            unsupported(c.args[0]->range, name + "() 时间参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang." + name + "\"(" + v.ssa + ") : (i64) -> !llvm.ptr " +
             opLoc());
        return Value{VK::List, ssa, false, 0, static_cast<uint8_t>(VK::Int)};
    }
    if (name == "system") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, "system() 命令参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.system\"(" + v.ssa + ") : (!llvm.ptr) -> i64 " +
             opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "spawn_wait") {
        need(2);
        Value p = genExpr(*c.args[0]);
        Value l = genExpr(*c.args[1]);
        if (p.vk != VK::Str)
            unsupported(c.args[0]->range, "spawn_wait() 路径参数必须是 str");
        if (l.vk != VK::List)
            unsupported(c.args[1]->range, "spawn_wait() 参数列表必须是 List");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.spawn_wait\"(" + p.ssa + ", " + l.ssa +
             ") : (!llvm.ptr, !llvm.ptr) -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    // ---- GUI M1+M2（0.9.11，gui 域，仅 AOT/Windows）：窗口/控件/事件。
    // 句柄不透明 I64；全部挂 opLoc（能力行 + panic 上下文与 sg_gui_* 运行
    // 时一致）；gui_poll 返回 Dict[str,str]（静态记元素 VK 驱动 dict 消费）。
    if (name == "gui_window") {
        need(3);
        Value t = genExpr(*c.args[0]);
        Value w = genExpr(*c.args[1]);
        Value h = genExpr(*c.args[2]);
        if (t.vk != VK::Str)
            unsupported(c.args[0]->range, "gui_window() 标题参数必须是 str");
        if (w.vk != VK::Int || h.vk != VK::Int)
            unsupported(c.args[1]->range, "gui_window() 尺寸参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.gui_window\"(" + t.ssa + ", " + w.ssa + ", " +
             h.ssa + ") : (!llvm.ptr, i64, i64) -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "gui_label" || name == "gui_button") {
        need(6);
        Value win = genExpr(*c.args[0]);
        Value t = genExpr(*c.args[1]);
        if (win.vk != VK::Int)
            unsupported(c.args[0]->range,
                        name + "() 窗口句柄参数必须是 int");
        if (t.vk != VK::Str)
            unsupported(c.args[1]->range, name + "() 文本参数必须是 str");
        std::string coords[4];
        for (int k = 0; k < 4; ++k) {
            Value v = genExpr(*c.args[k + 2]);
            if (v.vk != VK::Int)
                unsupported(c.args[k + 2]->range,
                            name + "() 位置/尺寸参数必须是 int");
            coords[k] = v.ssa;
        }
        std::string ssa = fresh();
        emit(ssa + " = \"lang." + name + "\"(" + win.ssa + ", " + t.ssa +
             ", " + coords[0] + ", " + coords[1] + ", " + coords[2] + ", " +
             coords[3] + ") : (i64, !llvm.ptr, i64, i64, i64, i64) -> i64 " +
             opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "gui_input") {
        need(5);
        Value win = genExpr(*c.args[0]);
        if (win.vk != VK::Int)
            unsupported(c.args[0]->range,
                        "gui_input() 窗口句柄参数必须是 int");
        std::string coords[4];
        for (int k = 0; k < 4; ++k) {
            Value v = genExpr(*c.args[k + 1]);
            if (v.vk != VK::Int)
                unsupported(c.args[k + 1]->range,
                            "gui_input() 位置/尺寸参数必须是 int");
            coords[k] = v.ssa;
        }
        std::string ssa = fresh();
        emit(ssa + " = \"lang.gui_input\"(" + win.ssa + ", " + coords[0] +
             ", " + coords[1] + ", " + coords[2] + ", " + coords[3] +
             ") : (i64, i64, i64, i64, i64) -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "gui_poll") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Int)
            unsupported(c.args[0]->range,
                        "gui_poll() 窗口句柄参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.gui_poll\"(" + v.ssa +
             ") : (i64) -> !llvm.ptr " + opLoc());
        return Value{VK::Dict, ssa, false, 0, static_cast<uint8_t>(VK::Str)};
    }
    if (name == "gui_set_text" || name == "gui_title") {
        need(2);
        Value h = genExpr(*c.args[0]);
        Value t = genExpr(*c.args[1]);
        if (h.vk != VK::Int)
            unsupported(c.args[0]->range, name + "() 句柄参数必须是 int");
        if (t.vk != VK::Str)
            unsupported(c.args[1]->range, name + "() 文本参数必须是 str");
        emit("\"lang." + name + "\"(" + h.ssa + ", " + t.ssa +
             ") : (i64, !llvm.ptr) -> () " + opLoc());
        return Value{VK::None, ""};
    }
    if (name == "gui_get_text") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Int)
            unsupported(c.args[0]->range,
                        "gui_get_text() 句柄参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.gui_get_text\"(" + v.ssa +
             ") : (i64) -> !llvm.ptr " + opLoc());
        return Value{VK::Str, ssa};
    }
    if (name == "gui_show" || name == "gui_hide" || name == "gui_close") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Int)
            unsupported(c.args[0]->range, name + "() 句柄参数必须是 int");
        emit("\"lang." + name + "\"(" + v.ssa + ") : (i64) -> () " +
             opLoc());
        return Value{VK::None, ""};
    }
    return std::nullopt;
}

} // namespace mlirgen
} // namespace aot
} // namespace scourge
