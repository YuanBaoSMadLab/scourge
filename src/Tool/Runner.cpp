// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 工具链 — run 执行核心实现
// ----------------------------------------------------------------------------
//  与 CLI 命令的纪律一致：诊断失败文本一律入 outcome.log（人类通道），
//  print 输出走 VM out 回调——程序输出与失败文本两条通道物理分离。
// ============================================================================
#include "scourge/Tool/Runner.hpp"

#include <fstream>
#include <sstream>
#include <utility>

#include "scourge/BC/Compiler.hpp"
#include "scourge/BC/Serialize.hpp"
#include "scourge/BC/VM.hpp"
#include "scourge/Common/Version.hpp"
#include "scourge/Tool/Session.hpp"

namespace scourge {

namespace {

/// 二进制读取（.sgbc 缓存）：不做换行归一化——字节流必须逐字节保真
///（readFileNormalized 会改写数据中的 \r，仅适用于源码文本）。
bool readBinaryFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

RunOutcome runOnce(const std::filesystem::path& inPath, bool useCache,
                   bool captureOut) {
    RunOutcome outcome;
    Session session;
    std::string entry = inPath.stem().string();

    // 阶段一：加载 + 解析（缓存指纹需要模块集与源文本）。
    if (!session.load(inPath)) {
        std::string text;
        session.diag().emitText(text);
        outcome.log += text;
        outcome.log += "run 中止：模块加载或解析失败\n";
        outcome.exitCode = 1;
        return outcome;
    }

    auto order = session.modulesInDependencyOrder(entry);
    outcome.modulePaths.reserve(order.size());
    for (const auto& name : order)
        outcome.modulePaths.push_back(session.loadedModulePath(name));

    // panic 定位用模块→路径映射（缓存命中时以缓存记录为准）。
    std::vector<std::string> panicPaths;
    panicPaths.reserve(order.size());
    for (const auto& name : order)
        panicPaths.push_back(session.loadedModulePath(name));

    // 源指纹：工具链标签 + 依赖层序逐模块（模块名、路径、归一化源文本）。
    // 指纹在检查之前计算——命中即可跳过检查与字节码编译（增量收益主体）。
    uint64_t hash = bc::fnv1a64(kScourgeVersionLabel);
    for (const auto& name : order) {
        hash = bc::fnv1a64(name, hash);
        hash = bc::fnv1a64(session.loadedModulePath(name), hash);
        hash = bc::fnv1a64(session.loadedSource(name), hash);
    }

    bc::Program program;
    bool fromCache = false;
    if (useCache) {
        std::filesystem::path cachePath =
            inPath.parent_path() / (entry + ".sgbc");
        std::string bytes;
        if (readBinaryFile(cachePath, bytes)) {
            bc::CacheMeta meta;
            // 全量校验 + 五重一致性：格式 / 工具链 / 指纹 / 入口 / 模块集。
            // 任一不符 → 视为未命中，全量重编（显式拒绝，无静默回退）。
            if (bc::decodeProgram(bytes, program, meta) &&
                meta.toolchain == std::string(kScourgeVersionLabel) &&
                meta.sourceHash == hash && meta.entry == entry &&
                meta.modules == order) {
                fromCache = true;
                panicPaths = std::move(meta.paths);
            } else {
                program = bc::Program{}; // 拒绝后恢复干净状态
            }
        }
    }

    if (!fromCache) {
        // 阶段二：全量检查（缓存未命中才付这笔开销）。
        session.check();
        if (session.diag().hasErrors()) {
            std::string text;
            session.diag().emitText(text);
            outcome.log += text;
            outcome.log += "run 中止：模块未通过全量验证\n";
            outcome.exitCode = 1;
            return outcome;
        }

        // 字节码编译：按依赖层序逐模块编译进同一 Program
        //（跨模块函数经占位复用/补全，见 Compiler::compileModule）。
        bc::Compiler compiler(session.src(), session.diag(),
                              session.registry());
        for (const auto& name : order) {
            const Module* m = session.loadedModule(name);
            if (!m) continue;
            if (!compiler.compileModule(*m)) {
                std::string text;
                session.diag().emitText(text);
                outcome.log += text;
                outcome.log += "run 中止：模块 '" + name +
                               "' 含字节码后端暂不支持的构造\n";
                outcome.exitCode = 1;
                return outcome;
            }
        }
        program = compiler.take();

        // 写缓存（加速项而非正确性依赖：写入失败静默跳过）。
        bc::CacheMeta meta;
        meta.toolchain = std::string(kScourgeVersionLabel);
        meta.entry = entry;
        meta.modules = order;
        meta.paths = panicPaths;
        meta.sourceHash = hash;
        std::ofstream of(inPath.parent_path() / (entry + ".sgbc"),
                         std::ios::binary);
        if (of) of << bc::encodeProgram(program, meta);
    }

    bc::VMOptions vo;
    if (captureOut)
        vo.out = [&outcome](const std::string& s) { outcome.output += s; };
    bc::VM vm(std::move(program), vo);
    if (!vm.run(order, entry)) {
        const bc::Panic& p = vm.lastPanic();
        const std::string& pm = p.module.empty() ? entry : p.module;
        std::string file;
        for (size_t i = 0; i < order.size() && i < panicPaths.size(); ++i) {
            if (order[i] == pm) {
                file = panicPaths[i];
                break;
            }
        }
        if (file.empty()) file = "<unknown>";
        outcome.log += "panic: " + p.message + " at " + p.func + " (" + file +
                       ":" + std::to_string(p.line) + ")\n";
        outcome.exitCode = 1;
        return outcome;
    }
    outcome.exitCode = vm.exitCode();
    return outcome;
}

} // namespace scourge
