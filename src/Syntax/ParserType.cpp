// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 语法分析器 · 类型注解与契约提取（Parser 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：parseTypeAnnotation（标量/张量/容器注解语法）与
//  attachContract/extractContracts（docstring requires/ensures/invariant
//  提取为契约子句）。从 Parser.cpp 按生命周期拆出（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/Syntax/Parser.hpp"

#include <cstdlib>

#include "scourge/Common/Capability.hpp"
#include "scourge/Syntax/Lexer.hpp"

namespace scourge {

// ============================================================================
// 类型注解
// ============================================================================

TypeExprPtr Parser::parseTypeAnnotation() {
    const Token& t = cur();
    std::string name;

    // 元组类型注解 (T1, T2, ...)（0.9.6）：单元素 (int,) 允许尾逗号。
    if (t.kind == TokKind::Punct && t.text == "(") {
        advance();
        auto ty = std::make_unique<TypeExpr>(TypeExpr::Kind::Tuple, t.range);
        if (!atPunct(")")) {
            for (;;) {
                TypeExprPtr el = parseTypeAnnotation();
                if (!el) return nullptr;
                ty->params.push_back(std::move(el));
                if (matchPunct(",")) {
                    if (atPunct(")")) break; // 尾逗号
                    continue;
                }
                break;
            }
        }
        if (!expectPunct(")")) return nullptr;
        return ty;
    }

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

} // namespace scourge
