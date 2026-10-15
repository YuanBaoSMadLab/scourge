// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 字节码 VM — 系统编程内建（0.9.4 H 专项）
// ----------------------------------------------------------------------------
//  职责边界：只实现 args/env 组、时间日期组、system/spawn_wait、信号四件；
//  doBuiltin 按白名单分派进来（VM.cpp 尾段路由，本文件是唯一实现处）。
//  语义契约（与 src/Tool/BuilderRTSys.hpp 的 sg_* 逐字一致，双模差分由
//  verify 探针锁定）：
//    * args() = opts_.args（不含程序名）；AOT 侧为 ANSI 代码页窄字节
//      （CommandLineToArgvW → CP_ACP），与 VM 侧 scourge.exe 的 CRT argv 同源；
//    * env 缺失返回 ""；set_env 名含 '=' 或空拒绝（返回 false）；unset_env
//      同条件静默无操作；
//    * localtime/gmtime 返回 [年,月,日,时,分,秒,星期0=周日,年积日(1 起)]，
//      转换失败 panic（消息与 AOT 逐字一致）；
//    * system(cmd) 直通 CRT system()（cmd.exe /c，返回退出码，不 panic）；
//    * spawn_wait 引号规则：空串或含空格/制表符加引号，内嵌引号翻倍；
//      启动失败 panic；句柄全关；
//    * 信号 = 原子标志轮询：SetConsoleCtrlHandler 捕获 CTRL_C/CTRL_BREAK
//      置标志并返回 TRUE（注册后 Ctrl+C 不再默认终止进程）。
// ============================================================================
#include "scourge/BC/VM.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace scourge {
namespace bc {

namespace {

/// 引号规则：空串或含空格/制表符 → 加引号；内嵌引号翻倍（MSVC 解析规则，
/// 与 BuilderRTSys.hpp 的 sg_spawn_quote 逐字一致）。
void spawnQuote(const std::string& s, std::string& out) {
    bool need = s.empty() || s.find(' ') != std::string::npos ||
                s.find('\t') != std::string::npos;
    if (need) out += '"';
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    if (need) out += '"';
}

/// 信号 = 原子标志轮询（与 sg_sig_handler 同语义：捕获 C/BREAK，返回 TRUE
/// 接管默认终止行为；其余事件放行默认处理）。
volatile LONG g_sigFlag = 0;

BOOL WINAPI sigHandler(DWORD t) {
    if (t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_sigFlag, 1);
        return TRUE;
    }
    return FALSE;
}

void signalEnsure() {
    static bool done = false;
    if (!done) {
        SetConsoleCtrlHandler(sigHandler, TRUE);
        done = true;
    }
}

} // namespace

void VM::sysBuiltin(const std::string& n, std::vector<Value>& a) {
    auto need = [&](size_t want) {
        if (a.size() != want)
            panicHere("内建 " + n + "() 需要 " + std::to_string(want) +
                      " 个参数");
    };
    auto needStr = [&](size_t k, const char* what) -> const std::string& {
        if (a[k].kind != Value::Kind::Str)
            panicHere(n + "() " + what + "必须是 str");
        return a[k].s;
    };

    if (n == "args") {
        auto out = std::make_shared<std::vector<Value>>();
        out->reserve(opts_.args.size());
        for (const std::string& s : opts_.args)
            out->push_back(Value::makeStr(s));
        push(Value::makeList(std::move(out)));
        return;
    }
    if (n == "env") {
        need(1);
        const std::string& name = needStr(0, "名称参数");
        const char* v = std::getenv(name.c_str());
        push(Value::makeStr(v ? v : ""));
        return;
    }
    if (n == "has_env") {
        need(1);
        const std::string& name = needStr(0, "名称参数");
        push(Value::makeBool(std::getenv(name.c_str()) != nullptr));
        return;
    }
    if (n == "set_env") {
        need(2);
        const std::string& name = needStr(0, "名称参数");
        const std::string& value = needStr(1, "值参数");
        if (name.empty() || name.find('=') != std::string::npos) {
            push(Value::makeBool(false));
            return;
        }
        push(Value::makeBool(_putenv((name + "=" + value).c_str()) == 0));
        return;
    }
    if (n == "unset_env") {
        need(1);
        const std::string& name = needStr(0, "名称参数");
        if (!name.empty() && name.find('=') == std::string::npos)
            _putenv((name + "=").c_str()); /* "name=" 删除该变量 */
        push(Value::makeNone());
        return;
    }
    if (n == "epoch") {
        push(Value::makeInt(static_cast<int64_t>(std::time(nullptr))));
        return;
    }
    if (n == "epoch_ms") {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        int64_t t = (static_cast<int64_t>(ft.dwHighDateTime) << 32) |
                    ft.dwLowDateTime;
        /* 1601-01-01 → 1970-01-01 的 100ns 间隔（11644473600 秒） */
        push(Value::makeInt(
            (t - static_cast<int64_t>(11644473600ll) * 10000000ll) / 10000ll));
        return;
    }
    if (n == "localtime" || n == "gmtime") {
        need(1);
        if (a[0].kind != Value::Kind::Int)
            panicHere(n + "() 时间参数必须是 int");
        std::time_t t = static_cast<std::time_t>(a[0].i);
        std::tm tmv;
        int rc = n == "localtime" ? localtime_s(&tmv, &t) : gmtime_s(&tmv, &t);
        if (rc != 0)
            panicHere(n == "localtime" ? "localtime() 时间值超出可表示范围"
                                       : "gmtime() 时间值超出可表示范围");
        std::vector<Value> d;
        d.reserve(8);
        d.push_back(Value::makeInt(tmv.tm_year + 1900));
        d.push_back(Value::makeInt(tmv.tm_mon + 1));
        d.push_back(Value::makeInt(tmv.tm_mday));
        d.push_back(Value::makeInt(tmv.tm_hour));
        d.push_back(Value::makeInt(tmv.tm_min));
        d.push_back(Value::makeInt(tmv.tm_sec));
        d.push_back(Value::makeInt(tmv.tm_wday));
        d.push_back(Value::makeInt(tmv.tm_yday + 1));
        push(Value::makeList(
            std::make_shared<std::vector<Value>>(std::move(d))));
        return;
    }
    if (n == "system") {
        need(1);
        const std::string& cmd = needStr(0, "命令参数");
        push(Value::makeInt(
            static_cast<int64_t>(std::system(cmd.c_str()))));
        return;
    }
    if (n == "spawn_wait") {
        need(2);
        const std::string& path = needStr(0, "路径参数");
        if (a[1].kind != Value::Kind::List)
            panicHere("spawn_wait() 参数列表必须是 List");
        std::string cmdline;
        spawnQuote(path, cmdline);
        for (const Value& v : *a[1].list) {
            cmdline += ' ';
            spawnQuote(v.s, cmdline);
        }
        int wlen = MultiByteToWideChar(CP_ACP, 0, cmdline.c_str(), -1, nullptr,
                                       0);
        std::wstring wcmd(static_cast<size_t>(wlen > 0 ? wlen : 1), L'\0');
        MultiByteToWideChar(CP_ACP, 0, cmdline.c_str(), -1, wcmd.data(), wlen);
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));
        if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, nullptr, &si, &pi))
            panicHere("spawn_wait() 无法启动进程: " + path);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        push(Value::makeInt(static_cast<int64_t>(code)));
        return;
    }
    if (n == "signal_caught") {
        signalEnsure();
        push(Value::makeBool(InterlockedExchangeAdd(&g_sigFlag, 0) != 0));
        return;
    }
    if (n == "signal_reset") {
        signalEnsure();
        InterlockedExchange(&g_sigFlag, 0);
        push(Value::makeNone());
        return;
    }
    panicHere("VM 内部错误：未知系统内建 '" + n + "'");
}

} // namespace bc
} // namespace scourge
