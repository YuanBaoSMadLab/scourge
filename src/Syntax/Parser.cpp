// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 语法分析器实现（驱动：构造、记号衔接与模块骨架）
// ----------------------------------------------------------------------------
//  职责边界：构造器、peekTok/advance/match/expect 记号衔接工具、
//  parseModule/parseBlock/parseStatement 顶层骨架、isConstLiteral 与
//  parseExpressionOnly（REPL/探针单表达式入口）。
//  分块：语句 ParserStmt.cpp、表达式 ParserExpr.cpp、类型注解与契约
//  ParserType.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/Syntax/Parser.hpp"

#include <cstdlib>

#include "scourge/Common/Capability.hpp"
#include "scourge/Syntax/Lexer.hpp"

namespace scourge {

// ============================================================================
// 构造与工具
// ============================================================================

Parser::Parser(const SourceMgr& src, FileId file, DiagEngine& diag,
               std::vector<Token> tokens)
    : src_(src), file_(file), diag_(diag), toks_(std::move(tokens)) {}

const Token& Parser::peekTok(size_t n) const {
    size_t i = pos_ + n;
    if (i >= toks_.size()) i = toks_.size() - 1;
    return toks_[i];
}

const Token& Parser::advance() {
    const Token& t = toks_[pos_];
    if (pos_ + 1 < toks_.size()) ++pos_;
    return t;
}

bool Parser::at(TokKind k) const { return cur().kind == k; }
bool Parser::atPunct(std::string_view op) const {
    return cur().kind == TokKind::Punct && cur().text == op;
}
bool Parser::atKeyword(std::string_view kw) const {
    return cur().kind == TokKind::Keyword && cur().text == kw;
}
bool Parser::matchPunct(std::string_view op) {
    if (atPunct(op)) {
        advance();
        return true;
    }
    return false;
}
bool Parser::matchKeyword(std::string_view kw) {
    if (atKeyword(kw)) {
        advance();
        return true;
    }
    return false;
}

const Token* Parser::expectPunct(std::string_view op) {
    if (atPunct(op)) {
        advance();
        return &toks_[pos_ - 1];
    }
    diag_.error(Errc::E1005_UnexpectedToken, file_, cur().range,
                "缺少 '" + std::string(op) + "'，实际为 " + tokenDesc(cur()));
    return nullptr;
}

const Token* Parser::expectKeyword(std::string_view kw) {
    if (atKeyword(kw)) {
        advance();
        return &toks_[pos_ - 1];
    }
    diag_.error(Errc::E1005_UnexpectedToken, file_, cur().range,
                "缺少关键字 '" + std::string(kw) + "'，实际为 " + tokenDesc(cur()));
    return nullptr;
}

const Token* Parser::expectKind(TokKind k, std::string_view what) {
    if (cur().kind == k) {
        advance();
        return &toks_[pos_ - 1];
    }
    diag_.error(Errc::E1005_UnexpectedToken, file_, cur().range,
                "期望 " + std::string(what) + "，实际为 " + tokenDesc(cur()));
    return nullptr;
}

std::string Parser::tokenDesc(const Token& t) {
    switch (t.kind) {
    case TokKind::Eof: return "文件结束";
    case TokKind::Newline: return "换行";
    case TokKind::Indent: return "缩进";
    case TokKind::Dedent: return "反缩进";
    case TokKind::Ident: return "标识符 '" + t.text + "'";
    case TokKind::Keyword: return "关键字 '" + t.text + "'";
    case TokKind::Punct: return "'" + t.text + "'";
    case TokKind::IntLit: return "整数 '" + t.text + "'";
    case TokKind::FloatLit: return "浮点数 '" + t.text + "'";
    case TokKind::StrLit: return "字符串";
    case TokKind::FStrPart: return "f-string 字面段";
    case TokKind::FStrExpr: return "f-string 表达式";
    }
    return "未知记号";
}

ExprPtr Parser::fail(const Token& at, std::string msg) {
    diag_.error(Errc::E1005_UnexpectedToken, file_, at.range, std::move(msg));
    return nullptr;
}

// ============================================================================
// 模块与语句
// ============================================================================

std::unique_ptr<Module> Parser::parseModule(std::string moduleName) {
    auto m = std::make_unique<Module>();
    m->file = file_;
    m->name = std::move(moduleName);

    while (!atEof()) {
        if (at(TokKind::Newline) || at(TokKind::Dedent)) {
            advance();
            continue;
        }
        StmtPtr s = parseStatement();
        if (!s) {
            syncToStatementBoundary();
            continue;
        }
        m->body.push_back(std::move(s));
    }
    return m;
}

void Parser::syncToStatementBoundary() {
    while (!atEof() && !at(TokKind::Newline) && !at(TokKind::Dedent)) advance();
    if (at(TokKind::Newline)) advance();
}

void Parser::endOfStatement() {
    if (at(TokKind::Newline)) {
        advance();
        return;
    }
    // 块尾（DEDENT）与文件尾合法终止语句。
    if (atEof() || at(TokKind::Dedent)) return;
    diag_.error(Errc::E1005_UnexpectedToken, file_, cur().range,
                "语句后缺少换行，实际为 " + tokenDesc(cur()));
}

StmtList Parser::parseBlock() {
    StmtList out;
    if (!expectPunct(":")) return out;
    if (at(TokKind::Newline)) advance();
    if (!at(TokKind::Indent)) {
        diag_.error(Errc::E1005_UnexpectedToken, file_, cur().range,
                    "期望缩进块（': ' 后需换行并缩进）");
        return out;
    }
    advance();
    while (!at(TokKind::Dedent) && !atEof()) {
        if (at(TokKind::Newline)) {
            advance();
            continue;
        }
        StmtPtr s = parseStatement();
        if (!s) {
            syncToStatementBoundary();
            continue;
        }
        out.push_back(std::move(s));
    }
    if (at(TokKind::Dedent)) advance();
    return out;
}

StmtPtr Parser::parseStatement() {
    if (atPunct("@")) return parseDecorated();
    if (cur().kind == TokKind::Keyword) {
        const std::string& kw = cur().text;
        if (kw == "def") return parseDef({});
        if (kw == "if") return parseIf();
        if (kw == "while") return parseWhile();
        if (kw == "for") return parseFor();
        if (kw == "return") return parseReturn();
        if (kw == "break") {
            Token t = cur();
            advance();
            endOfStatement();
            return std::make_unique<Break>(t.range);
        }
        if (kw == "continue") {
            Token t = cur();
            advance();
            endOfStatement();
            return std::make_unique<Continue>(t.range);
        }
        if (kw == "pass") {
            Token t = cur();
            advance();
            endOfStatement();
            return std::make_unique<Pass>(t.range);
        }
        if (kw == "try") return parseTry();
        if (kw == "defer") return parseDefer();
        if (kw == "await") { // await 是前缀表达式，不是语句起始
            diag_.error(Errc::E2010_AwaitOutsideAsync, file_, cur().range,
                        "await 只能出现在 async def 函数体内");
            return nullptr;
        }
        if (kw == "except") {
            diag_.error(Errc::E1005_UnexpectedToken, file_, cur().range,
                        "except 必须紧跟在 try 块之后");
            return nullptr;
        }
        if (kw == "assert") return parseAssert();
        if (kw == "async") { // async def：并发原语语法基座（文档第八章）
            advance();
            if (!atKeyword("def")) {
                diag_.error(Errc::E1005_UnexpectedToken, file_, cur().range,
                            "async 后必须是 def（async def 函数定义）");
                return nullptr;
            }
            return parseDef({}, /*isAsync=*/true);
        }
        if (kw == "import" || kw == "from") return parseImport();
        diag_.error(Errc::E1005_UnexpectedToken, file_, cur().range,
                    "意外的关键字 '" + kw + "'");
        return nullptr;
    }
    return parseSimple();
}

// ============================================================================
// 辅助
// ============================================================================

bool Parser::isConstLiteral(const Expr* e) const {
    switch (e->kind) {
    case Expr::Kind::IntLit:
    case Expr::Kind::FloatLit:
    case Expr::Kind::BoolLit:
    case Expr::Kind::StrLit:
    case Expr::Kind::NoneLit:
        return true;
    case Expr::Kind::Unary: {
        auto* u = static_cast<const Unary*>(e);
        return (u->op == "-" || u->op == "+") && u->operand &&
               isConstLiteral(u->operand.get());
    }
    default:
        return false;
    }
}

ExprPtr Parser::parseExpressionOnly(const SourceMgr& src, FileId file,
                                    DiagEngine& diag, SourceRange range) {
    if (range.length == 0) return nullptr;
    Lexer lx(src, file, diag, range.offset, range.offset + range.length);
    Parser p(src, file, diag, lx.tokenize());
    ExprPtr e = p.parseExpr();
    if (e && !p.atEof()) {
        diag.error(Errc::E1005_UnexpectedToken, file, p.cur().range,
                   "契约表达式后存在多余内容");
        return nullptr;
    }
    return e;
}

} // namespace scourge
