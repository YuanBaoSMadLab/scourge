// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 词法器（Python 风格缩进语法）
// ----------------------------------------------------------------------------
//  职责（设计文档第二章 Stage A）：
//  * 缩进块：行首空白宽度驱动 INDENT/DEDENT，仅允许空格（Tab 直接报错，
//    保证 AI 代理与人类看到同一份缩进语义）。
//  * 括号内隐式续行：() [] {} 中的换行不产生 NEWLINE/INDENT/DEDENT。
//  * 错误恢复：非法字符/坏字面量报错后跳过继续，不中断全文件切词——
//    “一次编译报出全部语法错误”（设计文档第三章）。
// ============================================================================
#pragma once

#include <vector>

#include "scourge/Common/Diagnostic.hpp"
#include "scourge/Syntax/Token.hpp"

namespace scourge {

class Lexer {
public:
    /// begin/end 限定切词区间（字节偏移，end 为Exclusive）；end=0 表示到文件尾。
    /// 契约子表达式解析（Parser::parseExpressionOnly）使用区间模式。
    Lexer(const SourceMgr& src, FileId file, DiagEngine& diag,
          uint32_t begin = 0, uint32_t end = 0);

    /// 全量切词；结果以 Eof 结尾。
    std::vector<Token> tokenize();

private:
    // ---- 字符工具 -----------------------------------------------------------
    char peek(size_t ahead = 0) const;
    char advance();
    bool atEnd() const;
    bool match(char c); ///< 若当前字符为 c 则消耗并返回 true

    // ---- 主循环 -------------------------------------------------------------
    void handleLineStart();     ///< 行首：缩进/空行/注释行处理
    void lexLine();             ///< 行体：逐记号切分
    void finishFile();          ///< EOF：补 NEWLINE + DEDENT + Eof

    // ---- 记号切分 -------------------------------------------------------------
    void lexString(char quote);
    void lexTripleString(char quote);
    void lexNumber();
    void lexIdent();
    bool lexOperator();         ///< 返回是否匹配到合法操作符

    // ---- 基础设施 -----------------------------------------------------------
    uint32_t pos() const { return pos_; }
    SourceRange makeRange(uint32_t begin) const {
        return SourceRange(begin, pos_ - begin);
    }
    void error(Errc code, uint32_t begin, std::string msg);
    void push(TokKind kind, uint32_t begin, std::string text);

    FileId file_;
    DiagEngine& diag_;
    const std::string& text_;

    uint32_t pos_ = 0;
    uint32_t end_ = 0;          ///< 切词区间上界（Exclusive）
    bool rangeMode_ = false;    ///< 区间模式：契约子表达式，不产 Newline/DEDENT
    int parenDepth_ = 0;
    bool atLineStart_ = true;
    std::vector<int> indentStack_{0};
    std::vector<Token> out_;
};

} // namespace scourge
