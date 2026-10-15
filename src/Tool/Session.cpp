// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译会话实现（接口见 Tool/Session.hpp）
// ----------------------------------------------------------------------------
//  从 main.cpp 提取：会话为多命令共享基础设施（check/interface/run/REPL），
//  文件划分遵循"一模块一职责"。加载阶段串行保证 FileId 确定性；
//  检查阶段分层并行 + 主线程定序并入，输出与并发度无关。
//  源码读取统一走 readFileNormalized：编码自适应（BOM/UTF-8/ANSI）+
//  换行归一化，任意主机/编辑器保存的源文件零配置可编译。
// ============================================================================
#include "scourge/Tool/Session.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "scourge/Interface/SgiGen.hpp"
#include "scourge/Syntax/Lexer.hpp"
#include "scourge/Syntax/Parser.hpp"

namespace scourge {

namespace {

#ifdef _WIN32
// ----------------------------------------------------------------------------
// 源码编码自适应（零配置，面向全球机型）。裁决顺序：
//   1. BOM 显式声明（UTF-8 / UTF-16 LE / UTF-16 BE）→ 按声明解码；
//   2. 无 BOM → 严格 UTF-8 校验（MB_ERR_INVALID_CHARS），有效即原样采用
//      （生态事实标准，不做无谓转码，行号/列号按字节保持稳定）；
//   3. 校验失败 → 按系统 ANSI 代码页（GBK/Shift-JIS/Latin-1 等）重解为
//      UTF-8，兼容记事本"ANSI"保存；
//   4. 两者皆败 → 原样保留，交由词法器按字节报错定位。
// 顺序纪律：必须先转码再归一化换行（UTF-16 的 \r\n 是 0D 00 0A 00）。
// ----------------------------------------------------------------------------
void u16ToUtf8(const std::wstring& s, std::string& out) {
    out.clear();
    out.reserve(s.size() * 3);
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned cp = static_cast<unsigned short>(s[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() &&
            static_cast<unsigned short>(s[i + 1]) >= 0xDC00 &&
            static_cast<unsigned short>(s[i + 1]) <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) +
                 (static_cast<unsigned short>(s[i + 1]) - 0xDC00);
            ++i; // 高低位代理对合成增补平面码点
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD; // 孤立代理 → 替换字符
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
}

/// 上述裁决的入口。超大文件跳过检测（Win32 API 入参为 int）。
void decodeSourceBytes(const std::string& raw, std::string& out) {
    if (raw.size() > 0x7FFFFFF0u) {
        out = raw;
        return;
    }
    // BOM 显式识别。
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
        out = raw.substr(3);
        return;
    }
    if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF &&
        static_cast<unsigned char>(raw[1]) == 0xFE) {
        std::wstring w;
        w.reserve(raw.size() / 2);
        for (size_t i = 2; i + 1 < raw.size(); i += 2)
            w.push_back(static_cast<wchar_t>(
                static_cast<unsigned char>(raw[i]) |
                (static_cast<unsigned char>(raw[i + 1]) << 8)));
        if ((raw.size() - 2) % 2 != 0) w.push_back(L'\uFFFD'); // 尾部半码元
        u16ToUtf8(w, out);
        return;
    }
    if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFE &&
        static_cast<unsigned char>(raw[1]) == 0xFF) {
        std::wstring w;
        w.reserve(raw.size() / 2);
        for (size_t i = 2; i + 1 < raw.size(); i += 2)
            w.push_back(static_cast<wchar_t>(
                (static_cast<unsigned char>(raw[i]) << 8) |
                static_cast<unsigned char>(raw[i + 1])));
        if ((raw.size() - 2) % 2 != 0) w.push_back(L'\uFFFD');
        u16ToUtf8(w, out);
        return;
    }

    // 无 BOM：严格 UTF-8 校验，有效即原样（常见快路径）。
    if (!raw.empty()) {
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          raw.data(),
                                          static_cast<int>(raw.size()),
                                          nullptr, 0);
        if (n > 0) {
            out = raw;
            return;
        }
        // ANSI 代码页 → UTF-16 → UTF-8。
        const UINT acp = GetACP();
        const int wlen = MultiByteToWideChar(acp, 0, raw.data(),
                                             static_cast<int>(raw.size()),
                                             nullptr, 0);
        if (wlen > 0) {
            std::wstring w(static_cast<size_t>(wlen), L'\0');
            if (MultiByteToWideChar(acp, 0, raw.data(),
                                    static_cast<int>(raw.size()), &w[0],
                                    wlen) == wlen) {
                u16ToUtf8(w, out);
                return;
            }
        }
    }
    out = raw;
}
#endif // _WIN32

// ----------------------------------------------------------------------------
// 源码读取：编码自适应（Windows，见 decodeSourceBytes）+ 统一换行符
//（词法器只处理 \n），保持位置确定性。一次性按文件长度读入（避免
// ostringstream 对小文件的中间缓冲拷贝）；换行归一化按 \r 批量切段拼接，
// 无 \r 时零拷贝直接采用原文。
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

#ifdef _WIN32
    std::string text;
    decodeSourceBytes(raw, text);
#else
    std::string text = std::move(raw);
#endif

    // \r\n 与孤立 \r 均归一为 \n；无 \r 直接移出（常见快路径）。
    if (text.find('\r') == std::string::npos) {
        out = std::move(text);
        return true;
    }
    out.clear();
    out.reserve(text.size());
    size_t i = 0;
    for (;;) {
        const size_t r = text.find('\r', i);
        if (r == std::string::npos) {
            out.append(text, i, std::string::npos);
            break;
        }
        out.append(text, i, r - i);
        out.push_back('\n');
        i = (r + 1 < text.size() && text[r + 1] == '\n') ? r + 2 : r + 1;
    }
    return true;
}

/// 硬件并发度；未知时回退 4（保守，避免超额并发）。
unsigned detectHardwareConcurrency() {
    unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 4u : n;
}

// ----------------------------------------------------------------------------
// 模块路径解析（0.9.6 L 节：包/子模块 + 最小本地 registry）
// ----------------------------------------------------------------------------

std::vector<std::string> splitDotted(const std::string& name) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = name.find('.', start);
        if (p == std::string::npos) {
            out.push_back(name.substr(start));
            break;
        }
        out.push_back(name.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

std::string joinDotted(const std::vector<std::string>& segs, size_t begin,
                       size_t end) {
    std::string out;
    for (size_t i = begin; i < end && i < segs.size(); ++i) {
        if (!out.empty()) out += '.';
        out += segs[i];
    }
    return out;
}

/// 数值感知版本比较：全数字段按整数，否则按字典序；缺段视为 0。
/// 保证 1.10 > 1.9；仅用于本地包缓存目录择优（无 semver 约束解析）。
bool verLess(const std::string& a, const std::string& b) {
    std::vector<std::string> sa = splitDotted(a), sb = splitDotted(b);
    auto numeric = [](const std::string& s) {
        return !s.empty() &&
               s.find_first_not_of("0123456789") == std::string::npos;
    };
    for (size_t k = 0; k < std::max(sa.size(), sb.size()); ++k) {
        std::string x = k < sa.size() ? sa[k] : "0";
        std::string y = k < sb.size() ? sb[k] : "0";
        if (numeric(x) && numeric(y)) {
            long long vx = std::stoll(x), vy = std::stoll(y);
            if (vx != vy) return vx < vy;
        } else if (x != y) {
            return x < y;
        }
    }
    return false;
}

/// import 模块文件候选解析（按序取第一个存在者）：
///   1. 相对当前文件目录（包目录 = 子目录，a.b → <dir>/a/b.sg）
///   2. 相对 CWD
///   3. 本地包缓存（SCOURGE_PACKAGES，默认 <CWD>/packages/）：
///      <root>/<pkg>@<ver>/…（ver 取数值感知最大）→ <root>/<pkg>/… 兜底
/// 找不到返回空路径；tried 非空时收集全部候选供 E5001 诊断列出。
std::filesystem::path resolveModulePath(
    const std::filesystem::path& baseDir, const std::vector<std::string>& path,
    std::vector<std::string>* tried) {
    auto note = [&](const std::filesystem::path& p) {
        std::error_code ec;
        if (tried) tried->push_back(p.string());
        return !p.empty() && std::filesystem::exists(p, ec);
    };
    // 前段为子目录，末段为文件 stem。
    std::filesystem::path rel;
    for (size_t i = 0; i + 1 < path.size(); ++i) rel /= path[i];
    const std::string leaf = path.back() + ".sg";

    std::filesystem::path c1 = baseDir / rel / leaf;
    if (note(c1)) return c1;
    std::filesystem::path cwd = std::filesystem::current_path();
    std::filesystem::path c2 = cwd / rel / leaf;
    if (!(c2 == c1) && note(c2)) return c2;

    const char* envPkgs = std::getenv("SCOURGE_PACKAGES");
    std::filesystem::path root =
        (envPkgs && *envPkgs) ? std::filesystem::path(envPkgs)
                              : cwd / "packages";
    std::error_code ec;
    if (std::filesystem::exists(root, ec)) {
        const std::string& pkg = path[0];
        std::filesystem::path sub;
        for (size_t i = 1; i + 1 < path.size(); ++i) sub /= path[i];
        std::string best; // 最大版本后缀（不含 @）
        for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
            std::error_code ec2;
            if (!e.is_directory(ec2)) continue;
            const std::string n = e.path().filename().string();
            if (n.rfind(pkg + "@", 0) != 0) continue;
            std::string ver = n.substr(pkg.size() + 1);
            if (best.empty() || verLess(best, ver)) best = ver;
        }
        if (!best.empty()) {
            std::filesystem::path c3 = root / (pkg + "@" + best) / sub / leaf;
            if (note(c3)) return c3;
        }
        std::filesystem::path c4 = root / pkg / sub / leaf;
        if (note(c4)) return c4;
    }
    return {};
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

    // 解析依赖（相对当前文件目录 → CWD → 本地包缓存；0.9.6 点分路径）。
    loadStack_.push_back(name);
    const std::filesystem::path dir = path.parent_path();
    for (const auto& stmt : ld.mod->body) {
        if (stmt->kind != Stmt::Kind::Import) continue;
        auto& im = static_cast<const Import&>(*stmt);
        auto addDep = [&](const std::string& dep) {
            if (std::find(ld.deps.begin(), ld.deps.end(), dep) ==
                ld.deps.end()) {
                ld.deps.push_back(dep);
            }
        };
        std::vector<std::string> tried;
        std::filesystem::path dep =
            resolveModulePath(dir, im.path, &tried);
        if (dep.empty()) {
            std::string list;
            for (size_t i = 0; i < tried.size(); ++i) {
                if (i) list += "；";
                list += tried[i];
            }
            diag_.error(Errc::E5001_ModuleNotFound, kInvalidFile, SourceRange(),
                        "找不到模块 '" + im.module() + "'（被 '" + name +
                            "' 导入；尝试过: " + list + "）");
            loadStack_.pop_back();
            return false;
        }
        addDep(im.module());
        loadModule(im.module(), dep);

        // from a import b：b 可能是子模块（a/b.sg）——磁盘探测存在则一并
        // 入图，保证分层检查时全名 "a.b" 先于使用方进入 registry（Sema 的
        // 子模块绑定查 imports_，依赖缺失会误报成员不存在）。
        if (im.isFrom) {
            for (const auto& item : im.items) {
                std::vector<std::string> sub = im.path;
                sub.push_back(item.name);
                std::filesystem::path sp = resolveModulePath(dir, sub, nullptr);
                if (sp.empty()) continue;
                std::string full = joinDotted(sub, 0, sub.size());
                addDep(full);
                loadModule(full, sp);
            }
        }
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
    // 0.9.6：点分名走统一候选解析（相对 CWD → 本地包缓存）。
    std::vector<std::string> tried;
    std::filesystem::path p = resolveModulePath(
        std::filesystem::current_path(), splitDotted(name), &tried);
    if (p.empty()) {
        std::string list;
        for (size_t i = 0; i < tried.size(); ++i) {
            if (i) list += "；";
            list += tried[i];
        }
        diag_.error(Errc::E5001_ModuleNotFound, kInvalidFile, SourceRange(),
                    "找不到模块 '" + name + "'（尝试过: " + list + "）");
        return false;
    }
    return loadModule(name, p);
}

bool Session::loadAs(const std::string& name,
                     const std::filesystem::path& path) {
    return loadModule(name, path);
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
    ExprTypeMap localTypes = checker.takeExprTypes();
    std::unordered_set<const Expr*> localPromotes = checker.takePromoteF64();

    std::lock_guard<std::mutex> lk(mutex_);
    // 暂存本模块诊断，并入主引擎推迟到本层线程全部结束之后（见 check），
    // 由主线程按确定序串行完成 → 双通道输出与并发度无关。
    moduleDiags_[name] = local.all();
    checked_[name] = syms;
    registry_[name] = syms;
    // 表达式类型注解并入主表（AST 指针跨模块不冲突；mutex 内定序合并，
    // 与并发度无关）——AOT 后端查表消费的单一事实源。
    exprTypes_.insert(localTypes.begin(), localTypes.end());
    promoteF64_.merge(localPromotes);
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
