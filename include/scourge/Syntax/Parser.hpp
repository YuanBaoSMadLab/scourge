// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 语法分析器（手写递归下降，设计文档第二章 Stage A）
// ----------------------------------------------------------------------------
//  错误恢复策略（设计文档第三章“一次报出全部语法错误”）：
//  * 表达式级失败 → 报错返回 nullptr，语句级向语句边界同步（行尾/DEDENT）；
//  * 词法错误已在 Lexer 内恢复，Parser 只处理语法面；
//  * 不使用异常：以 nullptr 传播失败，保持控制流显式可推理。
// ============================================================================
#pragma once

#include <vector>

#include "scourge/Common/Diagnostic.hpp"
#include "scourge/Syntax/AST.hpp"
#include "scourge/Syntax/Token.hpp"

namespace scourge {

class Parser {
public:
    Parser(const SourceMgr& src, FileId file, DiagEngine& diag,
           std::vector<Token> tokens);

    /// 解析完整模块。
    std::unique_ptr<Module> parseModule(std::string moduleName);

    /// 独立入口：对源码区间（契约子句）切词并解析单个表达式。
    static ExprPtr parseExpressionOnly(const SourceMgr& src, FileId file,
                                       DiagEngine& diag, SourceRange range);

private:
    // ---- 记号流工具 -----------------------------------------------------------
    const Token& cur() const { return toks_[pos_]; }
    const Token& peekTok(size_t n) const;
    const Token& advance();          ///< 返回当前并前进（Eof 处停留）
    bool at(TokKind k) const;
    bool atEof() const { return at(TokKind::Eof); }
    bool atPunct(std::string_view op) const;
    bool atKeyword(std::string_view kw) const;
    bool matchPunct(std::string_view op);
    bool matchKeyword(std::string_view kw);

    /// 期望一个记号；失败时报 E1005 并返回 nullptr。
    const Token* expectPunct(std::string_view op);
    const Token* expectKeyword(std::string_view kw);
    const Token* expectKind(TokKind k, std::string_view what);

    static std::string tokenDesc(const Token& t);

    // ---- 语句 ---------------------------------------------------------------
    StmtPtr parseStatement();
    StmtPtr parseDecorated();   ///< @ 装饰器行 + def / async def
    StmtPtr parseDef(std::vector<Decorator> decorators, bool isAsync = false);
    StmtPtr parseIf();
    StmtPtr parseWhile();
    StmtPtr parseFor();
    StmtPtr parseSimple();      ///< 赋值/增强赋值/表达式语句
    StmtPtr parseReturn();
    StmtPtr parseImport();      ///< import / from-import
    StmtPtr parseCapability();  ///< capability 域声明（0.9.7，软关键字）
    StmtPtr parseAssert();
    StmtPtr parseTry();         ///< try/except（panic 恢复路径）
    StmtPtr parseDefer();       ///< defer（函数级清理钩子，函数体顶层）
    StmtList parseBlock();      ///< 消耗 ': INDENT … DEDENT'
    void endOfStatement();      ///< 语句尾：消耗换行（缺失即报错并恢复）
    void syncToStatementBoundary();

    // ---- 表达式（优先级爬升）--------------------------------------------------
    ExprPtr parseExpr();        ///< 入口：三元 a if c else b
    ExprPtr parseOr();
    ExprPtr parseAnd();
    ExprPtr parseNot();
    ExprPtr parseComparison();
    ExprPtr parseAdd();
    ExprPtr parseMul();
    ExprPtr parseUnary();
    ExprPtr parsePostfix();
    ExprPtr parseAtom();
    ExprPtr parseCallArgs();    ///< '(' 实参列表 ')'
    std::vector<ExprPtr> parseParenElems(); ///< '(' expr, … ')' 通用列表

    // ---- 类型注解 -------------------------------------------------------------
    TypeExprPtr parseTypeAnnotation();

    // ---- 契约 -----------------------------------------------------------------
    void extractContracts(FuncDef& def);
    void attachContract(FuncDef& def, ContractClause::Kind kind,
                        std::string_view clauseText, uint32_t searchFrom);

    // ---- 其他 -----------------------------------------------------------------
    bool isConstLiteral(const Expr* e) const; ///< 默认参数校验
    bool hasErrors() const;                   ///< 诊断引擎是否已报错
    ExprPtr fail(const Token& at, std::string msg); ///< 报错并返回 nullptr

    const SourceMgr& src_;
    FileId file_;
    DiagEngine& diag_;
    std::vector<Token> toks_;
    size_t pos_ = 0;
    int depth_ = 0;             ///< 表达式递归深度护栏

    static constexpr int kMaxDepth = 200;
};

} // namespace scourge
