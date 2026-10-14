// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 语法分析器实现
// ============================================================================
#include "scourge/Syntax/Parser.hpp"

#include <cstdlib>

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

// ----------------------------------------------------------------------------
// 装饰器 + def
// ----------------------------------------------------------------------------

StmtPtr Parser::parseDecorated() {
    std::vector<Decorator> decos;
    while (atPunct("@")) {
        SourceRange atRange = cur().range;
        advance();
        const Token* nameTok = expectKind(TokKind::Ident, "装饰器名");
        if (!nameTok) return nullptr;

        Decorator d;
        d.name = nameTok->text;
        d.range = SourceRange::merge(atRange, nameTok->range);

        // @name(...)：位置参数与关键字参数
        if (matchPunct("(")) {
            if (!atPunct(")")) {
                for (;;) {
                    // 关键字参数：ident '=' expr
                    if (cur().kind == TokKind::Ident &&
                        peekTok(1).kind == TokKind::Punct && peekTok(1).text == "=") {
                        std::string key = advance().text;
                        advance(); // '='
                        ExprPtr v = parseExpr();
                        if (!v) return nullptr;
                        d.kwargs.emplace_back(std::move(key), std::move(v));
                    } else {
                        ExprPtr v = parseExpr();
                        if (!v) return nullptr;
                        d.args.push_back(std::move(v));
                    }
                    if (matchPunct(",")) {
                        if (atPunct(")")) break; // 允许尾逗号
                        continue;
                    }
                    break;
                }
            }
            if (!expectPunct(")")) return nullptr;
        }
        endOfStatement();
        decos.push_back(std::move(d));
    }

    if (atKeyword("async")) {
        advance();
        if (!atKeyword("def")) {
            diag_.error(Errc::E1005_UnexpectedToken, file_, cur().range,
                        "async 后必须是 def（async def 函数定义）");
            return nullptr;
        }
        return parseDef(std::move(decos), /*isAsync=*/true);
    }
    if (!atKeyword("def")) {
        diag_.error(Errc::E1005_UnexpectedToken, file_, cur().range,
                    "装饰器只能用于 def，实际为 " + tokenDesc(cur()));
        return nullptr;
    }
    return parseDef(std::move(decos));
}

StmtPtr Parser::parseDef(std::vector<Decorator> decorators, bool isAsync) {
    const Token& kw = advance(); // def
    const Token* nameTok = expectKind(TokKind::Ident, "函数名");
    if (!nameTok) return nullptr;
    auto def = std::make_unique<FuncDef>(kw.range, nameTok->text, nameTok->range);
    def->decorators = std::move(decorators);
    def->isAsync = isAsync;

    if (!expectPunct("(")) return nullptr;
    if (!atPunct(")")) {
        for (;;) {
            const Token* p = expectKind(TokKind::Ident, "参数名");
            if (!p) return nullptr;
            Param param;
            param.name = p->text;
            param.range = p->range;
            if (matchPunct(":")) {
                param.type = parseTypeAnnotation();
                if (!param.type) return nullptr;
            }
            if (matchPunct("=")) {
                param.defaultValue = parseExpr();
                if (!param.defaultValue) return nullptr;
                if (!isConstLiteral(param.defaultValue.get())) {
                    diag_.error(Errc::E1007_UnsupportedFeature, file_,
                                param.defaultValue->range,
                                "默认参数仅支持字面量（含一元正负号），保证调用语义确定");
                }
            }
            def->params.push_back(std::move(param));
            if (matchPunct(",")) {
                if (atPunct(")")) break;
                continue;
            }
            break;
        }
    }
    if (!expectPunct(")")) return nullptr;

    if (matchPunct("->")) {
        def->retType = parseTypeAnnotation();
        if (!def->retType) return nullptr;
    }
    // 冒号由 parseBlock 统一消耗（与 if/while/for 保持一致）。
    def->body = parseBlock();
    if (def->body.empty() && hasErrors()) return nullptr;

    // docstring：函数体首语句为字符串表达式时提取为契约容器。
    // （先于 @extern 校验：docstring 不计入函数体语句，D2 设计确认项。）
    if (!def->body.empty() && def->body[0]->kind == Stmt::Kind::ExprStmt) {
        auto* es = static_cast<ExprStmt*>(def->body[0].get());
        if (es->expr && es->expr->kind == Expr::Kind::StrLit) {
            auto* sl = static_cast<StrLit*>(es->expr.get());
            def->docstring = sl->strValue;
            def->docRange = sl->range;
            def->body.erase(def->body.begin());
        }
    }
    // @extern = C ABI 纯声明（文档第八章 FFI）：函数体只允许一条 pass 语句
    // （docstring 已先行提取、不计入函数体——D2 设计确认项）。
    bool hasExtern = false;
    for (const auto& d : def->decorators) hasExtern |= (d.name == "extern");
    if (hasExtern &&
        !(def->body.empty() ||
          (def->body.size() == 1 && def->body[0]->kind == Stmt::Kind::Pass))) {
        diag_.error(Errc::E1007_UnsupportedFeature, file_, def->nameRange,
                    "@extern 函数必须是纯声明：函数体只允许一条 pass 语句"
                    "（docstring 不计入函数体；实现在外部 C ABI 侧，不由本语言提供）");
    }
    extractContracts(*def);
    return def;
}

bool Parser::hasErrors() const { return diag_.hasErrors(); }

// ----------------------------------------------------------------------------
// 控制流语句
// ----------------------------------------------------------------------------

StmtPtr Parser::parseIf() {
    const Token& kw = advance(); // if
    ExprPtr cond = parseExpr();
    if (!cond) return nullptr;
    StmtList thenBody = parseBlock();
    auto st = std::make_unique<If>(kw.range, std::move(cond), std::move(thenBody));

    while (atKeyword("elif")) {
        advance();
        ExprPtr c = parseExpr();
        if (!c) return nullptr;
        If::Elif e;
        e.cond = std::move(c);
        e.body = parseBlock();
        st->elifs.push_back(std::move(e));
    }
    if (atKeyword("else")) {
        advance();
        st->elseBody = parseBlock();
    }
    return st;
}

StmtPtr Parser::parseWhile() {
    const Token& kw = advance(); // while
    ExprPtr cond = parseExpr();
    if (!cond) return nullptr;
    StmtList body = parseBlock();
    return std::make_unique<While>(kw.range, std::move(cond), std::move(body));
}

StmtPtr Parser::parseFor() {
    const Token& kw = advance(); // for
    const Token* var = expectKind(TokKind::Ident, "循环变量名");
    if (!var) return nullptr;
    if (!expectKeyword("in")) return nullptr;
    ExprPtr iter = parseExpr();
    if (!iter) return nullptr;
    StmtList body = parseBlock();
    return std::make_unique<For>(kw.range, var->text, var->range, std::move(iter),
                                 std::move(body));
}

StmtPtr Parser::parseReturn() {
    const Token& kw = advance(); // return
    ExprPtr value = nullptr;
    if (!at(TokKind::Newline) && !atEof() && !at(TokKind::Dedent))
        value = parseExpr();
    endOfStatement();
    return std::make_unique<Return>(kw.range, std::move(value));
}

StmtPtr Parser::parseAssert() {
    const Token& kw = advance(); // assert
    ExprPtr cond = parseExpr();
    if (!cond) return nullptr;
    ExprPtr msg = nullptr;
    if (matchPunct(",")) {
        msg = parseExpr();
        if (!msg) return nullptr;
    }
    endOfStatement();
    return std::make_unique<Assert>(kw.range, std::move(cond), std::move(msg));
}

StmtPtr Parser::parseTry() {
    const Token& kw = advance(); // try
    StmtList body = parseBlock();
    if (body.empty() && hasErrors()) return nullptr;

    if (!atKeyword("except")) {
        diag_.error(Errc::E1005_UnexpectedToken, file_, cur().range,
                    "try 块后必须有 except（panic 恢复路径）");
        return nullptr;
    }
    auto st = std::make_unique<Try>(kw.range, std::move(body));
    st->exceptRange = cur().range;
    advance(); // except

    // panic 恢复不分类别（One Obvious Way，文档第四章）；typed except 明确拒绝。
    if (!atPunct(":") && (at(TokKind::Ident) || at(TokKind::Keyword))) {
        diag_.error(Errc::E1007_UnsupportedFeature, file_, cur().range,
                    "panic 捕获不分类别：只支持裸 except:（可恢复错误请用"
                    " Result，错误即值）");
        // 恢复：跳过类别记号直至冒号，减少级联错误。
        while (!atPunct(":") && !atEof() && !at(TokKind::Newline) &&
               !at(TokKind::Dedent))
            advance();
    }
    st->handlerBody = parseBlock();

    if (atKeyword("except")) {
        diag_.error(Errc::E1007_UnsupportedFeature, file_, cur().range,
                    "只允许单条 except 子句（panic 恢复路径唯一）");
        advance();
        parseBlock(); // 恢复：吞掉冗余 except 块，避免级联错误
    }
    return st;
}

StmtPtr Parser::parseImport() {
    const Token& kw = advance(); // import / from
    bool isFrom = kw.text == "from";

    const Token* modTok = expectKind(TokKind::Ident, "模块名");
    if (!modTok) return nullptr;

    if (atPunct(".")) {
        diag_.error(Errc::E1007_UnsupportedFeature, file_, cur().range,
                    "点分模块路径暂不支持；每个 .sg 文件即一个模块");
    }

    std::vector<std::string> names;
    if (isFrom) {
        if (!expectKeyword("import")) return nullptr;
        for (;;) {
            const Token* n = expectKind(TokKind::Ident, "导入名");
            if (!n) return nullptr;
            names.push_back(n->text);
            if (matchKeyword("as")) {
                diag_.error(Errc::E1007_UnsupportedFeature, file_, cur().range,
                            "import as 别名暂不支持");
                advance(); // 恢复：跳过别名记号
            }
            if (matchPunct(",")) continue;
            break;
        }
    }
    endOfStatement();
    return std::make_unique<Import>(kw.range, modTok->text, isFrom,
                                    std::move(names));
}

// ----------------------------------------------------------------------------
// 简单语句：赋值 / 增强赋值 / 表达式语句
// ----------------------------------------------------------------------------

StmtPtr Parser::parseSimple() {
    // 声明式赋值：x: int = ... / x: int
    if (at(TokKind::Ident) && peekTok(1).kind == TokKind::Punct &&
        peekTok(1).text == ":") {
        const Token& nameTok = advance();
        advance(); // ':'
        TypeExprPtr ann = parseTypeAnnotation();
        if (!ann) return nullptr;
        ExprPtr value = nullptr;
        if (matchPunct("=")) {
            value = parseExpr();
            if (!value) return nullptr;
        }
        SourceRange r = SourceRange::merge(nameTok.range,
                                           value ? value->range : ann->range);
        auto target = std::make_unique<Name>(nameTok.range, nameTok.text);
        auto st = std::make_unique<Assign>(r, std::move(target), std::move(ann),
                                           std::move(value));
        endOfStatement();
        return st;
    }

    SourceRange begin = cur().range;
    ExprPtr e = parseExpr();
    if (!e) return nullptr;

    if (atPunct("=")) {
        advance();
        // 赋值目标合法性（设计文档：显式拒绝，报错即接口）。
        if (e->kind != Expr::Kind::Name && e->kind != Expr::Kind::Index) {
            diag_.error(Errc::E1006_InvalidAssignTarget, file_, e->range,
                        "非法赋值目标：只能对变量名或下标表达式赋值");
            return nullptr;
        }
        ExprPtr value = parseExpr();
        if (!value) return nullptr;
        if (atPunct("=")) {
            diag_.error(Errc::E1007_UnsupportedFeature, file_, cur().range,
                        "链式赋值（a = b = c）暂不支持");
            return nullptr;
        }
        SourceRange r = SourceRange::merge(begin, value->range);
        auto st = std::make_unique<Assign>(r, std::move(e), nullptr,
                                           std::move(value));
        endOfStatement();
        return st;
    }

    // 增强赋值
    static const char* kAugOps[] = {"+=", "-=", "*=", "/=", "//=", "%=",
                                    "@=", "**="};
    for (const char* op : kAugOps) {
        if (atPunct(op)) {
            advance();
            if (e->kind != Expr::Kind::Name) {
                diag_.error(Errc::E1006_InvalidAssignTarget, file_, e->range,
                            "增强赋值目标只能是变量名");
                return nullptr;
            }
            ExprPtr value = parseExpr();
            if (!value) return nullptr;
            SourceRange r = SourceRange::merge(begin, value->range);
            auto st = std::make_unique<AugAssign>(r, std::move(e), op,
                                                  std::move(value));
            endOfStatement();
            return st;
        }
    }

    endOfStatement();
    SourceRange r = SourceRange::merge(begin, e->range);
    return std::make_unique<ExprStmt>(r, std::move(e));
}

// ============================================================================
// 表达式（优先级：ternary < or < and < not < 比较 < +- < */ < 一元 < ** < 后缀）
// ============================================================================

ExprPtr Parser::parseExpr() {
    if (++depth_ > kMaxDepth) {
        --depth_;
        return fail(cur(), "表达式嵌套过深（> " + std::to_string(kMaxDepth) + " 层）");
    }
    ExprPtr e = parseOr();
    if (e && atKeyword("if")) {
        advance();
        ExprPtr cond = parseOr();
        if (!cond || !expectKeyword("else")) {
            --depth_;
            return nullptr;
        }
        ExprPtr els = parseExpr();
        if (!els) {
            --depth_;
            return nullptr;
        }
        SourceRange r = SourceRange::merge(e->range, els->range);
        e = std::make_unique<Ternary>(r, std::move(cond), std::move(e),
                                      std::move(els));
    }
    --depth_;
    return e;
}

ExprPtr Parser::parseOr() {
    ExprPtr e = parseAnd();
    while (e && atKeyword("or")) {
        const Token& op = advance();
        ExprPtr rhs = parseAnd();
        if (!rhs) return nullptr;
        SourceRange r = SourceRange::merge(e->range, rhs->range);
        e = std::make_unique<Binary>(r, op.text, std::move(e), std::move(rhs));
    }
    return e;
}

ExprPtr Parser::parseAnd() {
    ExprPtr e = parseNot();
    while (e && atKeyword("and")) {
        const Token& op = advance();
        ExprPtr rhs = parseNot();
        if (!rhs) return nullptr;
        SourceRange r = SourceRange::merge(e->range, rhs->range);
        e = std::make_unique<Binary>(r, op.text, std::move(e), std::move(rhs));
    }
    return e;
}

ExprPtr Parser::parseNot() {
    if (atKeyword("not")) {
        const Token& op = advance();
        ExprPtr operand = parseNot();
        if (!operand) return nullptr;
        SourceRange r = SourceRange::merge(op.range, operand->range);
        return std::make_unique<Unary>(r, "not", std::move(operand));
    }
    return parseComparison();
}

ExprPtr Parser::parseComparison() {
    ExprPtr e = parseAdd();
    if (!e) return nullptr;
    static const char* kCmps[] = {"==", "!=", "<", "<=", ">", ">="};
    for (const char* op : kCmps) {
        if (atPunct(op)) {
            const Token& t = advance();
            ExprPtr rhs = parseAdd();
            if (!rhs) return nullptr;
            SourceRange r = SourceRange::merge(e->range, rhs->range);
            e = std::make_unique<Binary>(r, t.text, std::move(e), std::move(rhs));
            if (atPunct("==") || atPunct("!=") || atPunct("<") ||
                atPunct("<=") || atPunct(">") || atPunct(">=")) {
                diag_.error(Errc::E1007_UnsupportedFeature, file_, cur().range,
                            "不支持链式比较；请用 and 显式组合（保证求值语义唯一）");
                // 恢复：吞掉链式剩余部分，避免语句尾级联报错。
                while (atPunct("==") || atPunct("!=") || atPunct("<") ||
                       atPunct("<=") || atPunct(">") || atPunct(">=")) {
                    advance();
                    if (!parseAdd()) return nullptr;
                }
            }
            return e;
        }
    }
    if (atKeyword("in")) {
        advance();
        ExprPtr rhs = parseAdd();
        if (!rhs) return nullptr;
        SourceRange r = SourceRange::merge(e->range, rhs->range);
        return std::make_unique<Binary>(r, "in", std::move(e), std::move(rhs));
    }
    return e;
}

ExprPtr Parser::parseAdd() {
    ExprPtr e = parseMul();
    while (e && (atPunct("+") || atPunct("-"))) {
        const Token& op = advance();
        ExprPtr rhs = parseMul();
        if (!rhs) return nullptr;
        SourceRange r = SourceRange::merge(e->range, rhs->range);
        e = std::make_unique<Binary>(r, op.text, std::move(e), std::move(rhs));
    }
    return e;
}

ExprPtr Parser::parseMul() {
    ExprPtr e = parseUnary();
    while (e && (atPunct("*") || atPunct("/") || atPunct("//") ||
                 atPunct("%") || atPunct("@"))) {
        const Token& op = advance();
        ExprPtr rhs = parseUnary();
        if (!rhs) return nullptr;
        SourceRange r = SourceRange::merge(e->range, rhs->range);
        e = std::make_unique<Binary>(r, op.text, std::move(e), std::move(rhs));
    }
    return e;
}

ExprPtr Parser::parseUnary() {
    if (atKeyword("await")) { // await 绑定紧于算术：await f() + 1 ≡ (await f()) + 1
        const Token& op = advance();
        ExprPtr operand = parseUnary();
        if (!operand) return nullptr;
        SourceRange r = SourceRange::merge(op.range, operand->range);
        return std::make_unique<Await>(r, std::move(operand));
    }
    if (atPunct("-") || atPunct("+")) {
        const Token& op = advance();
        ExprPtr operand = parseUnary();
        if (!operand) return nullptr;
        SourceRange r = SourceRange::merge(op.range, operand->range);
        return std::make_unique<Unary>(r, op.text, std::move(operand));
    }
    if (atKeyword("not")) return parseNot();

    ExprPtr base = parsePostfix();
    if (base && atPunct("**")) {
        advance();
        ExprPtr exp = parseUnary(); // 右结合，且允许 2 ** -1
        if (!exp) return nullptr;
        SourceRange r = SourceRange::merge(base->range, exp->range);
        return std::make_unique<Binary>(r, "**", std::move(base), std::move(exp));
    }
    return base;
}

ExprPtr Parser::parsePostfix() {
    ExprPtr e = parseAtom();
    for (;;) {
        if (!e) return nullptr;
        if (atPunct("(")) {
            advance();
            std::vector<ExprPtr> args;
            if (!atPunct(")")) {
                for (;;) {
                    // 调用不支持关键字实参（One Obvious Way：位置传参）。
                    if (cur().kind == TokKind::Ident &&
                        peekTok(1).kind == TokKind::Punct && peekTok(1).text == "=") {
                        diag_.error(Errc::E1007_UnsupportedFeature, file_,
                                    cur().range,
                                    "关键字实参暂不支持（@realtime 等装饰器除外）");
                    }
                    ExprPtr a = parseExpr();
                    if (!a) return nullptr;
                    args.push_back(std::move(a));
                    if (matchPunct(",")) {
                        if (atPunct(")")) break;
                        continue;
                    }
                    break;
                }
            }
            if (!expectPunct(")")) return nullptr;
            SourceRange r = SourceRange::merge(e->range, toks_[pos_ - 1].range);
            e = std::make_unique<Call>(r, std::move(e), std::move(args));
            continue;
        }
        if (atPunct("[")) {
            advance();
            std::vector<ExprPtr> idx;
            if (!atPunct("]")) {
                for (;;) {
                    ExprPtr a = parseExpr();
                    if (!a) return nullptr;
                    idx.push_back(std::move(a));
                    if (matchPunct(",")) {
                        if (atPunct("]")) break;
                        continue;
                    }
                    break;
                }
            }
            if (!expectPunct("]")) return nullptr;
            SourceRange r = SourceRange::merge(e->range, toks_[pos_ - 1].range);
            e = std::make_unique<Index>(r, std::move(e), std::move(idx));
            continue;
        }
        if (atPunct(".")) {
            advance();
            const Token* n = expectKind(TokKind::Ident, "成员名");
            if (!n) return nullptr;
            SourceRange r = SourceRange::merge(e->range, n->range);
            e = std::make_unique<Attr>(r, std::move(e), n->text);
            continue;
        }
        return e;
    }
}

ExprPtr Parser::parseAtom() {
    const Token& t = cur();
    switch (t.kind) {
    case TokKind::IntLit: {
        advance();
        int64_t v = std::strtoll(t.text.c_str(), nullptr, 10);
        return std::make_unique<IntLit>(t.range, v);
    }
    case TokKind::FloatLit: {
        advance();
        double v = std::strtod(t.text.c_str(), nullptr);
        return std::make_unique<FloatLit>(t.range, v);
    }
    case TokKind::StrLit:
        advance();
        return std::make_unique<StrLit>(t.range, t.text);
    case TokKind::Keyword:
        if (t.text == "True" || t.text == "False") {
            advance();
            return std::make_unique<BoolLit>(t.range, t.text == "True");
        }
        if (t.text == "None") {
            advance();
            return std::make_unique<NoneLit>(t.range);
        }
        return fail(t, "此处不应出现关键字 '" + t.text + "'");
    case TokKind::Ident:
        advance();
        return std::make_unique<Name>(t.range, t.text);
    case TokKind::Punct:
        if (t.text == "(") {
            advance();
            if (atPunct(")")) {
                advance();
                return std::make_unique<TupleLit>(t.range, std::vector<ExprPtr>{});
            }
            ExprPtr first = parseExpr();
            if (!first) return nullptr;
            if (atPunct(",")) { // 元组：值位置非法，仅形状注解路径合法
                std::vector<ExprPtr> elems;
                elems.push_back(std::move(first));
                while (matchPunct(",")) {
                    if (atPunct(")")) break;
                    ExprPtr e = parseExpr();
                    if (!e) return nullptr;
                    elems.push_back(std::move(e));
                }
                if (!expectPunct(")")) return nullptr;
                SourceRange r = SourceRange::merge(t.range, toks_[pos_ - 1].range);
                return std::make_unique<TupleLit>(r, std::move(elems));
            }
            if (!expectPunct(")")) return nullptr;
            return first;
        }
        if (t.text == "{") {
            diag_.error(Errc::E1007_UnsupportedFeature, file_, t.range,
                        "字典/集合字面量被拒绝（无宏/eval 清单；键值结构请用"
                        "类型注解与类表达）");
            return nullptr;
        }
        if (t.text == "[") {
            advance();
            std::vector<ExprPtr> elems;
            if (!atPunct("]")) {
                for (;;) {
                    ExprPtr e = parseExpr();
                    if (!e) return nullptr;
                    elems.push_back(std::move(e));
                    if (matchPunct(",")) {
                        if (atPunct("]")) break;
                        continue;
                    }
                    break;
                }
            }
            if (!expectPunct("]")) return nullptr;
            SourceRange r = SourceRange::merge(t.range, toks_[pos_ - 1].range);
            return std::make_unique<ListLit>(r, std::move(elems));
        }
        if (t.text == "{")
            return fail(t, "集合/字典字面量暂不支持（明确拒绝清单）");
        return fail(t, "意外的记号 " + tokenDesc(t));
    default:
        return fail(t, "意外的记号 " + tokenDesc(t));
    }
}

// ============================================================================
// 类型注解
// ============================================================================

TypeExprPtr Parser::parseTypeAnnotation() {
    const Token& t = cur();
    std::string name;
    if (t.kind == TokKind::Ident) {
        advance();
        name = t.text;
    } else if (t.kind == TokKind::Keyword && t.text == "None") {
        advance();
        name = "None";
    } else {
        fail(t, "期望类型注解，实际为 " + tokenDesc(t));
        return nullptr;
    }

    SourceRange r = t.range;
    if (!matchPunct("[")) {
        auto ty = std::make_unique<TypeExpr>(TypeExpr::Kind::Named, r);
        ty->name = name;
        return ty;
    }

    // 泛型：Tensor[elem, (dims)] 特判；其余 Named 泛型参数直接保留。
    if (name == "Tensor") {
        auto ty = std::make_unique<TypeExpr>(TypeExpr::Kind::Tensor, r);
        ty->name = "Tensor";
        auto elem = parseTypeAnnotation();
        if (!elem) return nullptr;
        ty->params.push_back(std::move(elem));
        if (!expectPunct(",")) return nullptr;

        // 形状元组：(2, 3, "?")
        if (!expectPunct("(")) return nullptr;
        for (;;) {
            TypeExpr::Dim dim;
            if (atPunct("?")) {
                advance();
                dim.isDynamic = true;
            } else {
                const Token* d = expectKind(TokKind::IntLit, "维度（整数或 '?'）");
                if (!d) return nullptr;
                dim.value = std::strtoll(d->text.c_str(), nullptr, 10);
            }
            ty->shape.push_back(dim);
            if (matchPunct(",")) {
                if (atPunct(")")) break;
                continue;
            }
            break;
        }
        if (!expectPunct(")")) return nullptr;
        if (!expectPunct("]")) return nullptr;
        ty->range = SourceRange::merge(r, toks_[pos_ - 1].range);
        return ty;
    }

    auto ty = std::make_unique<TypeExpr>(TypeExpr::Kind::Named, r);
    ty->name = name;
    for (;;) {
        auto p = parseTypeAnnotation();
        if (!p) return nullptr;
        ty->params.push_back(std::move(p));
        if (matchPunct(",")) {
            if (atPunct("]")) break; // 允许尾逗号
            continue;
        }
        break;
    }
    if (!expectPunct("]")) return nullptr;
    ty->range = SourceRange::merge(r, toks_[pos_ - 1].range);
    return ty;
}

// ============================================================================
// 契约提取（docstring requires/ensures/invariant）
// ============================================================================

void Parser::attachContract(FuncDef& def, ContractClause::Kind kind,
                            std::string_view clauseText, uint32_t searchFrom) {
    // 在源文本中定位子句原文，使诊断指回真实行号。
    const std::string& text = src_.file(file_).text;
    size_t found = text.find(clauseText, searchFrom);
    SourceRange range;
    if (found != std::string::npos &&
        found < def.docRange.offset + def.docRange.length) {
        range = SourceRange(static_cast<uint32_t>(found),
                            static_cast<uint32_t>(clauseText.size()));
        searchFrom = static_cast<uint32_t>(found + clauseText.size());
    } else {
        range = def.docRange; // 兜底：指向 docstring 整体
    }

    ExprPtr expr = parseExpressionOnly(src_, file_, diag_, range);
    if (!expr) return; // 子解析器已报错

    ContractClause c;
    c.kind = kind;
    c.expr = std::move(expr);
    c.range = range;
    def.contracts.push_back(std::move(c));
}

void Parser::extractContracts(FuncDef& def) {
    if (def.docstring.empty()) return;

    const std::string& s = def.docstring;
    size_t pos = 0;
    uint32_t searchFrom = def.docRange.offset;
    while (pos < s.size()) {
        size_t eol = s.find('\n', pos);
        std::string_view line =
            std::string_view(s).substr(pos, (eol == std::string::npos ? s.size() : eol) - pos);
        pos = (eol == std::string::npos) ? s.size() : eol + 1;

        // 去除首尾空白
        size_t b = 0, e = line.size();
        while (b < e && (line[b] == ' ' || line[b] == '\t' || line[b] == '\r')) ++b;
        while (e > b && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r')) --e;
        std::string_view trimmed = line.substr(b, e - b);

        struct Entry {
            std::string_view kw;
            ContractClause::Kind kind;
        };
        static const Entry kEntries[] = {
            {"requires:", ContractClause::Kind::Requires},
            {"ensures:", ContractClause::Kind::Ensures},
            {"invariant:", ContractClause::Kind::Invariant},
        };
        for (const auto& en : kEntries) {
            if (trimmed.starts_with(en.kw)) {
                std::string_view body = trimmed.substr(en.kw.size());
                size_t b2 = 0, e2 = body.size();
                while (b2 < e2 && (body[b2] == ' ' || body[b2] == '\t')) ++b2;
                while (e2 > b2 && (body[e2 - 1] == ' ' || body[e2 - 1] == '\t')) --e2;
                body = body.substr(b2, e2 - b2);
                if (body.empty()) {
                    diag_.error(Errc::E3002_ContractTypeError, file_,
                                def.docRange,
                                std::string(en.kw) + " 子句缺少表达式");
                    break;
                }
                attachContract(def, en.kind, body, searchFrom);
                break;
            }
        }
    }
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
