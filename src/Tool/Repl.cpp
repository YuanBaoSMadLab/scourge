// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge REPL 实现 — 会话式求值循环（设计文档 5.3）
// ----------------------------------------------------------------------------
//  复用纪律：每轮完整走 Lexer → Parser → TypeChecker → bc::Compiler →
//  bc::VM，与 check / run 命令同一管线；不引入任何平行实现。
//  会话状态 = 活动模块图（import 模块集 + preamble 文本拼接）——每轮
//  全量重编译，import 模块从磁盘重新加载进会话 SourceMgr，语义与
//  「把这些语句与 import 写进一个 .sg 文件再 run」完全一致。
// ============================================================================
#include "scourge/Tool/Repl.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "scourge/BC/Compiler.hpp"
#include "scourge/BC/VM.hpp"
#include "scourge/Common/Capability.hpp"
#include "scourge/Sema/TypeChecker.hpp"
#include "scourge/Tool/Session.hpp"
#include "scourge/Syntax/Lexer.hpp"
#include "scourge/Syntax/Parser.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace scourge {

namespace {

/// 求值包装函数名（经 VMOptions::entryFunc 指定，避免与用户定义的 main 冲突）。
constexpr const char* kReplEntry = "__repl__";

/// 输入单元类别（按首记号词法判定；正式诊断一律交主管线产出）。
enum class UnitKind {
    Statement,  ///< 语句 → 并入会话状态（仅验证不执行）
    Expression, ///< 表达式 → print 包装求值
    PrintCall,  ///< print(...) 调用 → 直接执行（不二次包装）
    Import,     ///< import / from-import → 会话模块图（from-import 显式拒绝）
};

/// 语句起始关键字白名单。值关键字（True/False/None/not/and/or/in/except/
/// elif/else/await）不在其列——以它们开头的输入仍按表达式求值或交检查器报错。
/// capability 软关键字亦按语句并入会话（0.9.7：顶层声明随 preamble 重编译）。
bool isStmtKeyword(const std::string& kw) {
    static const char* kStmtKw[] = {
        "def", "try", "while", "for", "if", "assert", "async",
        "pass", "return", "break", "continue", "capability",
    };
    for (const char* k : kStmtKw)
        if (kw == k) return true;
    return false;
}

/// 赋值类标点：= 与增强赋值（+= -= *= /= //= %= @= **=，与 Parser 同集）。
bool isAssignPunct(const std::string& op) {
    if (op == "=") return true;
    return op.size() >= 2 && op.back() == '=' && op[0] != '=' &&
           op[0] != '<' && op[0] != '>' && op[0] != '!';
}

/// 剥离行尾注释（# 起始；字符串字面量内不生效）。仅用于块开启判定，
/// 提交的原文不经此处理（注释由 Lexer 正常消费）。
std::string stripComment(const std::string& s) {
    bool inStr = false;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (inStr) {
            if (c == '\\')
                ++i;
            else if (c == quote)
                inStr = false;
        } else if (c == '"' || c == '\'') {
            inStr = true;
            quote = c;
        } else if (c == '#') {
            return s.substr(0, i);
        }
    }
    return s;
}

/// 判定（已剥注释的）行是否以冒号收尾——块开启的标志。
bool endsWithColon(const std::string& s) {
    size_t n = s.size();
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) --n;
    return n > 0 && s[n - 1] == ':';
}

/// 词法首记号分类。临时 SourceMgr/DiagEngine 只为切词，诊断即弃。
UnitKind classifyUnit(const std::string& unit) {
    SourceMgr tmp;
    DiagEngine diag(tmp);
    FileId f = tmp.addFile("repl-input", unit);
    Lexer lexer(tmp, f, diag);
    std::vector<Token> toks = lexer.tokenize();

    size_t i = 0;
    while (i < toks.size() && (toks[i].kind == TokKind::Newline ||
                               toks[i].kind == TokKind::Indent))
        ++i;
    if (i >= toks.size() || toks[i].kind == TokKind::Eof)
        return UnitKind::Expression; // 空输入：交主管线裁决
    const Token& first = toks[i];
    const Token* second =
        (i + 1 < toks.size() && toks[i + 1].kind != TokKind::Eof)
            ? &toks[i + 1]
            : nullptr;

    if (first.kind == TokKind::Keyword) {
        if (first.text == "import" || first.text == "from")
            return UnitKind::Import;
        return isStmtKeyword(first.text) ? UnitKind::Statement
                                         : UnitKind::Expression;
    }
    if (first.kind == TokKind::Ident && second &&
        second->kind == TokKind::Punct) {
        if (first.text == "print" && second->text == "(")
            return UnitKind::PrintCall;
        // "name:"（注解赋值）与 "name =" / "name +=" 同为赋值语句。
        if (second->text == ":" || isAssignPunct(second->text))
            return UnitKind::Statement;
    }
    return UnitKind::Expression;
}

void emitDiags(std::ostream& err, const DiagEngine& diag) {
    std::string text;
    diag.emitText(text);
    err << text;
}

bool isStdinTty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

} // namespace

// ----------------------------------------------------------------------------
// 会话生命周期
// ----------------------------------------------------------------------------

Repl::Repl(std::ostream& out, std::ostream& err) : out_(out), err_(err) {}

int Repl::run(std::istream& in) {
    const bool tty = (&in == &std::cin) && isStdinTty();
    if (tty) out_ << "Scourge REPL — :help 查看命令\n";

    std::string line;
    while (!finished_) {
        if (tty) out_ << (inBlock_ ? "... " : ">>> ") << std::flush;
        if (!std::getline(in, line)) {
            if (inBlock_) err_ << "输入流结束：未闭合块已丢弃\n";
            break;
        }
        feed(line);
    }
    return 0;
}

bool Repl::feed(const std::string& raw) {
    if (finished_) return false;

    // CRLF 兼容：先剥行尾 \r（Lexer 只认 \n，与 readFileNormalized 同规则）。
    std::string line = raw;
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // 判定视图：两端去空白（单行前导空白无块语义；块内行保留原文缩进）。
    std::string t = line;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
        t.erase(t.begin());
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();

    if (t == ":quit" || t == ":q") {
        finished_ = true;
        return false;
    }

    if (inBlock_) {
        if (t.empty()) { // 空行：块结束，整体提交
            inBlock_ = false;
            std::string unit;
            unit.swap(blockBuf_);
            acceptUnit(unit);
            return true;
        }
        if (t[0] == ':') { // 块内命令：块优先，提示先收块
            err_ << "块未结束：先以空行结束当前块，再使用命令\n";
            return true;
        }
        blockBuf_ += line; // 保留原文缩进（INDENT/DEDENT 依赖列宽）
        blockBuf_ += '\n';
        return true;
    }

    if (!t.empty() && t[0] == ':') {
        runCommand(t);
        return true;
    }
    if (t.empty()) return true; // 空闲态空行：忽略

    if (endsWithColon(stripComment(t))) { // 行尾冒号 → 开块
        inBlock_ = true;
        blockBuf_ = t;
        blockBuf_ += '\n';
        return true;
    }

    acceptUnit(t);
    return true;
}

// ----------------------------------------------------------------------------
// 输入单元处理
// ----------------------------------------------------------------------------

void Repl::acceptUnit(const std::string& unit) {
    switch (classifyUnit(unit)) {
    case UnitKind::Statement:
        defineStatement(unit);
        break;
    case UnitKind::Expression:
        evalExpression(unit, /*wrapPrint=*/true);
        break;
    case UnitKind::PrintCall:
        evalExpression(unit, /*wrapPrint=*/false);
        break;
    case UnitKind::Import:
        acceptImport(unit);
        break;
    }
}

void Repl::evalExpression(const std::string& expr, bool wrapPrint) {
    std::string tail = "def ";
    tail += kReplEntry;
    tail += "() -> int:\n";
    if (wrapPrint)
        tail += "    print(" + expr + ")\n";
    else
        tail += "    " + expr + "\n"; // print 调用按语句执行，不二次包装
    tail += "    return 0\n";
    compileSession(sessionSource(tail), /*execute=*/true);
}

void Repl::defineStatement(const std::string& stmt) {
    std::string saved = preamble_;
    preamble_ += stmt;
    if (preamble_.empty() || preamble_.back() != '\n') preamble_ += '\n';
    if (!compileSession(sessionSource(""), /*execute=*/false)) {
        preamble_ = saved; // 失败回滚：会话状态不丢
        err_ << "（定义未并入会话；已回滚）\n";
    }
}

void Repl::acceptImport(const std::string& unit) {
    // 用主管线解析该单元提取模块名（不手写字符串切分）。
    // 临时 SourceMgr/DiagEngine 只为取 Import 节点，诊断即弃。
    SourceMgr tmp;
    DiagEngine diag(tmp);
    FileId f = tmp.addFile("repl-import", unit);
    Lexer lexer(tmp, f, diag);
    Parser parser(tmp, f, diag, lexer.tokenize());
    auto mod = parser.parseModule("repl-import");
    if (!mod || diag.hasErrors()) {
        emitDiags(err_, diag);
        return;
    }
    std::vector<std::string> names;
    for (const auto& s : mod->body) {
        if (s->kind != Stmt::Kind::Import) continue;
        auto& im = static_cast<const Import&>(*s);
        if (im.isFrom) {
            err_ << "REPL 暂不支持 from-import（仅 import <模块>）\n";
            return;
        }
        names.push_back(im.module());
    }
    if (names.empty()) {
        err_ << "import 语句未解析出模块名\n";
        return;
    }

    // 试编译验收：候选模块图完整检查（不执行），0 错误才并入会话。
    std::vector<std::string> saved = imports_;
    for (const auto& n : names)
        if (std::find(imports_.begin(), imports_.end(), n) == imports_.end())
            imports_.push_back(n);
    if (!compileSession(sessionSource(""), /*execute=*/false)) {
        imports_ = std::move(saved); // 失败回滚：模块图状态不丢
        err_ << "（import 未并入会话；已回滚）\n";
    }
}

std::string Repl::sessionSource(const std::string& tail) const {
    std::string src;
    for (const auto& n : imports_) src += "import " + n + "\n";
    src += preamble_;
    src += tail;
    return src;
}

bool Repl::compileSession(const std::string& source, bool execute) {
    // 每轮独立 Session：import 模块从磁盘重新加载（活动模块图，磁盘编辑
    // 即时生效），repl 轮模块与 import 模块共用同一 SourceMgr——行号与
    // 字节码行映射跨模块一致。诊断/符号状态随轮销毁，无跨轮污染。
    Session session;
    for (const auto& n : imports_) {
        if (!session.importModule(n)) {
            emitDiags(err_, session.diag());
            return false;
        }
    }
    SourceMgr& src = session.src();
    DiagEngine& diag = session.diag();
    FileId file = src.addFile("repl", source);

    Lexer lexer(src, file, diag);
    Parser parser(src, file, diag, lexer.tokenize());
    auto mod = parser.parseModule("repl");
    if (!mod || diag.hasErrors()) {
        emitDiags(err_, diag);
        return false;
    }

    if (!imports_.empty()) {
        // 先查 import 模块图 → registry（repl 模块检查要用其符号）。
        session.checkImported();
        if (diag.hasErrors()) {
            emitDiags(err_, diag);
            return false;
        }
    }
    // repl 轮模块自身检查（preamble + 求值包装）——与单模块会话同一纪律：
    // 定义 / 表达式全部经类型/形状/契约检查，无一绕过。
    TypeChecker checker(src, diag, session.registry());
    checker.check(*mod);
    if (diag.hasErrors()) {
        emitDiags(err_, diag);
        return false;
    }
    lastWarnings_ = diag.warningCount();
    if (!execute) return true;

    // 字节码：import 模块先编译（依赖序），repl 轮模块最后——
    // 跨模块调用解析与 <top> 执行序与 run 命令完全一致。
    bc::Compiler compiler(src, diag, session.registry(), &session.promoteF64());
    auto order = session.allModulesInDependencyOrder();
    for (const auto& n : order) {
        const Module* m = session.loadedModule(n);
        if (!m || !compiler.compileModule(*m)) {
            emitDiags(err_, diag);
            return false;
        }
    }
    if (!compiler.compileModule(*mod)) {
        emitDiags(err_, diag);
        return false;
    }
    order.push_back("repl");

    bc::VMOptions vo;
    vo.entryFunc = kReplEntry;
    vo.out = [this](const std::string& s) { out_ << s; };
    // 能力授予集（0.9.7）：repl 轮模块（preamble 内 capability 声明）+
    // import 模块闭包——与 run 的模块并集同语义。
    bc::Program prog = compiler.take();
    prog.capMask = capMaskOf(*mod);
    for (const auto& n : order) {
        const Module* m = session.loadedModule(n);
        if (m) prog.capMask |= capMaskOf(*m);
    }
    bc::VM vm(std::move(prog), vo);
    if (!vm.run(order, "repl")) {
        const bc::Panic& p = vm.lastPanic();
        // 定位：repl 轮模块用合成名，import 模块用磁盘路径。
        std::string where =
            (p.module.empty() || p.module == "repl")
                ? std::string("repl")
                : session.loadedModulePath(p.module);
        if (where.empty()) where = p.module;
        err_ << "panic: " << p.message << " at " << p.func << " ("
             << where << ":" << p.line << ")\n";
        return false;
    }
    return true;
}

void Repl::runCommand(const std::string& cmd) {
    if (cmd == ":help") {
        out_ << "命令:\n"
                "  <表达式>     求值并输出（如 1 + 2、fib(10)、x[0]）\n"
                "  <语句>       并入会话状态（def / 赋值；行尾冒号开启块，空行结束）\n"
                "  import <名>  并入会话模块图（相对当前目录 <名>.sg，试编译验收）\n"
                "  :check       对当前会话状态执行全量验证\n"
                "  :reset       清空会话状态\n"
                "  :quit / :q   退出\n"
                "说明: 定义仅验证不执行；表达式轮会重放会话顶层副作用\n"
                "      （含 import 模块顶层，磁盘编辑即时生效）；\n"
                "      from-import 暂不支持；__repl__ 为求值包装保留名\n";
        return;
    }
    if (cmd == ":check") {
        if (compileSession(sessionSource(""), /*execute=*/false))
            out_ << "会话检查：OK（0 错误, " << lastWarnings_ << " 警告）\n";
        return;
    }
    if (cmd == ":reset") {
        preamble_.clear();
        imports_.clear();
        out_ << "会话状态已重置\n";
        return;
    }
    err_ << "未知命令 '" << cmd << "'（:help 查看可用命令）\n";
}

// ----------------------------------------------------------------------------
// CLI 入口
// ----------------------------------------------------------------------------

int cmdRepl() {
    Repl repl(std::cout, std::cerr);
    return repl.run(std::cin);
}

} // namespace scourge
