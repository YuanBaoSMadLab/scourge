// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 测试框架实现（接口见 Tool/TestRunner.hpp）
// ----------------------------------------------------------------------------
//  轮次模型（与 REPL 同构，诊断/符号随轮销毁，无跨轮污染）：
//    * 校验轮：目标模块 load + checkImported，0 错误才继续；
//    * 显式轮：fresh Session 装载 <stem>.sgt（派生名 <stem>__sgt），
//      合成 sgt_driver 驱动逐个调用 test_*（每测试独立 try/except 隔离）；
//    * 属性轮：每个 @pure+契约函数一轮 fresh Session——驱动模块在内存合成
//      （_sgt_check 前置跳过 / panic 失败 / ensures 违反失败 / @pure 差分），
//      print "#TEST ..." 协议行由 C++ 侧逐例聚合，反例 = 首个非 pass 例。
//
//  协议形态（C++ 侧只认 "#TEST " 前缀行，被测函数自身 print 一律忽略）：
//    #TEST pass                属性例通过
//    #TEST skip                前置不满足，跳过
//    #TEST fail panic          前置满足后调用 panic
//    #TEST fail ensures        ensures 违反
//    #TEST fail pure           @pure 差分不一致
//    #TEST error               契约表达式求值 panic（驱动层捕获）
//    #TEST pass <name>         显式测试通过（fail/panic 同理带名）
//
//  纪律：
//    * 契约文本自源码 range 还原（Parser::attachContract 保证 range 覆盖
//      表达式原文），跨模块引用经 qualifyContractText 限定——被测模块已过
//      全量检查，适配后的驱动必然可解析（适配缺陷会以编译诊断显式暴露）；
//    * 输入生成确定性：splitmix64 流，边界池优先、随机填充；
//    * 不可生成参数类型（Result/Option/Dict/动态形状等）整函数跳过并注明
//      原因，绝不静默。
// ============================================================================
#include "scourge/Tool/TestRunner.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <utility>

#include "scourge/BC/Compiler.hpp"
#include "scourge/BC/Serialize.hpp"
#include "scourge/BC/VM.hpp"
#include "scourge/Common/Capability.hpp"
#include "scourge/Common/JsonWriter.hpp"
#include "scourge/Syntax/Lexer.hpp"
#include "scourge/Syntax/Parser.hpp"
#include "scourge/Tool/Session.hpp"

namespace fs = std::filesystem;

namespace scourge {

namespace sgt {

// ----------------------------------------------------------------------------
// 纯助手（单测覆盖：tests/test_runner.cpp）
// ----------------------------------------------------------------------------

Rng::Rng(const std::string& derived)
    : state(bc::fnv1a64(derived)) {}
// 注意：此处实参为 std::string → 精确匹配 fnv1a64(string_view) 重载。
// 切勿传入 const char*（会命中 (const void*, len) 重载，seed 被当 len 野读）。

std::string renderFloat(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    std::string s = buf;
    const size_t dot = s.find('.');
    if (dot != std::string::npos) {
        size_t last = s.size() - 1;
        while (last > dot + 1 && s[last] == '0') --last;
        s.resize(last + 1);
    }
    return s;
}

std::string renderString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

namespace {

bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

} // namespace

std::string qualifyContractText(const std::string& text,
                                const std::string& modName,
                                const std::map<std::string, bool>& locals) {
    std::string out;
    out.reserve(text.size() + modName.size() * 4);
    bool inStr = false;
    char prevNonSpace = 0; ///< 上一个已输出的非空白字符（成员访问判定）
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (inStr) {
            out += c;
            if (c == '\\' && i + 1 < text.size()) out += text[++i];
            else if (c == '"') inStr = false;
            ++i;
            continue;
        }
        if (c == '"') {
            inStr = true;
            out += c;
            ++i;
            continue;
        }
        if (isIdentStart(c)) {
            size_t j = i;
            while (j < text.size() && isIdentChar(text[j])) ++j;
            const std::string ident = text.substr(i, j - i);
            const bool afterDot = prevNonSpace == '.';
            if (!afterDot && ident == "result") {
                out += "_sgt_r";
            } else if (!afterDot && locals.count(ident)) {
                out += modName + "." + ident;
            } else {
                out += ident;
            }
            prevNonSpace = ident.back();
            i = j;
            continue;
        }
        out += c;
        if (!std::isspace(static_cast<unsigned char>(c))) prevNonSpace = c;
        ++i;
    }
    return out;
}

} // namespace sgt

namespace {

// ----------------------------------------------------------------------------
// 计划与结果结构
// ----------------------------------------------------------------------------

/// 属性测试计划（参数类型/契约文本自当轮 Session 派生）。
struct PropPlan {
    std::string func;
    std::vector<std::string> paramNames;
    std::vector<TypePtr> paramTypes;
    std::vector<std::string> preClauses;  ///< requires + invariant(无 result)
    std::vector<std::string> postClauses; ///< ensures + invariant(含 result)
    bool differential = false;            ///< @pure 差分（返回 Int/Bool/Str）
    bool runnable = false;
    std::string skipReason;
};

struct TestResult {
    std::string name;
    std::string status; ///< pass / fail / panic / skip / error
    std::string detail; ///< 失败原因或跳过原因
};

struct PropResult {
    std::string func;
    std::string status; ///< pass / fail / skip / error
    std::string skipReason;
    std::string detail; ///< 编译诊断 / 执行中断文本
    int64_t runs = 0;
    int64_t passed = 0;
    int64_t failed = 0;
    int64_t skipped = 0;
    int64_t seed = 0;
    std::vector<std::string> ceArgs; ///< 反例实参（已渲染字面量）
    std::string ceReason;            ///< 反例成因
};

/// 逗号连接（驱动调用实参 / 反例展示共用）。
std::string joinArgs(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += ", ";
        out += parts[i];
    }
    return out;
}

// ----------------------------------------------------------------------------
// 类型工具
// ----------------------------------------------------------------------------

/// 参数类型可生成性：Int/Float/Bool/Str 直接生成；List 递归；Tensor 须
/// 静态形状且秩 ≤ 2；其余类型（Result/Option/Dict/动态形状等）显式跳过。
bool genableType(const TypePtr& t, std::string& why) {
    switch (t->kind) {
    case Type::Kind::Int:
    case Type::Kind::Float:
    case Type::Kind::Bool:
    case Type::Kind::Str:
        return true;
    case Type::Kind::List:
        return genableType(t->params[0], why);
    case Type::Kind::Tensor: {
        if (t->shape.size() > 2) {
            why = "张量秩 > 2 暂不生成输入";
            return false;
        }
        for (int64_t d : t->shape) {
            if (d == kDyn) {
                why = "张量含动态维（?），形状未知";
                return false;
            }
        }
        return genableType(t->params[0], why);
    }
    default:
        why = "类型 " + typeToString(t) + " 暂不生成输入";
        return false;
    }
}

/// 类型注解渲染（与 Parser::parseTypeAnnotation 语法对齐；仅覆盖
/// genableType 放行的类型——不可生成类型在此前已被跳过）。
std::string renderAnnotation(const TypePtr& t) {
    switch (t->kind) {
    case Type::Kind::Int: return "int";
    case Type::Kind::Float: return "float";
    case Type::Kind::Bool: return "bool";
    case Type::Kind::Str: return "str";
    case Type::Kind::List: return "List[" + renderAnnotation(t->params[0]) + "]";
    case Type::Kind::Tensor: {
        std::string dims;
        for (size_t i = 0; i < t->shape.size(); ++i) {
            if (i) dims += ", ";
            dims += std::to_string(t->shape[i]);
        }
        return "Tensor[" + renderAnnotation(t->params[0]) + ", (" + dims + ")]";
    }
    default: return "None"; // 不可达：plan 已过滤；占位保编译
    }
}

/// 契约表达式是否引用 result（invariant 分类：引用 → 后置，否则前置）。
bool exprRefsResult(const Expr* e) {
    if (!e) return false;
    switch (e->kind) {
    case Expr::Kind::Name:
        return static_cast<const Name*>(e)->name == "result";
    case Expr::Kind::Unary:
        return exprRefsResult(static_cast<const Unary*>(e)->operand.get());
    case Expr::Kind::Binary: {
        auto* b = static_cast<const Binary*>(e);
        return exprRefsResult(b->lhs.get()) || exprRefsResult(b->rhs.get());
    }
    case Expr::Kind::Ternary: {
        auto* t = static_cast<const Ternary*>(e);
        return exprRefsResult(t->cond.get()) || exprRefsResult(t->thenE.get()) ||
               exprRefsResult(t->elseE.get());
    }
    case Expr::Kind::Call: {
        auto* c = static_cast<const Call*>(e);
        if (exprRefsResult(c->callee.get())) return true;
        for (const auto& a : c->args)
            if (exprRefsResult(a.get())) return true;
        return false;
    }
    case Expr::Kind::Index: {
        auto* ix = static_cast<const Index*>(e);
        if (exprRefsResult(ix->base.get())) return true;
        for (const auto& i : ix->indices)
            if (exprRefsResult(i.get())) return true;
        return false;
    }
    case Expr::Kind::Attr:
        return exprRefsResult(static_cast<const Attr*>(e)->base.get());
    case Expr::Kind::ListLit:
        for (const auto& el : static_cast<const ListLit*>(e)->elems)
            if (exprRefsResult(el.get())) return true;
        return false;
    default:
        return false;
    }
}

// ----------------------------------------------------------------------------
// 输入生成（边界池优先 + 随机填充；splitmix64 流，确定性）
// ----------------------------------------------------------------------------

std::string genValue(const TypePtr& t, sgt::Rng& rng, size_t runIdx);

/// 张量字面量：按静态形状逐维嵌套（秩 ≤ 2 已由 plan 保证）。
std::string genTensor(const TypePtr& t, sgt::Rng& rng, size_t runIdx,
                      size_t dim) {
    if (dim + 1 == t->shape.size()) {
        std::string out = "[";
        for (int64_t i = 0; i < t->shape[dim]; ++i) {
            if (i) out += ", ";
            out += genValue(t->params[0], rng, runIdx);
        }
        return out + "]";
    }
    std::string out = "[";
    for (int64_t i = 0; i < t->shape[dim]; ++i) {
        if (i) out += ", ";
        out += genTensor(t, rng, runIdx, dim + 1);
    }
    return out + "]";
}

std::string genValue(const TypePtr& t, sgt::Rng& rng, size_t runIdx) {
    switch (t->kind) {
    case Type::Kind::Int: {
        // 边界池（覆盖 0/±1/±2/±3/±10/±1e6/±2^40），随后 [-1e9, 1e9] 随机。
        static const int64_t kPool[] = {0,       1,        -1,     2,
                                        -2,      3,        -3,     10,
                                        -10,     1000000,  -1000000,
                                        1099511627776LL, -1099511627776LL};
        constexpr size_t kPoolN = sizeof(kPool) / sizeof(kPool[0]);
        if (runIdx < kPoolN) return std::to_string(kPool[runIdx]);
        return std::to_string(rng.below(-1000000000LL, 1000000000LL));
    }
    case Type::Kind::Float: {
        // 定点网格（无 NaN/inf），随后 [-1000, 1000] 步长 0.001 随机。
        static const double kPool[] = {0.0,  1.0,   -1.0,   0.5,   -0.5,
                                       2.5,  100.0, -100.0, 0.001};
        constexpr size_t kPoolN = sizeof(kPool) / sizeof(kPool[0]);
        if (runIdx < kPoolN) return sgt::renderFloat(kPool[runIdx]);
        return sgt::renderFloat(
            static_cast<double>(rng.below(-1000000LL, 1000000LL)) / 1000.0);
    }
    case Type::Kind::Bool:
        return (runIdx % 2 == 0) ? "True" : "False";
    case Type::Kind::Str: {
        if (runIdx == 0) return "\"\""; // 空串边界
        static const char kAlphabet[] = "abcdef ";
        std::string s;
        const int64_t n = rng.below(0, 8);
        for (int64_t i = 0; i < n; ++i) s += kAlphabet[rng.below(0, 7)];
        return sgt::renderString(s);
    }
    case Type::Kind::List: {
        if (runIdx == 0) return "[]"; // 空表边界
        std::string out = "[";
        const int64_t n = rng.below(0, 4);
        for (int64_t i = 0; i < n; ++i) {
            if (i) out += ", ";
            out += genValue(t->params[0], rng, runIdx);
        }
        return out + "]";
    }
    case Type::Kind::Tensor:
        return genTensor(t, rng, runIdx, 0);
    default:
        return "None"; // 不可达：plan 已过滤
    }
}

// ----------------------------------------------------------------------------
// 计划构建（当轮 Session 内派生；契约文本自源码 range 还原）
// ----------------------------------------------------------------------------

/// 从已检查的模块符号构建一个属性的执行计划。
/// 不可生成参数（runnable=false）时填 skipReason，绝不静默。
PropPlan buildPlan(Session& session, const std::string& modName,
                   const FuncSig& sig) {
    PropPlan p;
    p.func = sig.name;
    if (!sig.def) {
        p.skipReason = "无函数定义（内建签名）";
        return p;
    }

    // 契约文本自源码还原（attachContract 保证 range 覆盖表达式原文），
    // 跨模块引用限定为 模块名.名字，result 绑定驱动局部 _sgt_r。
    const FileId file = session.loadedFileId(modName);
    const std::string& text = session.src().file(file).text;
    std::map<std::string, bool> locals;
    const auto& syms = *session.registry().at(modName);
    for (const auto& [n, f] : syms.funcs) {
        (void)f;
        locals[n] = true;
    }
    for (const auto& [n, t] : syms.globals) {
        (void)t;
        locals[n] = true;
    }

    for (const auto& c : sig.def->contracts) {
        std::string raw = text.substr(c.range.offset, c.range.length);
        std::string adapted = sgt::qualifyContractText(raw, modName, locals);
        const bool post = c.kind == ContractClause::Kind::Ensures ||
                          (c.kind == ContractClause::Kind::Invariant &&
                           exprRefsResult(c.expr.get()));
        (post ? p.postClauses : p.preClauses).push_back(std::move(adapted));
    }

    for (size_t i = 0; i < sig.params.size(); ++i) {
        p.paramNames.push_back(sig.def->params[i].name);
        p.paramTypes.push_back(sig.params[i]);
        std::string why;
        if (!genableType(sig.params[i], why)) {
            p.skipReason = "参数 '" + sig.def->params[i].name + "'：" + why;
            return p;
        }
    }

    // @pure 差分仅对可稳定比较的返回类型启用（float 有 NaN 语义分歧，
    // Result/Option/容器等无全等内建——均不启用，避免假阳性）。
    p.differential = sig.ret && (sig.ret->kind == Type::Kind::Int ||
                                 sig.ret->kind == Type::Kind::Bool ||
                                 sig.ret->kind == Type::Kind::Str);
    p.runnable = true;
    return p;
}

// ----------------------------------------------------------------------------
// 驱动合成
// ----------------------------------------------------------------------------

/// and 连接的括号合取；无子句返回空串。
std::string joinClauses(const std::vector<std::string>& clauses) {
    if (clauses.empty()) return "";
    std::string out = "(" + clauses[0] + ")";
    for (size_t i = 1; i < clauses.size(); ++i)
        out += " and (" + clauses[i] + ")";
    return out;
}

/// 属性驱动：_sgt_check（前置跳过 / panic 失败 / ensures 违反失败 / @pure
/// 差分）+ __sgt__ 入口逐例调用。每个输入恰好输出一行协议（协议序 =
/// 实参序），C++ 侧按下标聚合反例。
std::string synthesizePropertyDriver(
    const PropPlan& p, const std::string& modName,
    const std::vector<std::vector<std::string>>& tuples) {
    std::ostringstream o;
    o << "import " << modName << "\n\n";

    const std::string args = joinArgs(p.paramNames);
    const std::string pre = joinClauses(p.preClauses);
    const std::string post = joinClauses(p.postClauses);

    o << "def _sgt_check(";
    for (size_t i = 0; i < p.paramNames.size(); ++i) {
        if (i) o << ", ";
        o << p.paramNames[i] << ": " << renderAnnotation(p.paramTypes[i]);
    }
    o << ") -> None:\n";
    if (!pre.empty()) {
        o << "    if not (" << pre << "):\n"
          << "        print(\"#TEST skip\")\n"
          << "        return\n";
    }
    o << "    try:\n"
      << "        _sgt_r = " << modName << "." << p.func << "(" << args
      << ")\n"
      << "    except:\n"
      << "        print(\"#TEST fail panic\")\n"
      << "        return\n";
    if (!post.empty()) {
        o << "    if not (" << post << "):\n"
          << "        print(\"#TEST fail ensures\")\n"
          << "        return\n";
    }
    if (p.differential) {
        o << "    try:\n"
          << "        _sgt_r2 = " << modName << "." << p.func << "(" << args
          << ")\n"
          << "        if _sgt_r2 != _sgt_r:\n"
          << "            print(\"#TEST fail pure\")\n"
          << "            return\n"
          << "    except:\n"
          << "        print(\"#TEST fail pure\")\n"
          << "        return\n";
    }
    o << "    print(\"#TEST pass\")\n\n";

    // 入口：逐例调用；每例独立 try/except——契约表达式自身的 panic 也
    // 显式化（#TEST error），保证协议行与实参 1:1 对齐。
    o << "def __sgt__() -> None:\n";
    for (const auto& t : tuples) {
        o << "    try:\n"
          << "        _sgt_check(" << joinArgs(t) << ")\n"
          << "    except:\n"
          << "        print(\"#TEST error\")\n";
    }
    return o.str();
}

/// 显式测试驱动：逐个调用 sgt 模块的 test_*（bool 返回），panic 与
/// 返回 False 分开上报，互不中断。
std::string synthesizeTestDriver(const std::string& sgtName,
                                 const std::vector<std::string>& testNames) {
    std::ostringstream o;
    o << "import " << sgtName << "\n\ndef __sgt__() -> None:\n";
    for (const auto& n : testNames) {
        o << "    try:\n"
          << "        if " << sgtName << "." << n << "():\n"
          << "            print(\"#TEST pass " << n << "\")\n"
          << "        else:\n"
          << "            print(\"#TEST fail " << n << "\")\n"
          << "    except:\n"
          << "        print(\"#TEST panic " << n << "\")\n";
    }
    return o.str();
}

// ----------------------------------------------------------------------------
// 驱动执行（REPL 同构：解析 → 检查 → 字节码 → VM，协议行捕获）
// ----------------------------------------------------------------------------

struct DriverRun {
    bool ok = false;       // 编译 + 执行全程无诊断/未捕获 panic
    std::string diags;     // 编译期诊断文本（驱动缺陷 / 契约适配问题）
    std::string out;       // print 输出（协议行 + 被测函数打印）
    std::string panicText; // 未捕获 panic（含定位）
};

/// 合成驱动模块的编译执行。诊断/panic 均显式回收（不静默）。
DriverRun runDriver(Session& session, const std::string& modName,
                    const std::string& source) {
    DriverRun r;
    SourceMgr& src = session.src();
    DiagEngine& diag = session.diag();

    FileId file = src.addFile(modName, source);
    Lexer lexer(src, file, diag);
    Parser parser(src, file, diag, lexer.tokenize());
    auto mod = parser.parseModule(modName);
    if (!mod || diag.hasErrors()) {
        diag.emitText(r.diags);
        return r;
    }
    TypeChecker checker(src, diag, session.registry());
    checker.check(*mod);
    if (diag.hasErrors()) {
        diag.emitText(r.diags);
        return r;
    }

    // 字节码：已加载模块依赖序在前，驱动模块最后（与 run/REPL 执行序一致）。
    bc::Compiler compiler(src, diag, session.registry(), &session.promoteF64());
    auto order = session.allModulesInDependencyOrder();
    for (const auto& n : order) {
        const Module* m = session.loadedModule(n);
        if (!m || !compiler.compileModule(*m)) {
            diag.emitText(r.diags);
            return r;
        }
    }
    if (!compiler.compileModule(*mod)) {
        diag.emitText(r.diags);
        return r;
    }
    order.push_back(modName);

    bc::VMOptions vo;
    vo.entryFunc = "__sgt__";
    vo.out = [&](const std::string& s) { r.out += s; };
    // 能力授予集（0.9.7）：驱动模块 + import 闭包，与 run 同语义。
    bc::Program prog = compiler.take();
    prog.capMask = capMaskOf(*mod);
    for (const auto& n : order) {
        const Module* m = session.loadedModule(n);
        if (m) prog.capMask |= capMaskOf(*m);
    }
    bc::VM vm(std::move(prog), vo);
    if (!vm.run(order, modName)) {
        const bc::Panic& p = vm.lastPanic();
        std::string where = session.loadedModulePath(p.module);
        if (where.empty()) where = p.module;
        r.panicText = "panic: " + p.message + " at " + p.func + " (" + where +
                      ":" + std::to_string(p.line) + ")";
        return r;
    }
    r.ok = true;
    return r;
}

// ----------------------------------------------------------------------------
// 协议解析
// ----------------------------------------------------------------------------

/// 协议行提取：仅保留 "#TEST " 前缀行，按空白切 token（tok[0]=状态，
/// tok[1]=成因标签或测试名）。被测函数自身的 print 输出一律忽略。
std::vector<std::vector<std::string>> parseProtocol(const std::string& out) {
    std::vector<std::vector<std::string>> lines;
    size_t pos = 0;
    for (;;) {
        size_t eol = out.find('\n', pos);
        std::string line = out.substr(
            pos, (eol == std::string::npos ? out.size() : eol) - pos);
        if (line.size() >= 7 && line.compare(0, 6, "#TEST ") == 0) {
            std::vector<std::string> tok;
            size_t b = 6;
            for (;;) {
                size_t sp = line.find(' ', b);
                tok.push_back(line.substr(
                    b, (sp == std::string::npos ? line.size() : sp) - b));
                if (sp == std::string::npos) break;
                b = sp + 1;
            }
            lines.push_back(std::move(tok));
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return lines;
}

/// 加载 + 全量检查（属性轮/显式轮共用；失败时 diag 可取诊断文本）。
bool loadAndCheck(Session& session, const fs::path& inPath,
                  const fs::path& sgtPath) {
    if (!session.load(inPath)) return false;
    if (!sgtPath.empty() && !session.loadAs(inPath.stem().string() + "__sgt",
                                            sgtPath))
        return false;
    session.checkImported();
    return !session.diag().hasErrors();
}

} // namespace

// ----------------------------------------------------------------------------
// 命令入口
// ----------------------------------------------------------------------------

int runTests(const std::vector<std::string>& args) {
    bool json = false;
    uint64_t seedArg = 0;
    bool haveSeed = false;
    int64_t runs = 64;
    std::string inFile;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--json") {
            json = true;
        } else if (args[i] == "--seed" && i + 1 < args.size()) {
            seedArg = std::strtoull(args[++i].c_str(), nullptr, 10);
            haveSeed = true;
        } else if (args[i] == "--runs" && i + 1 < args.size()) {
            runs = std::strtoll(args[++i].c_str(), nullptr, 10);
        } else {
            inFile = args[i];
        }
    }
    if (inFile.empty() || args.size() > 7 || runs < 1 || runs > 4096) {
        std::cerr << "用法: scourge test [--json] [--seed N] [--runs N] "
                     "<file.sg>（--runs 取值 1..4096）\n";
        return 2;
    }

    const fs::path inPath(inFile);
    const std::string stem = inPath.stem().string();
    const fs::path sgtPath = inPath.parent_path() / (stem + ".sgt");
    const bool hasSgt = fs::exists(sgtPath);

    // ---- 校验轮：目标模块全量检查（0 错误才继续；.sgt 不在此轮） ----
    {
        Session session;
        if (!loadAndCheck(session, inPath, {})) {
            std::string out;
            session.diag().emitText(out);
            std::cerr << out;
            return 1;
        }
    }

    std::vector<TestResult> testResults;
    std::vector<PropResult> propResults;

    // ---- 显式轮：<stem>.sgt（存在才执行；编译失败不阻断属性轮） ----
    if (hasSgt) {
        Session session;
        if (!loadAndCheck(session, inPath, sgtPath)) {
            std::string out;
            session.diag().emitText(out);
            TestResult tr;
            tr.name = "<" + stem + ".sgt>";
            tr.status = "error";
            tr.detail = out;
            testResults.push_back(std::move(tr));
        } else {
            const std::string sgtName = stem + "__sgt";
            const auto& syms = *session.registry().at(sgtName);
            std::vector<std::string> testNames;
            for (const auto& [n, sig] : syms.funcs) {
                if (n.compare(0, 5, "test_") != 0) continue;
                if (sig.ret && sig.ret->kind == Type::Kind::Bool) {
                    testNames.push_back(n);
                } else {
                    TestResult tr;
                    tr.name = n;
                    tr.status = "skip";
                    tr.detail = "返回类型非 bool（test_* 须为 () -> bool）";
                    testResults.push_back(std::move(tr));
                }
            }
            if (testNames.empty()) {
                TestResult tr;
                tr.name = "<" + stem + ".sgt>";
                tr.status = "skip";
                tr.detail = "无 test_* 函数";
                testResults.push_back(std::move(tr));
            } else {
                DriverRun r = runDriver(
                    session, "sgt_driver", synthesizeTestDriver(sgtName, testNames));
                auto lines = parseProtocol(r.out);
                for (size_t i = 0; i < testNames.size(); ++i) {
                    TestResult tr;
                    tr.name = testNames[i];
                    // 协议行与 testNames 一一对应（驱动按同序生成）。
                    if (i < lines.size()) {
                        const std::string& st = lines[i][0];
                        tr.status = st;
                        if (st == "fail") tr.detail = "返回 False";
                        if (st == "panic") tr.detail = "执行 panic";
                    } else {
                        tr.status = "error";
                        tr.detail =
                            r.panicText.empty() ? "协议行缺失" : r.panicText;
                    }
                    testResults.push_back(std::move(tr));
                }
            }
        }
    }

    // ---- 属性候选：@pure 且带契约的模块函数（名字级发现，随轮重建计划） ----
    std::vector<std::string> propNames;
    {
        Session session;
        if (loadAndCheck(session, inPath, {})) {
            const auto& syms = *session.registry().at(stem);
            for (const auto& [n, sig] : syms.funcs)
                if (sig.isPure && !sig.contracts.empty()) propNames.push_back(n);
        }
    }

    for (const auto& funcName : propNames) {
        PropResult pr;
        pr.func = funcName;

        Session session;
        if (!loadAndCheck(session, inPath, {})) {
            pr.status = "error";
            session.diag().emitText(pr.detail);
            propResults.push_back(std::move(pr));
            continue;
        }
        const auto& syms = *session.registry().at(stem);
        PropPlan p = buildPlan(session, stem, syms.funcs.at(funcName));
        if (!p.runnable) {
            pr.status = "skip";
            pr.skipReason = p.skipReason;
            propResults.push_back(std::move(pr));
            continue;
        }

        // 种子：--seed 显式指定；缺省按 模块/函数名 派生（同版本可复现）。
        const uint64_t seed =
            haveSeed ? seedArg : sgt::Rng(stem + "/" + funcName).state;
        pr.seed = static_cast<int64_t>(seed);
        sgt::Rng rng(seed);

        std::vector<std::vector<std::string>> tuples;
        tuples.reserve(static_cast<size_t>(runs));
        for (int64_t i = 0; i < runs; ++i) {
            std::vector<std::string> t;
            for (const auto& pt : p.paramTypes)
                t.push_back(genValue(pt, rng, static_cast<size_t>(i)));
            tuples.push_back(std::move(t));
        }

        DriverRun r =
            runDriver(session, "sgt_prop", synthesizePropertyDriver(p, stem, tuples));
        if (!r.ok) {
            pr.status = "error";
            pr.detail = r.panicText.empty() ? r.diags : r.panicText;
            propResults.push_back(std::move(pr));
            continue;
        }

        // 协议行与实参逐一对齐（驱动保证 1:1）；首个非 pass 例即反例。
        auto lines = parseProtocol(r.out);
        pr.runs = runs;
        if (lines.size() != tuples.size()) {
            pr.status = "error";
            pr.detail = "协议行数 " + std::to_string(lines.size()) + " != 例数 " +
                        std::to_string(tuples.size()) +
                        (r.panicText.empty() ? "" : "；" + r.panicText);
            propResults.push_back(std::move(pr));
            continue;
        }
        bool anyFail = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& st = lines[i][0];
            if (st == "pass") {
                ++pr.passed;
                continue;
            }
            if (st == "skip") {
                ++pr.skipped;
                continue;
            }
            ++pr.failed;
            if (!anyFail) {
                anyFail = true;
                pr.ceArgs = tuples[i];
                const std::string tag =
                    lines[i].size() >= 2 ? lines[i][1] : st;
                pr.ceReason =
                    st == "error" ? "契约表达式求值 panic"
                    : tag == "panic" ? "前置满足后调用 panic"
                    : tag == "ensures" ? "ensures 违反"
                                       : "@pure 差分不一致";
            }
        }
        pr.status = pr.failed > 0 ? "fail" : "pass";
        propResults.push_back(std::move(pr));
    }

    // ---- 报告 ----
    const auto bad = [](const std::string& s) {
        return s != "pass" && s != "skip";
    };
    const bool ok = std::none_of(testResults.begin(), testResults.end(),
                                 [&](const TestResult& t) { return bad(t.status); }) &&
                    std::none_of(propResults.begin(), propResults.end(),
                                 [&](const PropResult& p) { return bad(p.status); });

    if (json) {
        JsonWriter w(/*pretty=*/true);
        w.beginObject();
        w.key("module");
        w.valueString(stem);
        w.key("seed");
        if (haveSeed) w.valueInt(static_cast<int64_t>(seedArg));
        else w.valueNull();
        w.key("tests");
        w.beginArray();
        for (const auto& t : testResults) {
            w.beginObject();
            w.key("name");
            w.valueString(t.name);
            w.key("status");
            w.valueString(t.status);
            if (!t.detail.empty()) {
                w.key("detail");
                w.valueString(t.detail);
            }
            w.endObject();
        }
        w.endArray();
        w.key("properties");
        w.beginArray();
        for (const auto& p : propResults) {
            w.beginObject();
            w.key("func");
            w.valueString(p.func);
            w.key("status");
            w.valueString(p.status);
            w.key("runs");
            w.valueInt(p.runs);
            w.key("passed");
            w.valueInt(p.passed);
            w.key("failed");
            w.valueInt(p.failed);
            w.key("skipped");
            w.valueInt(p.skipped);
            w.key("seed");
            w.valueInt(p.seed);
            if (!p.skipReason.empty()) {
                w.key("skipReason");
                w.valueString(p.skipReason);
            }
            if (!p.ceArgs.empty()) {
                w.key("counterexample");
                w.beginObject();
                w.key("args");
                w.beginArray();
                for (const auto& a : p.ceArgs) w.valueString(a);
                w.endArray();
                w.key("reason");
                w.valueString(p.ceReason);
                w.endObject();
            }
            if (!p.detail.empty()) {
                w.key("detail");
                w.valueString(p.detail);
            }
            w.endObject();
        }
        w.endArray();
        w.key("summary");
        w.beginObject();
        w.key("tests");
        w.valueInt(static_cast<int64_t>(testResults.size()));
        w.key("testFailures");
        w.valueInt(static_cast<int64_t>(std::count_if(
            testResults.begin(), testResults.end(),
            [&](const TestResult& t) { return bad(t.status); })));
        w.key("properties");
        w.valueInt(static_cast<int64_t>(propResults.size()));
        w.key("propertyFailures");
        w.valueInt(static_cast<int64_t>(std::count_if(
            propResults.begin(), propResults.end(),
            [&](const PropResult& p) { return bad(p.status); })));
        w.key("ok");
        w.valueBool(ok);
        w.endObject();
        w.endObject();
        std::cout << w.take() << "\n";
    } else {
        std::cout << "== scourge test: " << inFile << " ==\n";
        for (const auto& t : testResults) {
            const char* mark = t.status == "pass"    ? "[通过]"
                               : t.status == "skip"  ? "[跳过]"
                               : t.status == "error" ? "[错误]"
                                                     : "[失败]";
            std::cout << mark << " " << t.name;
            if (!t.detail.empty()) std::cout << "—— " << t.detail;
            std::cout << "\n";
        }
        for (const auto& p : propResults) {
            if (p.status == "skip") {
                std::cout << "[跳过] " << p.func << "()—— " << p.skipReason
                          << "\n";
                continue;
            }
            if (p.status == "error") {
                std::cout << "[错误] " << p.func << "()—— " << p.detail << "\n";
                continue;
            }
            std::cout << (p.status == "pass" ? "[通过] " : "[失败] ") << p.func
                      << "()：";
            std::cout << p.runs << " 例（通过 " << p.passed << "，跳过 "
                      << p.skipped << "）\n";
            if (!p.ceArgs.empty()) {
                std::cout << "       反例: " << p.func << "(" << joinArgs(p.ceArgs)
                          << ")—— " << p.ceReason << "\n";
            }
        }
        if (testResults.empty() && propResults.empty())
            std::cout << "无可运行的测试（未找到 " << stem
                      << ".sgt，模块亦无 @pure+契约函数）\n";
        std::cout << (ok ? "全部通过\n" : "存在失败\n");
    }

    return ok ? 0 : 1;
}

} // namespace scourge
