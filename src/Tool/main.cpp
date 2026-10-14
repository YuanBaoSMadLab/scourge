// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 工具链 — 单二进制驱动（设计文档第九章 9.1：argv0 分发）
// ----------------------------------------------------------------------------
//  命令面（P1 双模执行 + P2 外部协作）：
//    scourge check [--json] [-j N] <file.sg>...  全量验证（解析→类型→形状→契约）
//    scourge interface [--out <f>] <file>        生成 .sgi 接口文件
//    scourge run [--no-cache] [--watch] <file>   编译 SG-BC 字节码并执行
//                                                （.sgbc 缓存；--watch 保存即重跑）
//    scourge repl                                会话式求值（REPL，:help 查看会话命令）
//    scourge mcp                                 MCP 服务器（stdio JSON-RPC 2.0，
//                                                供外部 AI 代理零胶水接入）
//    scourge version / help
//
//  编译会话（加载 + DAG 分层并行检查）在 Tool/Session，run 执行核心在
//  Tool/Runner，REPL 在 Tool/Repl，MCP 服务器在 Tool/McpServer——
//  本文件只保留命令分发与 watch 循环。
//
//  退出码：0 成功；1 诊断错误；2 用法错误。
// ============================================================================
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "scourge/Common/Version.hpp"
#include "scourge/Tool/Builder.hpp"
#include "scourge/Tool/McpServer.hpp"
#include "scourge/Tool/Repl.hpp"
#include "scourge/Tool/Runner.hpp"
#include "scourge/Tool/Session.hpp"

namespace fs = std::filesystem;
using namespace scourge;

namespace {

// ----------------------------------------------------------------------------
// 命令实现
// ----------------------------------------------------------------------------

void printDiags(DiagEngine& diag, bool json) {
    if (json) {
        JsonWriter w(/*pretty=*/true);
        diag.emitJson(w);
        std::cout << w.take() << "\n";
        return;
    }
    std::string out;
    diag.emitText(out);
    std::cout << out;
}

int cmdCheck(const std::vector<std::string>& args) {
    bool json = false;
    unsigned jobs = 0;
    std::vector<std::string> files;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--json") {
            json = true;
        } else if (args[i] == "-j" && i + 1 < args.size()) {
            jobs = static_cast<unsigned>(std::strtoul(args[++i].c_str(), nullptr, 10));
        } else {
            files.push_back(args[i]);
        }
    }
    if (files.empty()) {
        std::cerr << "用法: scourge check [--json] [-j N] <file.sg>...\n";
        return 2;
    }

    bool anyError = false;
    for (const auto& f : files) {
        // 每个入口独立会话（SourceMgr 独立，保证 file:line 对应该文件）。
        Session session;
        session.compile(fs::path(f), jobs);
        size_t err = session.diag().errorCount();
        size_t warn = session.diag().warningCount();

        if (json) {
            printDiags(session.diag(), true);
        } else if (session.diag().all().empty()) {
            std::cout << f << ": OK（0 错误, 0 警告）\n";
        } else {
            printDiags(session.diag(), false);
            std::cout << f << ": " << err << " 错误, " << warn << " 警告\n";
        }
        anyError |= (err > 0);
    }
    return anyError ? 1 : 0;
}

int cmdInterface(const std::vector<std::string>& args) {
    std::string outFile;
    std::string inFile;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--out" && i + 1 < args.size()) {
            outFile = args[++i];
        } else {
            inFile = args[i];
        }
    }
    if (inFile.empty()) {
        std::cerr << "用法: scourge interface [--out <file>] <file.sg>\n";
        return 2;
    }

    Session session;
    fs::path inPath(inFile);
    std::string stem = inPath.stem().string();
    session.compile(inPath);

    if (session.diag().hasErrors()) {
        printDiags(session.diag(), false);
        std::cerr << "interface 生成中止：模块未通过全量验证（发布门禁）\n";
        return 1;
    }

    std::string sgi = session.emitInterface(stem);
    if (sgi.empty()) {
        std::cerr << "interface 生成失败：模块未加载\n";
        return 1;
    }

    fs::path out =
        outFile.empty() ? inPath.parent_path() / (stem + ".sgi")
                        : (outFile == "-" ? "-" : fs::path(outFile));
    if (outFile == "-") {
        std::cout << sgi;
    } else {
        std::ofstream of(out, std::ios::binary);
        of << sgi;
        std::cout << "已生成 " << out.string() << "\n";
    }
    return 0;
}

// ----------------------------------------------------------------------------
// run --watch（设计文档 5.3）：保存 → 重编 → 执行 → 与上次输出 diff 展示。
// 执行核心 runOnce 已提取至 Tool/Runner（CLI 与 MCP 服务器共用，零分叉）。
// 轮询模块文件 mtime（简单可靠）；编辑器分步落盘经静默窗口防抖。
// 退出方式：Ctrl+C。重跑走 runOnce——.sgbc 指纹未变的模块自动命中缓存。
// ----------------------------------------------------------------------------

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            lines.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(std::move(cur));
    return lines;
}

/// 输出 diff：公共前后缀裁剪，中段以 -/+ 成对展示（超长截断）。
void printOutputDiff(const std::vector<std::string>& oldL,
                     const std::vector<std::string>& newL) {
    size_t p = 0;
    while (p < oldL.size() && p < newL.size() && oldL[p] == newL[p]) ++p;
    size_t s = 0;
    while (s < oldL.size() - p && s < newL.size() - p &&
           oldL[oldL.size() - 1 - s] == newL[newL.size() - 1 - s])
        ++s;
    if (p >= oldL.size() - s && p >= newL.size() - s) {
        std::cout << "[watch] 输出无变化\n";
        return;
    }
    constexpr size_t kMaxDiffLines = 200;
    size_t shown = 0;
    for (size_t i = p; i < oldL.size() - s; ++i) {
        if (shown++ == kMaxDiffLines) {
            std::cout << "  …（diff 截断）\n";
            return;
        }
        std::cout << "- " << oldL[i] << "\n";
    }
    for (size_t i = p; i < newL.size() - s; ++i) {
        if (shown++ == kMaxDiffLines) {
            std::cout << "  …（diff 截断）\n";
            return;
        }
        std::cout << "+ " << newL[i] << "\n";
    }
}

int cmdRunWatch(const fs::path& inPath, bool useCache) {
    auto snapshot = [](const std::vector<fs::path>& ps) {
        std::map<fs::path, fs::file_time_type> m;
        for (const auto& p : ps) {
            std::error_code ec;
            fs::file_time_type t = fs::last_write_time(p, ec);
            m[p] = ec ? (fs::file_time_type::min)() : t; // 括号防 min 宏（windows.h）
        }
        return m;
    };

    RunOutcome r = runOnce(inPath, useCache, /*captureOut=*/true);
    if (!r.log.empty()) std::cerr << r.log; // 失败诊断走 stderr（不混入输出流）
    if (r.modulePaths.empty()) {
        std::cerr << "watch 中止：无法确定监视目标（模块加载失败）\n";
        return 1;
    }
    std::cout << r.output << std::flush;
    std::vector<std::string> last = splitLines(r.output);
    auto mtimes = snapshot(r.modulePaths);
    std::cout << "[watch] 监视 " << r.modulePaths.size()
              << " 个模块：保存后自动重跑，Ctrl+C 退出\n"
              << std::flush;

    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        auto now = snapshot(r.modulePaths);
        if (now == mtimes) continue;

        // 防抖：等待写入静默（编辑器常分步落盘 / 临时文件替换）。
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            auto quiet = snapshot(r.modulePaths);
            if (quiet == now) break;
            now = quiet;
        }
        mtimes = now;

        std::cout << "\n[watch] 检测到变更，重跑 " << inPath.string() << "\n"
                  << std::flush;
        r = runOnce(inPath, useCache, /*captureOut=*/true);
        if (!r.log.empty()) std::cerr << r.log;
        printOutputDiff(last, splitLines(r.output));
        last = splitLines(r.output);
        if (!r.modulePaths.empty())
            mtimes = snapshot(r.modulePaths); // 模块集可能变化（增删 import）
        std::cout << std::flush;
    }
}

int cmdRun(const std::vector<std::string>& args) {
    bool useCache = true;
    bool watch = false;
    std::string inFile;
    for (const auto& a : args) {
        if (a == "--no-cache")
            useCache = false;
        else if (a == "--watch")
            watch = true;
        else
            inFile = a;
    }
    if (inFile.empty() || args.size() > 3) {
        std::cerr << "用法: scourge run [--no-cache] [--watch] <file.sg>\n";
        return 2;
    }

    fs::path inPath(inFile);
    if (!watch) {
        RunOutcome r = runOnce(inPath, useCache, /*captureOut=*/false);
        // 失败诊断走 stderr——stdout 只承载程序输出（与 MCP 通道纪律一致）。
        if (!r.log.empty()) std::cerr << r.log;
        return r.exitCode;
    }
    return cmdRunWatch(inPath, useCache);
}

int cmdBuild(const std::vector<std::string>& args) {
    std::string outFile;
    std::string inFile;
    bool gpuMode = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) {
            outFile = args[++i];
        } else if (args[i] == "--gpu") {
            gpuMode = true; /* GPU 覆盖集 op 走 CUDA 动态加载（自动回退 CPU） */
        } else {
            inFile = args[i];
        }
    }
    if (inFile.empty() || args.size() > 4) {
        std::cerr << "用法: scourge build [--gpu] <file.sg> [-o out.exe]\n";
        return 2;
    }

    fs::path inPath(inFile);
    fs::path outPath =
        outFile.empty()
            ? inPath.parent_path() / (inPath.stem().string() + ".exe")
            : fs::path(outFile);
    BuildOutcome r = buildOnce(inPath, outPath, gpuMode);
    // 失败文本走 stderr——stdout 只承载构建结果报告。
    if (!r.log.empty()) std::cerr << r.log;
    if (r.exitCode == 0) std::cout << "已生成 " << r.exe.string() << "\n";
    return r.exitCode;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); // 中文诊断在 Windows 控制台正常显示
#endif
    std::vector<std::string> args(argv + (argc > 1 ? 1 : 0), argv + argc);
    if (args.empty()) {
        std::cout << kScourgeVersionLabel
                  << "\n用法: scourge <check|interface|run|build|repl|mcp|version|help> ...\n";
        return 2;
    }
    const std::string cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (cmd == "version" || cmd == "--version") {
        std::cout << kScourgeVersionLabel << "\n";
        return 0;
    }
    if (cmd == "help" || cmd == "--help") {
        std::cout << kScourgeVersionLabel << "\n\n"
                  << "命令:\n"
            << "  check [--json] [-j N] <file.sg>...  全量验证（JSON 诊断可选，N 为并行度）\n"
            << "  interface [--out <f>] <file>        生成 .sgi 接口文件\n"
            << "  run [--no-cache] [--watch] <file>   编译执行（.sgbc 缓存；--watch 保存即重跑）\n"
            << "  build [-o out.exe] <file>           AOT 构建：MLIR → LLVM IR → 原生可执行\n"
            << "  repl                                会话式求值（REPL；会话内 :help 查看命令）\n"
            << "  mcp                                 MCP 服务器模式（stdio JSON-RPC 2.0，供 AI 代理接入）\n"
            << "  version                             版本信息\n"
            << "  help                                本帮助\n";
        return 0;
    }
    if (cmd == "check") return cmdCheck(rest);
    if (cmd == "interface") return cmdInterface(rest);
    if (cmd == "run") return cmdRun(rest);
    if (cmd == "build") return cmdBuild(rest);
    if (cmd == "repl") {
        if (!rest.empty()) {
            std::cerr << "用法: scourge repl\n";
            return 2;
        }
        return cmdRepl();
    }
    if (cmd == "mcp") {
        if (!rest.empty()) {
            std::cerr << "用法: scourge mcp（stdio JSON-RPC，无命令行参数）\n";
            return 2;
        }
        return cmdMcp();
    }

    std::cerr << "未知命令 '" << cmd << "'（scourge help 查看用法）\n";
    return 2;
}
