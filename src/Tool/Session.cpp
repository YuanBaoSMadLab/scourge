// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译会话实现（接口见 Tool/Session.hpp）
// ----------------------------------------------------------------------------
//  从 main.cpp 提取：会话为多命令共享基础设施（check/interface/run/REPL），
//  文件划分遵循"一模块一职责"。加载阶段串行保证 FileId 确定性；
//  检查阶段分层并行 + 主线程定序并入，输出与并发度无关。
// ============================================================================
#include "scourge/Tool/Session.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <thread>
#include <utility>

#include "scourge/Interface/SgiGen.hpp"
#include "scourge/Syntax/Lexer.hpp"
#include "scourge/Syntax/Parser.hpp"

namespace scourge {

namespace {

// ----------------------------------------------------------------------------
// 源码读取：统一换行符（词法器只处理 \n），保持位置确定性。
// 一次性按文件长度读入（避免 ostringstream 对小文件的中间缓冲拷贝）；
// 换行归一化按 \r 批量切段拼接，无 \r 时零拷贝直接采用原文。
// ----------------------------------------------------------------------------
bool readFileNormalized(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) return false;
    in.seekg(0, std::ios::beg);

    std::string raw;
    raw.resize(static_cast<size_t>(size));
    if (size > 0 && !in.read(raw.data(), size)) return false;
    if (size > 0) raw.resize(static_cast<size_t>(in.gcount()));

    // \r\n 与孤立 \r 均归一为 \n；无 \r 直接移出（常见快路径）。
    if (raw.find('\r') == std::string::npos) {
        out = std::move(raw);
        return true;
    }
    out.clear();
    out.reserve(raw.size());
    size_t i = 0;
    for (;;) {
        const size_t r = raw.find('\r', i);
        if (r == std::string::npos) {
            out.append(raw, i, std::string::npos);
            break;
        }
        out.append(raw, i, r - i);
        out.push_back('\n');
        i = (r + 1 < raw.size() && raw[r + 1] == '\n') ? r + 2 : r + 1;
    }
    return true;
}

/// 硬件并发度；未知时回退 4（保守，避免超额并发）。
unsigned detectHardwareConcurrency() {
    unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 4u : n;
}

} // namespace

// ----------------------------------------------------------------------------
// 生命周期
// ----------------------------------------------------------------------------

Session::Session() : diag_(src_) {}

// ----------------------------------------------------------------------------
// 加载（阶段一：串行 DFS）
// ----------------------------------------------------------------------------

bool Session::loadModule(const std::string& name,
                         const std::filesystem::path& path) {
    if (loaded_.count(name)) return true;

    // 循环依赖：import 必须构成静态 DAG（设计文档第五章）。
    for (const auto& s : loadStack_) {
        if (s == name) {
            std::string chain;
            for (const auto& m : loadStack_) chain += m + " -> ";
            chain += name;
            diag_.error(Errc::E5002_ImportCycle, kInvalidFile, SourceRange(),
                        "import 构成循环依赖：" + chain);
            return false;
        }
    }

    std::string text;
    if (!readFileNormalized(path, text)) {
        diag_.error(Errc::E5001_ModuleNotFound, kInvalidFile, SourceRange(),
                    "找不到模块 '" + name + "'（期望文件: " + path.string() +
                        "）");
        return false;
    }
    FileId file = src_.addFile(path.string(), std::move(text));

    Lexer lexer(src_, file, diag_);
    Parser parser(src_, file, diag_, lexer.tokenize());
    auto mod = parser.parseModule(name);
    if (!mod) return false;

    Loaded ld;
    ld.mod = std::move(mod);
    ld.path = path;
    ld.file = file;

    // 解析依赖（相对当前文件目录）。
    loadStack_.push_back(name);
    const std::filesystem::path dir = path.parent_path();
    for (const auto& stmt : ld.mod->body) {
        if (stmt->kind != Stmt::Kind::Import) continue;
        auto& im = static_cast<const Import&>(*stmt);
        if (std::find(ld.deps.begin(), ld.deps.end(), im.module) ==
            ld.deps.end()) {
            ld.deps.push_back(im.module);
        }
        loadModule(im.module, dir / (im.module + ".sg"));
    }
    loadStack_.pop_back();

    loaded_[name] = std::move(ld);
    return true;
}

bool Session::load(const std::filesystem::path& entryPath) {
    entryStem_ = entryPath.stem().string();
    return loadModule(entryStem_, entryPath);
}

bool Session::importModule(const std::string& name) {
    if (loaded_.count(name)) return true; // 幂等：同轮重复 import
    return loadModule(name,
                      std::filesystem::current_path() / (name + ".sg"));
}

// ----------------------------------------------------------------------------
// 检查（阶段二：DAG 分层并行）
// ----------------------------------------------------------------------------

/// 依依赖关系把已加载模块划分为层：第 i 层仅依赖 < i 层。
/// 层内模块互相独立 → 可安全并行检查。roots 为遍历根（单入口或活动模块图）。
std::vector<std::vector<std::string>> Session::buildLayers(
    const std::vector<std::string>& roots) const {
    // 深度计算（DAG 无环；loadModule 已拒绝环）。此处用显式栈避免递归 lambda。
    std::vector<std::string> order;
    std::vector<std::string> stack;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
        stack.push_back(*it); // 逆序压栈 → 出栈序与 roots 一致
    std::map<std::string, bool> seen;
    while (!stack.empty()) {
        std::string n = stack.back();
        stack.pop_back();
        if (seen[n]) continue;
        seen[n] = true;
        order.push_back(n);
        auto lit = loaded_.find(n);
        if (lit == loaded_.end()) continue;
        for (const auto& dep : lit->second.deps) {
            if (loaded_.count(dep) && dep != n) stack.push_back(dep);
        }
    }

    // 迭代到不动点求最长路径层号（拓扑序逆序处理即可一轮收敛，
    // 这里用简单多轮松弛，模块数规模下开销可忽略）。
    std::map<std::string, int> lvl;
    for (const auto& n : order) lvl[n] = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& n : order) {
            auto lit = loaded_.find(n);
            if (lit == loaded_.end()) continue;
            for (const auto& dep : lit->second.deps) {
                if (lvl.count(dep) && dep != n &&
                    lvl[n] < lvl[dep] + 1) {
                    lvl[n] = lvl[dep] + 1;
                    changed = true;
                }
            }
        }
    }

    std::vector<std::vector<std::string>> layers;
    for (const auto& [n, d] : lvl) {
        if (d >= static_cast<int>(layers.size()))
            layers.resize(static_cast<size_t>(d) + 1);
        layers[static_cast<size_t>(d)].push_back(n);
    }
    return layers;
}

/// 检查单个模块：持 registry 快照 → 独立 TypeChecker → 合并诊断与符号。
/// 层内各线程只读 registry、互写独立诊断缓冲，互不竞争。
void Session::checkModule(const std::string& name) {
    auto lit = loaded_.find(name);
    if (lit == loaded_.end()) return;

    // 快照（registry 只读；后续层不会改动本层依赖）。
    std::shared_ptr<const ModuleRegistry> snapshot;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        snapshot = std::make_shared<const ModuleRegistry>(registry_);
    }

    DiagEngine local(src_);
    TypeChecker checker(src_, local, *snapshot);
    auto syms = std::make_shared<ModuleSymbols>(checker.check(*lit->second.mod));

    std::lock_guard<std::mutex> lk(mutex_);
    // 暂存本模块诊断，并入主引擎推迟到本层线程全部结束之后（见 check），
    // 由主线程按确定序串行完成 → 双通道输出与并发度无关。
    moduleDiags_[name] = local.all();
    checked_[name] = syms;
    registry_[name] = syms;
}

void Session::checkImported(unsigned jobs) {
    std::vector<std::string> roots;
    roots.reserve(loaded_.size());
    for (const auto& [n, ld] : loaded_) roots.push_back(n);
    checkLayers(std::move(roots), jobs);
}

std::shared_ptr<ModuleSymbols> Session::check(unsigned jobs) {
    checkLayers({entryStem_}, jobs);
    auto it = checked_.find(entryStem_);
    return it == checked_.end() ? nullptr : it->second;
}

/// 分层并行检查核心（roots 已定）：层内工作池，层间串行，主线程定序并入。
void Session::checkLayers(std::vector<std::string> roots, unsigned jobs) {
    auto layers = buildLayers(roots);
    const unsigned hw = jobs == 0 ? detectHardwareConcurrency() : jobs;

    for (auto& layer : layers) {
        // 每层内按字典序确定提交顺序（与并发度无关，诊断顺序稳定）。
        std::sort(layer.begin(), layer.end());

        if (hw <= 1 || layer.size() <= 1) {
            for (const auto& n : layer) checkModule(n);
        } else {
            std::atomic<size_t> next{0};
            const unsigned workers =
                std::min<unsigned>(hw, static_cast<unsigned>(layer.size()));
            std::vector<std::thread> pool;
            pool.reserve(workers);
            for (unsigned w = 0; w < workers; ++w) {
                pool.emplace_back([&] {
                    for (;;) {
                        size_t i = next.fetch_add(1);
                        if (i >= layer.size()) break;
                        checkModule(layer[i]);
                    }
                });
            }
            for (auto& t : pool) t.join();
        }

        // 层内线程全部结束后，主线程按字典序并入本层诊断。
        // 并入序仅取决于模块名，与线程调度/并发度无关 → 输出逐字节确定。
        for (const auto& n : layer) {
            auto dit = moduleDiags_.find(n);
            if (dit == moduleDiags_.end()) continue;
            for (const auto& d : dit->second) diag_.adopt(d);
            moduleDiags_.erase(dit); // 及时释放，避免层间累积内存
        }
    }
}

std::shared_ptr<ModuleSymbols> Session::compile(
    const std::filesystem::path& entryPath, unsigned jobs) {
    if (!load(entryPath)) return nullptr;
    return check(jobs);
}

// ----------------------------------------------------------------------------
// 访问器
// ----------------------------------------------------------------------------

std::string Session::emitInterface(const std::string& name) {
    auto lit = loaded_.find(name);
    auto cit = checked_.find(name);
    if (lit == loaded_.end() || cit == checked_.end()) return "";
    return generateSgi(src_, *lit->second.mod, *cit->second);
}

std::vector<std::string> Session::depOrder(
    const std::vector<std::string>& roots) const {
    // buildLayers 的层内迭代来自 std::map（字典序），层间为依赖序。
    std::vector<std::string> out;
    for (const auto& layer : buildLayers(roots))
        for (const auto& n : layer) out.push_back(n);
    return out;
}

std::vector<std::string> Session::modulesInDependencyOrder(
    const std::string& entry) const {
    return depOrder({entry});
}

std::vector<std::string> Session::allModulesInDependencyOrder() const {
    std::vector<std::string> roots;
    roots.reserve(loaded_.size());
    for (const auto& [n, ld] : loaded_) roots.push_back(n);
    return depOrder(roots);
}

const Module* Session::loadedModule(const std::string& name) const {
    auto it = loaded_.find(name);
    return it == loaded_.end() ? nullptr : it->second.mod.get();
}

std::string Session::loadedModulePath(const std::string& name) const {
    auto it = loaded_.find(name);
    return it == loaded_.end() ? std::string() : it->second.path.string();
}

FileId Session::loadedFileId(const std::string& name) const {
    auto it = loaded_.find(name);
    return it == loaded_.end() ? kInvalidFile : it->second.file;
}

std::string Session::loadedSource(const std::string& name) const {
    auto it = loaded_.find(name);
    return it == loaded_.end() ? std::string() : src_.file(it->second.file).text;
}

} // namespace scourge
