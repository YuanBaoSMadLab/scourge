// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 内建函数名单一事实源
// ----------------------------------------------------------------------------
//  职责边界：Sema（builtinType）/ BC（Compiler 注册）/ AOT（MLIRGen 分派）
//  共用同一清单。新增内建只需在此登记一次；各端实现仍按名分派。
//  format 不在此表：Parser 已降糖，仅 Sema 保留字面量首参的定向诊断。
// ============================================================================
#pragma once

#include <string_view>

namespace scourge {

inline constexpr const char* kBuiltinNames[] = {
    "print",     "str",   "len", "sum",       "now",   "range", "ok", "err",
    // Option 糖：some(x) 构造 Some，none() 即空选项（≡ Result[T,None]）。
    "some",      "none",
    // F-20：可调用 panic
    "panic",
    "abs",       "sqrt",  "exp", "log",       "max",   "min",
    "transpose", "reshape", "data_ptr",
    // 标量数学/随机/排序
    "sin",  "cos",  "tan",   "asin", "acos", "atan", "sinh", "cosh",
    "tanh", "atan2", "floor", "ceil", "round", "trunc",
    "random", "seed", "sorted",
    // IO 子系统
    "input", "read_text", "write_text", "append_text", "exists",
    "list_dir",
    // 网络客户端与睡眠
    "net_connect", "net_send", "net_recv", "net_close", "net_udp_send",
    "net_http_get", "net_http_get_json", "sleep",
    "net_bind", "net_accept", "net_set_timeout",
    // HTTP 服务端
    "net_http_read_request", "net_http_write_response",
    // 系统编程（args/env/时间/进程/信号）
    "args", "env", "has_env", "set_env", "unset_env", "epoch", "epoch_ms",
    "localtime", "gmtime", "system", "spawn_wait", "signal_caught",
    "signal_reset",
    // 标准库广度（统计/倒序/哈希）
    "mean", "var", "std", "median", "reversed", "sha256", "crc32",
    // JSON
    "json_decode",
    // 类型转换/码点
    "int", "float", "ord", "chr",
    // 真并发
    "thread_spawn", "thread_join", "lock_new", "lock_acquire",
    "lock_release", "channel_new", "chan_send", "chan_recv", "chan_close",
    // GUI（VM 侧运行到即 panic；AOT 发射）
    "gui_window", "gui_close", "gui_poll", "gui_label", "gui_button",
    "gui_input", "gui_set_text", "gui_get_text", "gui_show", "gui_hide",
    "gui_title",
    // fmt()：f-string :spec 降糖目标
    "fmt"};

inline bool isBuiltinName(std::string_view n) {
    for (const char* b : kBuiltinNames)
        if (n == b) return true;
    return false;
}

} // namespace scourge
