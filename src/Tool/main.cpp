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
//  Tool/Runner，REPL 在 Tool/Repl，MCP 服务器在 Tool/McpServer，测试框架
//  在 Tool/TestRunner——本文件只保留命令分发与 watch 循环。
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
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include "scourge/Common/Version.hpp"
#include "scourge/Tool/Builder.hpp"
#include "scourge/Tool/McpServer.hpp"
#include "scourge/Tool/Repl.hpp"
#include "scourge/Tool/Runner.hpp"
#include "scourge/Tool/Session.hpp"
#include "scourge/Tool/TestRunner.hpp"

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
        std::cerr << "用法: sg check [--json] [-j N] <file.sg>...\n";
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
        std::cerr << "用法: sg interface [--out <file>] <file.sg>\n";
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

int cmdRunWatch(const fs::path& inPath, bool useCache,
                const std::vector<std::string>& scriptArgs) {
    auto snapshot = [](const std::vector<fs::path>& ps) {
        std::map<fs::path, fs::file_time_type> m;
        for (const auto& p : ps) {
            std::error_code ec;
            fs::file_time_type t = fs::last_write_time(p, ec);
            m[p] = ec ? (fs::file_time_type::min)() : t; // 括号防 min 宏（windows.h）
        }
        return m;
    };

    RunOutcome r = runOnce(inPath, useCache, /*captureOut=*/true, scriptArgs);
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
        r = runOnce(inPath, useCache, /*captureOut=*/true, scriptArgs);
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
    std::vector<std::string> scriptArgs; // <file> 之后的透传参数（args() 消费）
    bool fileSeen = false;
    for (const auto& a : args) {
        if (!fileSeen && a == "--no-cache")
            useCache = false;
        else if (!fileSeen && a == "--watch")
            watch = true;
        else if (!fileSeen)
            inFile = a, fileSeen = true;
        else
            scriptArgs.push_back(a); // 文件名后的一切原样透传（可含 -- 开头）
    }
    if (inFile.empty() || args.size() > 3 + scriptArgs.size()) {
        std::cerr << "用法: sg run [--no-cache] [--watch] <file.sg> [args...]\n";
        return 2;
    }

    fs::path inPath(inFile);
    if (inPath.extension() != ".sg") {
        // `sg run main` 短名：省略 .sg 后缀（存在 main.sg 才生效）。
        fs::path cand = inPath;
        cand += ".sg";
        std::error_code ec;
        if (fs::exists(cand, ec)) inPath = cand;
    }
    if (!watch) {
        RunOutcome r = runOnce(inPath, useCache, /*captureOut=*/false, scriptArgs);
        // 失败诊断走 stderr——stdout 只承载程序输出（与 MCP 通道纪律一致）。
        if (!r.log.empty()) std::cerr << r.log;
        return r.exitCode;
    }
    return cmdRunWatch(inPath, useCache, scriptArgs);
}

int cmdBuild(const std::vector<std::string>& args) {
    std::string outFile;
    std::string outDir;     // --out-dir：最终产物目录（默认 <源目录>/out）
    std::string buildDirS;  // --build-dir：中间产物/缓存目录（默认 <源目录>/.sgbuild）
    std::string inFile;
    bool gpuMode = false;
    bool useCache = true;
    bool explicitSysroot = false; // 显式 --sysroot：一键交叉不覆盖
    bool freestanding = false;    // --freestanding：裸机通道
    std::string board;            // --board <预设名>
    // 手工板级参数（预设展开后回填：显式参数 > 预设 > 默认）
    BareOpts manual;
    unsigned manualSet = 0; // 位标：0 kind 1 base 2 shift 3 clk 4 baud
                            //      5 load 6 stack 7 map
    CrossOpts xo;
    auto parseUll = [](const std::string& s, unsigned long long& out) {
        try {
            size_t idx = 0;
            out = std::stoull(s, &idx, s.rfind("0x", 0) == 0 ? 16 : 0);
            return idx == s.size();
        } catch (...) {
            return false;
        }
    };
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) {
            outFile = args[++i];
        } else if (args[i] == "--out-dir" && i + 1 < args.size()) {
            outDir = args[++i]; /* 最终产物目录覆盖（默认 <源目录>/out） */
        } else if (args[i] == "--build-dir" && i + 1 < args.size()) {
            buildDirS = args[++i]; /* 中间产物/缓存目录覆盖（默认 .sgbuild） */
        } else if (args[i] == "--gpu") {
            gpuMode = true; /* GPU 覆盖集 op 走 CUDA 动态加载（自动回退 CPU） */
        } else if (args[i] == "--no-cache") {
            useCache = false; /* 跳过 .sgbuild/obj 缓存，单步直编直链 */
        } else if (args[i] == "--target" && i + 1 < args.size()) {
            xo.target = args[++i]; /* LLVM triple（跨平台/嵌入式 Linux） */
        } else if (args[i].rfind("--target=", 0) == 0) {
            xo.target = args[i].substr(9); /* 等号形态（脚本/CI 友好） */
        } else if (args[i] == "--sysroot" && i + 1 < args.size()) {
            xo.sysroot = args[++i]; /* musl 静态 sysroot 目录 */
            explicitSysroot = true;
        } else if (args[i] == "--cpu" && i + 1 < args.size()) {
            xo.cpu = args[++i]; /* -mcpu（cortex-a53…） */
        } else if (args[i] == "--march" && i + 1 < args.size()) {
            xo.march = args[++i]; /* -march（armv7-a、rv64gc…） */
        } else if (args[i] == "--static") {
            xo.staticLink = true; /* 静态链接（交叉 POSIX 必需） */
        } else if (args[i] == "--freestanding") {
            freestanding = true; /* 裸机通道（无 libc；crt 内置） */
        } else if (args[i] == "--board" && i + 1 < args.size()) {
            board = args[++i]; /* 板级预设（隐含 --freestanding） */
        } else if (args[i] == "--uart-kind" && i + 1 < args.size()) {
            manual.uartKind = args[++i]; /* 16550 | pl011 */
            manualSet |= 1 << 0;
        } else if (args[i] == "--uart-base" && i + 1 < args.size()) {
            parseUll(args[++i], manual.uartBase);
            manualSet |= 1 << 1;
        } else if (args[i] == "--uart-shift" && i + 1 < args.size()) {
            manual.uartShift = std::atoi(args[++i].c_str());
            manualSet |= 1 << 2;
        } else if (args[i] == "--uart-clk" && i + 1 < args.size()) {
            parseUll(args[++i], manual.uartClk);
            manualSet |= 1 << 3;
        } else if (args[i] == "--baud" && i + 1 < args.size()) {
            manual.baud = std::atoi(args[++i].c_str());
            manualSet |= 1 << 4;
        } else if (args[i] == "--load-base" && i + 1 < args.size()) {
            parseUll(args[++i], manual.loadBase);
            manualSet |= 1 << 5;
        } else if (args[i] == "--stack" && i + 1 < args.size()) {
            parseUll(args[++i], manual.stackSize);
            manualSet |= 1 << 6;
        } else if (args[i] == "--map" && i + 1 < args.size()) {
            /* MMU 1GB 块属性 "0,1,1,0"（1=normal 0=device） */
            std::string m = args[++i];
            for (int gb = 0; gb < 4 && !m.empty(); ++gb) {
                manual.mapGb[gb] = m[0] == '1';
                size_t comma = m.find(',');
                m = comma == std::string::npos ? "" : m.substr(comma + 1);
            }
            manualSet |= 1 << 7;
        } else {
            inFile = args[i];
        }
    }
    if (inFile.empty() || args.size() > 44) {
        std::cerr << "用法: sg build [--gpu] [--no-cache] [--out-dir <dir>] "
                     "[--build-dir <dir>] "
                     "[--target <triple>|--target=<triple>] "
                     "[--sysroot <dir>] [--cpu <c>] [--march <a>] [--static] "
                     "[--freestanding] [--board <name>] [--uart-* ...] "
                     "<file.sg> [-o out]\n";
        return 2;
    }

    // ---- 裸机通道判定：--freestanding / --board / *-none-eabi triple。
    //      预设先行展开，显式 --uart-* 等手工参数最后覆盖（参数优先于预设，
    //      这里以"预设后回填默认、用户已设字段不覆盖"语义实现）。 ----
    if (board.empty() && !xo.target.empty() &&
        xo.target.find("-none-eabi") != std::string::npos) {
        freestanding = true; /* bare triple 即裸机（musl 通道用 linux-*） */
    }
    if (!board.empty()) {
        std::string err = applyBarePreset(board, xo.bare, xo.cpu);
        if (!err.empty()) {
            std::cerr << err << "\n";
            return 1;
        }
        if (xo.target.empty()) xo.target = "aarch64-none-eabi";
        freestanding = true;
    }
    if (freestanding) {
        // 显式手工参数最后回填（覆盖预设；未指定的字段保持预设/默认值）
        if (manualSet & (1 << 0)) xo.bare.uartKind = manual.uartKind;
        if (manualSet & (1 << 1)) xo.bare.uartBase = manual.uartBase;
        if (manualSet & (1 << 2)) xo.bare.uartShift = manual.uartShift;
        if (manualSet & (1 << 3)) xo.bare.uartClk = manual.uartClk;
        if (manualSet & (1 << 4)) xo.bare.baud = manual.baud;
        if (manualSet & (1 << 5)) xo.bare.loadBase = manual.loadBase;
        if (manualSet & (1 << 6)) xo.bare.stackSize = manual.stackSize;
        if (manualSet & (1 << 7))
            for (int gb = 0; gb < 4; ++gb)
                xo.bare.mapGb[gb] = manual.mapGb[gb];
        if (xo.target.empty()) {
            std::cerr << "--freestanding 需要 --target <triple>（当前仅 "
                         "aarch64-none-eabi）或 --board <预设>\n";
            return 2;
        }
        xo.bare.enabled = true;
        xo.bare.board = board;
    }

    // ---- 一键交叉（0.9.9）：POSIX triple 自动补齐 musl 静态链接与 sysroot
    // 定位——交叉不再要求记全 --target/--sysroot/--static 三件套。显式
    // --sysroot 优先（不覆盖，Builder 照旧校验其完整性）。裸机通道不走此路。 ----
    if (!xo.target.empty() && !xo.bare.enabled &&
        xo.target.find("windows") == std::string::npos &&
        xo.target.find("mingw") == std::string::npos) {
        xo.staticLink = true; // musl 静态产物零依赖（Builder 对交叉同此约束）
        if (!explicitSysroot) {
            // 候选相对当前目录，命中首个含 lib/libc.a 者（与 Builder 校验
            // 同一判定，避免选中不完整 sysroot）
            const std::string cands[] = {
                "vendor/" + xo.target + "/sysroot",
                "vendor/" + xo.target,
                "toolchain/sysroot-" + xo.target,
            };
            std::error_code ec;
            for (const std::string& c : cands) {
                if (fs::exists(fs::path(c) / "lib" / "libc.a", ec)) {
                    xo.sysroot = c;
                    break;
                }
            }
            if (xo.sysroot.empty()) {
                std::cerr << "一键交叉：未找到 " << xo.target
                          << " 的 musl sysroot（已尝试 vendor/" << xo.target
                          << "/sysroot、vendor/" << xo.target
                          << "/、toolchain/sysroot-" << xo.target
                          << "，要求含 lib/libc.a）\n"
                             "  处方：在仓库根目录运行 "
                             "toolchain/build_musl_sysroot.sh 产出后重试，"
                             "或显式 --sysroot <dir> 指定\n";
                return 1;
            }
        }
    }

    fs::path inPath(inFile);
    if (inPath.extension() != ".sg" && !xo.bare.enabled) {
        // `sg build main` 短名：省略 .sg 后缀（存在 main.sg 才生效）。
        fs::path cand = inPath;
        cand += ".sg";
        std::error_code ec;
        if (fs::exists(cand, ec)) inPath = cand;
    }
    // 产物路径裁定（优先级：-o 显式 > --out-dir > 默认 <源目录>/out/）。
    // 裸机通道产物为 .elf；宿主通道 Windows = .exe / POSIX = 无后缀。
    fs::path outPath;
    if (!outFile.empty()) {
        outPath = fs::path(outFile);
    } else {
        const char* ext =
            xo.bare.enabled ? ".elf"
#ifdef _WIN32
                            : ".exe";
#else
                            : "";
#endif
        fs::path base = outDir.empty() ? inPath.parent_path() / "out"
                                       : fs::path(outDir);
        outPath = base / (inPath.stem().string() + ext);
    }
    fs::path buildDir =
        buildDirS.empty() ? fs::path() : fs::path(buildDirS);
    BuildOutcome r = buildOnce(inPath, outPath, gpuMode, useCache, xo,
                               buildDir);
    // 失败文本走 stderr——stdout 只承载构建结果报告。
    if (!r.log.empty()) std::cerr << r.log;
    if (r.exitCode == 0) {
        // sget 包 DLL 分发（0.9.8 生态）：扫描 <输入文件目录>/packages/*/bin/
        // *.dll 拷到产物目录——FFI 走运行时 LoadLibraryA（搜索序列覆盖 exe
        // 所在目录），产物无需链接 .lib 即可用第三方动态库。缓存命中路径
        // 同样部署（重装/更新包后重链接亦生效）。
        std::error_code ec;
        fs::path pkgs = inPath.parent_path() / "packages";
        if (fs::exists(pkgs, ec)) {
            for (const auto& pkg : fs::directory_iterator(pkgs, ec)) {
                fs::path bin = pkg.path() / "bin";
                if (!fs::exists(bin, ec)) continue;
                for (const auto& dll : fs::directory_iterator(bin, ec)) {
                    if (dll.path().extension() != ".dll") continue;
                    fs::copy_file(dll.path(), outPath.parent_path() /
                                                  dll.path().filename(),
                                  fs::copy_options::overwrite_existing, ec);
                }
            }
        }
        std::cout << "已生成 " << r.exe.string()
                  << (r.fromCache ? "（AOT 缓存命中，仅链接）\n" : "\n");
        if (!r.bin.empty())
            std::cout << "已生成 " << r.bin.string() << "（raw bin）\n";
    }
    return r.exitCode;
}

} // namespace

#ifdef _WIN32
// 开发期崩溃报告：未处理异常时打印异常码与整栈符号（dbghelp + PDB，
// 支持文件:行号），定位编译器自身崩溃；不影响正常路径。
static LONG WINAPI crashReport(EXCEPTION_POINTERS* ep) {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);
    fprintf(stderr, "\ncompiler-crash code=0x%08lX\n",
            static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode));
    void* stk[32];
    USHORT n = CaptureStackBackTrace(0, 32, stk, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        DWORD64 pc = reinterpret_cast<DWORD64>(stk[i]);
        alignas(SYMBOL_INFOW) char buf[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)];
        auto* si = reinterpret_cast<SYMBOL_INFOW*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFOW);
        si->MaxNameLen = 255;
        DWORD64 off = 0;
        if (SymFromAddrW(proc, pc, &off, si)) {
            IMAGEHLP_LINEW64 ln{};
            ln.SizeOfStruct = sizeof(IMAGEHLP_LINEW64);
            DWORD disp = 0;
            if (SymGetLineFromAddrW64(proc, pc, &disp, &ln))
                fprintf(stderr, "  #%hu %ls+0x%llX (%ls:%lu)\n", i, si->Name,
                        off, ln.FileName, static_cast<unsigned long>(ln.LineNumber));
            else
                fprintf(stderr, "  #%hu %ls+0x%llX\n", i, si->Name, off);
        } else {
            fprintf(stderr, "  #%hu pc=%p\n", i, stk[i]);
        }
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); // 中文诊断在 Windows 控制台正常显示
    SetUnhandledExceptionFilter(crashReport);
#endif
    std::vector<std::string> args(argv + (argc > 1 ? 1 : 0), argv + argc);
    if (args.empty()) {
        std::cout << kScourgeVersionLabel
              << "\n用法: sg <check|interface|run|build|test|repl|mcp|version|help> ...\n";
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
            << "  run [--no-cache] [--watch] <file>   编译执行（缓存落 .sgbuild/；--watch 保存即重跑）\n"
            << "  build [--gpu] [--no-cache] [--out-dir <dir>] [--build-dir <dir>]\n"
            << "        [--target <triple>|--target=<triple>] [--sysroot <dir>]\n"
            << "        [--cpu <c>] [--march <a>] [--static]\n"
            << "        [--freestanding] [--board <name>] [--uart-kind/-base/-shift/-clk] [--baud <b>]\n"
            << "        [--load-base <hex>] [--stack <n>] [--map <0,1,1,0>] <file> [-o out]\n"
            << "                                    AOT 构建：MLIR → LLVM IR → 原生可执行。\n"
            << "                                      最终产物默认 <源目录>/out/，中间产物与\n"
            << "                                      缓存默认 <源目录>/.sgbuild/（--out-dir/\n"
            << "                                      --build-dir 可覆盖）；交叉编译目标：\n"
            << "                                      *linux-gnu 走 musl 静态 sysroot\n"
            << "                                      （POSIX triple 自动补 --static 并定位\n"
            << "                                      sysroot）；裸机 --freestanding/--board\n"
            << "                                      出 ELF+bin，预设 qemu-virt-a64 / rk3588\n"
            << "  repl                                会话式求值（REPL；会话内 :help 查看命令）\n"
            << "  test [--json] [--seed N] [--runs N] <file>\n"
            << "                                      测试框架：<stem>.sgt 显式测试 +\n"
            << "                                      @pure 契约属性测试（自动预言）\n"
            << "  mcp                                 MCP 服务器模式（stdio JSON-RPC 2.0，供 AI 代理接入；\n"
            << "                                      亦可用独立入口 sgmcp.exe）\n"
            << "  version                             版本信息\n"
            << "  help                                本帮助\n";
        return 0;
    }
    if (cmd == "check") return cmdCheck(rest);
    if (cmd == "interface") return cmdInterface(rest);
    if (cmd == "run") return cmdRun(rest);
    if (cmd == "build") return cmdBuild(rest);
    if (cmd == "test") return runTests(rest);
    if (cmd == "repl") {
        if (!rest.empty()) {
            std::cerr << "用法: sg repl\n";
            return 2;
        }
        return cmdRepl();
    }
    if (cmd == "mcp") {
        if (!rest.empty()) {
            std::cerr << "用法: sg mcp（stdio JSON-RPC，无命令行参数）\n";
            return 2;
        }
        return cmdMcp();
    }

    std::cerr << "未知命令 '" << cmd << "'（sg help 查看用法）\n";
    return 2;
}
