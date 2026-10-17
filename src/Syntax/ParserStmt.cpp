// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 语法分析器 · 语句（Parser 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：parseDecorated/parseDef（装饰器与函数定义）、parseIf/While/For/
//  Return/Assert/Try/Defer/Import（控制流与导入）、parseCapability/parseSimple
//  （能力声明与赋值/表达式语句）。从 Parser.cpp 按生命周期拆出；
//  记号衔接与模块骨架见 Parser.cpp，表达式见 ParserExpr.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/Syntax/Parser.hpp"

#include <cstdlib>

#include "scourge/Common/Capability.hpp"
#include "scourge/Syntax/Lexer.hpp"

namespace scourge {

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
            // 变参标记 ...（O 节 FFI 宽类型域）：循环顶部检查——`a, ...`
            // 与 `(...)` 首位两种到达路径统一在此收口，后者顺带产出
            // "至少一个命名参数"的 C ABI 就地报（比"期望参数名"更可懂）。
            // "仅 @extern 合法"由检查器把关。
            if (atPunct("...")) {
                advance();
                def->isVariadic = true;
                if (def->params.empty())
                    diag_.error(Errc::E1007_UnsupportedFeature, file_,
                                cur().range,
                                "变参标记 ... 前必须至少有一个命名参数"
                                "（C ABI 要求）");
                if (!atPunct(")"))
                    diag_.error(Errc::E1007_UnsupportedFeature, file_,
                                cur().range, "变参标记 ... 必须是参数表最后一项");
                break;
            }
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

StmtPtr Parser::parseDefer() {
    const Token& kw = advance(); // defer
    StmtList body = parseBlock();
    if (body.empty() && hasErrors()) return nullptr;
    // 仅函数体顶层的限制由检查器判定（Parser 无函数嵌套上下文）。
    return std::make_unique<Defer>(kw.range, std::move(body));
}

StmtPtr Parser::parseImport() {
    const Token& kw = advance(); // import / from

    // 点分模块路径（0.9.6）：Ident ("." Ident)*；目录即命名空间，
    // a.b 解析为 <dir>/a/b.sg。首段必须是 Ident，后续段同。
    std::vector<std::string> path;
    const Token* seg = expectKind(TokKind::Ident, "模块名");
    if (!seg) return nullptr;
    path.push_back(seg->text);
    while (matchPunct(".")) {
        const Token* nxt = expectKind(TokKind::Ident, "模块路径段");
        if (!nxt) return nullptr;
        path.push_back(nxt->text);
    }

    if (!kw.text.empty() && kw.text == "from") {
        if (!expectKeyword("import")) return nullptr;
        std::vector<Import::Item> items;
        for (;;) {
            const Token* n = expectKind(TokKind::Ident, "导入名");
            if (!n) return nullptr;
            Import::Item item{n->text, {}};
            if (matchKeyword("as")) {
                const Token* a = expectKind(TokKind::Ident, "别名");
                if (!a) return nullptr;
                item.alias = a->text;
            }
            items.push_back(std::move(item));
            if (matchPunct(",")) continue;
            break;
        }
        endOfStatement();
        return std::make_unique<Import>(kw.range, std::move(path), true,
                                        std::move(items));
    }

    // import a.b [as c]：别名绑定 c（Python 语义），首段与末段宽容绑定
    // 由检查器完成（AI 幻觉写法 b.f() 同样可编译）。
    std::string alias;
    if (matchKeyword("as")) {
        const Token* a = expectKind(TokKind::Ident, "别名");
        if (!a) return nullptr;
        alias = a->text;
    }
    endOfStatement();
    return std::make_unique<Import>(kw.range, std::move(path), false,
                                    std::vector<Import::Item>{},
                                    std::move(alias));
}

// ----------------------------------------------------------------------------
// 简单语句：赋值 / 增强赋值 / 表达式语句
// ----------------------------------------------------------------------------

StmtPtr Parser::parseCapability() {
    const Token& kw = advance(); // capability
    std::vector<Capability::Item> caps;
    for (;;) {
        const Token* n = expectKind(TokKind::Ident, "能力域名");
        if (!n) return nullptr;
        Cap cap;
        if (!capFromName(n->text, cap)) {
            diag_.error(Errc::E1005_UnexpectedToken, file_, n->range,
                        "未知能力域名 '" + n->text +
                            "'（可用：io net env proc time signal ffi）");
            return nullptr;
        }
        caps.push_back({n->text, n->range});
        if (matchPunct(",")) continue;
        break;
    }
    endOfStatement();
    return std::make_unique<Capability>(kw.range, std::move(caps));
}

StmtPtr Parser::parseSimple() {
    // capability 软关键字：仅当后随域名 Ident 时按能力声明解析；块内出现
    // 由检查器拒绝（声明只属模块顶层）。软关键字保证既有标识符不破坏。
    if (at(TokKind::Ident) && cur().text == "capability" &&
        peekTok(1).kind == TokKind::Ident)
        return parseCapability();
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

} // namespace scourge
