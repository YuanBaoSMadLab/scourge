// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 语法分析器 · 表达式（Parser 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：parseExpr 入口与优先级链（ternary < or < and < not < 比较 <
//  +- < */ < 一元 < ** < 后缀）、f-string 与 format 反糖、postfix 与 atom。
//  从 Parser.cpp 按生命周期拆出；语句与类型注解见 ParserStmt/ParserType.cpp
//  （拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/Syntax/Parser.hpp"

#include <cstdlib>

#include "scourge/Common/Capability.hpp"
#include "scourge/Syntax/Lexer.hpp"

namespace scourge {

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

// f-string 段游程 → 拼接链。语义契约：
//   * 字面段恒为 StrLit；表达式无 spec 恒经 str()（f"{x}" 与 str(x) 同一
//     AST 形态，Sema/编译零特判——转换错误沿用 str() 既有形态，双模一致
//     免费）；带格式说明符（0.9.9 f"{x:.2f}"）降糖为 fmt(expr, "spec")；
//   * 相邻多个 f-string 字面量（f"a{x}" f"b"）按 Python 语义合并处理；
//   * 全字面单段折叠为普通字符串字面量（f"abc" ≡ "abc"）。
ExprPtr Parser::parseFString() {
    std::vector<ExprPtr> parts;
    SourceRange range = cur().range;
    while (cur().kind == TokKind::FStrPart || cur().kind == TokKind::FStrExpr) {
        Token t = cur();
        advance();
        range = SourceRange::merge(range, t.range);
        if (t.kind == TokKind::FStrPart) {
            parts.push_back(std::make_unique<StrLit>(t.range, std::move(t.text)));
            continue;
        }
        // FStrExpr：range 指向花括号间原始源码，绝对字节偏移可直接喂给
        // 契约子表达式解析器（Lexer 区间模式，位置天然映射回源坐标）。
        // 格式说明符拆分（0.9.9）：在原始文本上找**顶层**首个 ':'（跟踪
        // {}[]() 深度并跳过引号串——f"{ {'a':1}['a']:>4 }" 的字典冒号不是
        // spec 分隔符），expr 部分保持绝对偏移可解析，spec 为其后全文。
        size_t colon = std::string::npos;
        {
            const std::string& s = t.text;
            int depth = 0;
            char quote = 0;
            for (size_t k = 0; k < s.size(); ++k) {
                char c = s[k];
                if (quote) {
                    if (c == '\\' && k + 1 < s.size()) { ++k; continue; }
                    if (c == quote) quote = 0;
                    continue;
                }
                if (c == '\'' || c == '"') { quote = c; continue; }
                if (c == '{' || c == '[' || c == '(') { ++depth; continue; }
                if (c == '}' || c == ']' || c == ')') { --depth; continue; }
                if (c == ':' && depth == 0) { colon = k; break; }
            }
        }
        ExprPtr e;
        if (colon == std::string::npos) {
            e = parseExpressionOnly(src_, file_, diag_, t.range);
            if (!e) return nullptr; // 子解析器已报错
            std::vector<ExprPtr> callArgs;
            callArgs.push_back(std::move(e));
            parts.push_back(std::make_unique<Call>(
                t.range, std::make_unique<Name>(t.range, "str"),
                std::move(callArgs)));
        } else {
            SourceRange exprRange(t.range.offset,
                                  static_cast<uint32_t>(colon));
            e = parseExpressionOnly(src_, file_, diag_, exprRange);
            if (!e) return nullptr; // 子解析器已报错
            std::string spec = t.text.substr(colon + 1);
            std::vector<ExprPtr> callArgs;
            callArgs.push_back(std::move(e));
            callArgs.push_back(
                std::make_unique<StrLit>(t.range, std::move(spec)));
            parts.push_back(std::make_unique<Call>(
                t.range, std::make_unique<Name>(t.range, "fmt"),
                std::move(callArgs)));
        }
    }
    return buildStrChain(std::move(parts), range);
}

ExprPtr Parser::buildStrChain(std::vector<ExprPtr> parts, SourceRange range) {
    if (parts.empty())
        return std::make_unique<StrLit>(range, ""); // 词法保证 ≥1 段；防御空 f""
    if (parts.size() == 1) return std::move(parts[0]);
    ExprPtr acc = std::move(parts[0]);
    for (size_t k = 1; k < parts.size(); ++k) {
        SourceRange r = SourceRange::merge(acc->range, parts[k]->range);
        acc = std::make_unique<Binary>(r, "+", std::move(acc), std::move(parts[k]));
    }
    return acc;
}

// format(fmt, x, ...) 编译期展开：{{ }} → 字面 { }，{} → str(实参) 占位，
// 与 f-string 共用 buildStrChain（单一降糖出口）。占位符数与实参数严格
// 一致（One Obvious Way：多余实参不静默忽略）。内建为保留字（Sema 中内建
// 优先于用户函数），无遮蔽歧义。
ExprPtr Parser::maybeDesugarFormat(std::unique_ptr<Call> c) {
    if (c->args.empty() || c->callee->kind != Expr::Kind::Name ||
        static_cast<const Name&>(*c->callee).name != "format" ||
        c->args[0]->kind != Expr::Kind::StrLit)
        return c; // 非字面量首参：原样透传，Sema format 分支定向报错

    const StrLit* fmt = static_cast<const StrLit*>(c->args[0].get());
    const std::string& s = fmt->strValue;
    std::vector<ExprPtr> parts;
    std::string lit;
    size_t i = 0, used = 1; // used = 已消费实参数（含 fmt 自身）
    auto flushLit = [&] {
        if (!lit.empty())
            parts.push_back(std::make_unique<StrLit>(fmt->range, std::move(lit)));
        else
            lit.clear();
    };
    while (i < s.size()) {
        char ch = s[i++];
        if (ch != '{' && ch != '}') {
            lit += ch;
            continue;
        }
        if (i < s.size() && s[i] == ch) { // {{ / }}
            lit += ch;
            ++i;
            continue;
        }
        if (ch == '}') {
            diag_.error(Errc::E1007_UnsupportedFeature, file_, fmt->range,
                        "format() 字面量单个 '}' 非法（字面 '}' 请写 '}}'）");
            return nullptr;
        }
        if (i >= s.size()) {
            diag_.error(Errc::E1007_UnsupportedFeature, file_, fmt->range,
                        "format() 字面量存在未闭合的 '{'");
            return nullptr;
        }
        if (s[i] != '}') { // 匿名占位恒为 "{}"；非 '}' 说明 '}' 已成对折叠过
            diag_.error(Errc::E1007_UnsupportedFeature, file_, fmt->range,
                        "format() 字面量非法占位符（仅支持 '{}'）");
            return nullptr;
        }
        ++i; // 消耗占位符闭括号
        if (used >= c->args.size()) {
            diag_.error(Errc::E1007_UnsupportedFeature, file_, c->range,
                        "format() 占位符多于实参");
            return nullptr;
        }
        flushLit();
        std::vector<ExprPtr> one;
        one.push_back(std::move(c->args[used++]));
        parts.push_back(std::make_unique<Call>(
            fmt->range, std::make_unique<Name>(fmt->range, "str"),
            std::move(one)));
    }
    flushLit();
    if (used != c->args.size()) {
        diag_.error(Errc::E1007_UnsupportedFeature, file_, c->range,
                    "format() 占位符 " + std::to_string(used - 1) +
                        " 个与实参 " + std::to_string(c->args.size() - 1) +
                        " 个不一致");
        return nullptr;
    }
    return buildStrChain(std::move(parts), c->range);
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
            e = maybeDesugarFormat(std::make_unique<Call>(r, std::move(e),
                                                          std::move(args)));
            continue;
        }
        if (atPunct("[")) {
            advance();
            // 切片 s[a:b]（0.9.9）：`[` 后首个 token 是 `:` → 缺 start；
            // 表达式后遇 `:` → 切片并按需补缺 end。三元走 if/else 关键字
            // 不吃 `:`，故期望文法安全；步长（第二个 `:`）显式拒绝；
            // 切片与多维逗号下标（张量 m[i, j]）互斥。
            bool isSlice = false;
            std::vector<ExprPtr> idx;
            if (!atPunct("]")) {
                if (atPunct(":")) {
                    advance();
                    isSlice = true;
                    idx.push_back(nullptr); // start 缺省
                    idx.push_back(nullptr);
                } else {
                    ExprPtr a = parseExpr();
                    if (!a) return nullptr;
                    if (atPunct(":")) {
                        advance();
                        isSlice = true;
                        idx.push_back(std::move(a));
                        idx.push_back(nullptr); // end 占位（可能缺省）
                    } else {
                        idx.push_back(std::move(a));
                        while (matchPunct(",")) { // 多维：张量专属
                            if (atPunct("]")) break;
                            ExprPtr b2 = parseExpr();
                            if (!b2) return nullptr;
                            idx.push_back(std::move(b2));
                        }
                    }
                }
                if (isSlice && !atPunct("]")) {
                    idx[1] = parseExpr();
                    if (!idx[1]) return nullptr;
                    if (atPunct(":")) {
                        diag_.error(Errc::E1007_UnsupportedFeature, file_,
                                    cur().range,
                                    "切片不支持步长（s[a:b:c]）；请用循环"
                                    "显式取步长子序列");
                        return nullptr;
                    }
                }
            }
            if (!expectPunct("]")) return nullptr;
            SourceRange r = SourceRange::merge(e->range, toks_[pos_ - 1].range);
            auto node = std::make_unique<Index>(r, std::move(e), std::move(idx));
            node->isSlice = isSlice;
            e = std::move(node);
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
    case TokKind::FStrPart:
    case TokKind::FStrExpr:
        return parseFString();
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
            // 0.9.3 字典/集合字面量：{k: v, ...} 或 {e1, e2, ...}（首元素后
            // 冒号 = 字典、逗号/收括号 = 集合）；空 {} 恒为字典（类型由注解
            // 推断，Set 注解上下文由检查器判为空集合）。
            advance();
            if (atPunct("}")) {
                advance();
                SourceRange r0 = SourceRange::merge(t.range, toks_[pos_ - 1].range);
                return std::make_unique<DictLit>(r0,
                                                 std::vector<std::pair<ExprPtr, ExprPtr>>{});
            }
            ExprPtr first = parseExpr();
            if (!first) return nullptr;
            if (atPunct(":")) { // 字典路径
                std::vector<std::pair<ExprPtr, ExprPtr>> pairs;
                for (;;) {
                    if (!expectPunct(":")) return nullptr;
                    ExprPtr v = parseExpr();
                    if (!v) return nullptr;
                    pairs.emplace_back(std::move(first), std::move(v));
                    if (matchPunct(",")) {
                        if (atPunct("}")) break;
                        first = parseExpr();
                        if (!first) return nullptr;
                        continue;
                    }
                    break;
                }
                if (!expectPunct("}")) return nullptr;
                SourceRange r = SourceRange::merge(t.range, toks_[pos_ - 1].range);
                return std::make_unique<DictLit>(r, std::move(pairs));
            }
            // 集合路径：{e1, e2, ...}（首元素已解析）
            std::vector<ExprPtr> elems;
            elems.push_back(std::move(first));
            while (matchPunct(",")) {
                if (atPunct("}")) break;
                ExprPtr e = parseExpr();
                if (!e) return nullptr;
                elems.push_back(std::move(e));
            }
            if (!expectPunct("}")) return nullptr;
            SourceRange r = SourceRange::merge(t.range, toks_[pos_ - 1].range);
            return std::make_unique<SetLit>(r, std::move(elems));
        }
        if (t.text == "[") {
            advance();
            ExprPtr first = atPunct("]") ? nullptr : parseExpr();
            // 列表推导式（0.9.8）：[elem for x in iter (if cond)?]。存在
            // 判据 = 首元素后跟 for 关键字——for 为语句关键字，普通表达式
            // 内非法（parseExpr 已拒），peek 无歧义。
            if (first && cur().kind == TokKind::Keyword && cur().text == "for") {
                advance();
                if (cur().kind != TokKind::Ident)
                    return fail(cur(), "推导式循环变量必须是标识符");
                Token varT = cur();
                advance();
                if (!(cur().kind == TokKind::Keyword && cur().text == "in"))
                    return fail(cur(), "推导式预期 'in'");
                advance();
                // iter 与过滤条件用 parseOr（不吃三元尾巴）：否则 iter 后
                // 的过滤 'if' 会被吞成三元导致强制 else。需要三元迭代源
                // 时用括号包住（括号内完整 parseExpr）。
                ExprPtr it = parseOr();
                if (!it) return nullptr;
                ExprPtr cond;
                if (cur().kind == TokKind::Keyword && cur().text == "if") {
                    advance();
                    cond = parseOr();
                    if (!cond) return nullptr;
                }
                if (!expectPunct("]")) return nullptr;
                SourceRange r = SourceRange::merge(t.range, toks_[pos_ - 1].range);
                return std::make_unique<ListComp>(r, std::move(first),
                                                  varT.text, varT.range,
                                                  std::move(it),
                                                  std::move(cond));
            }
            std::vector<ExprPtr> elems;
            if (first) {
                elems.push_back(std::move(first));
                if (matchPunct(",")) {
                    while (!atPunct("]")) {
                        ExprPtr e = parseExpr();
                        if (!e) return nullptr;
                        elems.push_back(std::move(e));
                        if (!matchPunct(",")) break;
                    }
                }
            }
            if (!expectPunct("]")) return nullptr;
            SourceRange r = SourceRange::merge(t.range, toks_[pos_ - 1].range);
            return std::make_unique<ListLit>(r, std::move(elems));
        }
        return fail(t, "意外的记号 " + tokenDesc(t));
    default:
        return fail(t, "意外的记号 " + tokenDesc(t));
    }
}

} // namespace scourge
