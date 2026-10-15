// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 词法器实现
// ============================================================================
#include "scourge/Syntax/Lexer.hpp"

#include <cctype>
#include <cstring>

namespace scourge {

Lexer::Lexer(const SourceMgr& src, FileId file, DiagEngine& diag,
             uint32_t begin, uint32_t end)
    : file_(file), diag_(diag), text_(src.file(file).text) {
    pos_ = begin <= text_.size() ? begin : static_cast<uint32_t>(text_.size());
    end_ = (end == 0 || end > text_.size()) ? static_cast<uint32_t>(text_.size())
                                            : end;
    rangeMode_ = (pos_ > 0 || end_ < text_.size());
}

// ----------------------------------------------------------------------------
// 字符工具
// ----------------------------------------------------------------------------
char Lexer::peek(size_t ahead) const {
    size_t i = pos_ + ahead;
    return i < end_ ? text_[i] : '\0';
}

char Lexer::advance() {
    char c = peek();
    if (pos_ < end_) ++pos_;
    return c;
}

bool Lexer::atEnd() const { return pos_ >= end_; }

bool Lexer::match(char c) {
    if (peek() == c) {
        ++pos_;
        return true;
    }
    return false;
}

void Lexer::error(Errc code, uint32_t begin, std::string msg) {
    diag_.error(code, file_, SourceRange(begin, pos_ > begin ? pos_ - begin : 1),
                std::move(msg));
}

void Lexer::push(TokKind kind, uint32_t begin, std::string text) {
    Token t;
    t.kind = kind;
    t.range = makeRange(begin);
    t.text = std::move(text);
    out_.push_back(std::move(t));
}

// ----------------------------------------------------------------------------
// 行首处理：缩进驱动 INDENT/DEDENT
// ----------------------------------------------------------------------------
void Lexer::handleLineStart() {
    // 跳过空白并统计缩进宽度；括号续行内不处理缩进。
    int width = 0;
    while (peek() == ' ' || peek() == '\t') {
        if (peek() == '\t') {
            uint32_t begin = pos_;
            advance();
            diag_.error(Errc::E1004_BadIndentation, file_,
                        SourceRange(begin, 1), "缩进使用 Tab；Scourge 只允许空格缩进");
            ++width; // 恢复：按 1 列继续
        } else {
            advance();
            ++width;
        }
    }

    // 空行 / 注释行：不产生任何记号，也不影响缩进栈。
    if (atEnd() || peek() == '\n' || peek() == '#') {
        while (!atEnd() && peek() != '\n') advance();
        if (!atEnd()) advance(); // 消耗换行符，避免下一轮空转
        return;
    }

    if (parenDepth_ > 0) { // 防御：括号内无缩进语义（正常不可达）
        atLineStart_ = false;
        return;
    }

    int top = indentStack_.back();
    if (width > top) {
        indentStack_.push_back(width);
        push(TokKind::Indent, pos_, "");
    } else if (width < top) {
        while (indentStack_.size() > 1 && indentStack_.back() > width) {
            indentStack_.pop_back();
            push(TokKind::Dedent, pos_, "");
        }
        if (indentStack_.back() != width) {
            diag_.error(Errc::E1004_BadIndentation, file_,
                        SourceRange(pos_, 1),
                        "缩进宽度 " + std::to_string(width) +
                            " 与任何外层缩进层级不匹配（期望 " +
                            std::to_string(indentStack_.back()) + "）");
            indentStack_.push_back(width); // 恢复：接受当前宽度继续
        }
    }
    atLineStart_ = false;
}

// ----------------------------------------------------------------------------
// 字符串
// ----------------------------------------------------------------------------
void Lexer::lexString(char quote) {
    uint32_t begin = pos_ - 1; // 含开引号
    std::string value;
    for (;;) {
        if (atEnd() || peek() == '\n') {
            error(Errc::E1002_UnterminatedString, begin, "字符串未在行尾前闭合");
            break;
        }
        char c = advance();
        if (c == quote) break;
        if (c == '\\') {
            if (atEnd() || peek() == '\n') {
                error(Errc::E1002_UnterminatedString, begin, "字符串未在行尾前闭合");
                break;
            }
            char e = advance();
            switch (e) {
            case 'n': value += '\n'; break;
            case 't': value += '\t'; break;
            case 'r': value += '\r'; break;
            case '0': value += '\0'; break;
            case '\\': value += '\\'; break;
            case '\'': value += '\''; break;
            case '"': value += '"'; break;
            default: {
                uint32_t eb = pos_ - 2;
                diag_.error(Errc::E1001_IllegalChar, file_, SourceRange(eb, 2),
                            std::string("非法转义 \\") + e);
                value += e; // 恢复：按字面字符继续
            }
            }
        } else {
            value += c;
        }
    }
    push(TokKind::StrLit, begin, std::move(value));
}

void Lexer::lexTripleString(char quote) {
    uint32_t begin = pos_ - 3;
    std::string value;
    for (;;) {
        if (atEnd()) {
            error(Errc::E1002_UnterminatedString, begin, "三引号字符串未闭合");
            break;
        }
        if (peek() == quote && peek(1) == quote && peek(2) == quote) {
            advance(); advance(); advance();
            break;
        }
        char c = advance();
        if (c == '\\' && !atEnd()) {
            char e = advance();
            switch (e) {
            case 'n': value += '\n'; break;
            case 't': value += '\t'; break;
            case 'r': value += '\r'; break;
            case '\\': value += '\\'; break;
            case '\'': value += '\''; break;
            case '"': value += '"'; break;
            default: value += e;
            }
        } else {
            value += c;
        }
    }
    push(TokKind::StrLit, begin, std::move(value));
}

// ----------------------------------------------------------------------------
// 数字
// ----------------------------------------------------------------------------
void Lexer::lexNumber() {
    uint32_t begin = pos_;
    bool isFloat = false;

    // 进制前缀：显式拒绝（保持字面量面最小、确定）。
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'o' || peek(1) == 'b')) {
        advance(); advance();
        while (isalnum(static_cast<unsigned char>(peek()))) advance();
        error(Errc::E1003_BadNumber, begin,
              "十六进制/八进制/二进制字面量暂不支持，请使用十进制");
        push(TokKind::IntLit, begin, "0"); // 恢复：占位 0，文本已报错
        return;
    }

    while (isdigit(static_cast<unsigned char>(peek()))) advance();

    if (peek() == '_') { // 数字分隔符：拒绝
        while (isalnum(static_cast<unsigned char>(peek())) || peek() == '_') advance();
        error(Errc::E1003_BadNumber, begin, "数字分隔符 _ 暂不支持");
        push(TokKind::IntLit, begin, "0"); // 恢复：占位 0
        return;
    }

    if (peek() == '.') {
        if (!isdigit(static_cast<unsigned char>(peek(1)))) {
            // "1." 或 "1.foo"：拒绝裸小数点，避免与成员访问歧义。
            advance();
            error(Errc::E1003_BadNumber, begin, "小数点后缺少数字");
            push(TokKind::FloatLit, begin, text_.substr(begin, pos_ - begin));
            return;
        }
        isFloat = true;
        advance();
        while (isdigit(static_cast<unsigned char>(peek()))) advance();
    }

    if (peek() == 'e' || peek() == 'E') {
        size_t save = pos_;
        advance();
        if (peek() == '+' || peek() == '-') advance();
        if (isdigit(static_cast<unsigned char>(peek()))) {
            isFloat = true;
            while (isdigit(static_cast<unsigned char>(peek()))) advance();
        } else {
            pos_ = static_cast<uint32_t>(save); // 不是指数，回退交给后续记号
        }
    }

    push(isFloat ? TokKind::FloatLit : TokKind::IntLit, begin,
         text_.substr(begin, pos_ - begin));
}

// ----------------------------------------------------------------------------
// 标识符 / 关键字 / 字符串前缀
// ----------------------------------------------------------------------------
void Lexer::lexIdent() {
    uint32_t begin = pos_;
    while (isalnum(static_cast<unsigned char>(peek())) || peek() == '_') advance();
    std::string word = text_.substr(begin, pos_ - begin);

    // 字符串前缀（f/b/r 等）：明确拒绝，保持字符串语义唯一（设计文档第三章）。
    bool allPrefixChars = !word.empty() && word.size() <= 2;
    for (char c : word)
        if (!strchr("rRfFbB", c)) allPrefixChars = false;
    if (allPrefixChars && (peek() == '"' || peek() == '\'') && word != "_") {
        diag_.error(Errc::E1007_UnsupportedFeature, file_,
                    SourceRange(begin, static_cast<uint32_t>(word.size())),
                    "字符串前缀 " + word + " 暂不支持（明确拒绝清单）");
        // 继续按普通字符串切词，尽量减少连锁错误。
    }

    if (isKeyword(word)) {
        push(TokKind::Keyword, begin, std::move(word));
    } else {
        push(TokKind::Ident, begin, std::move(word));
    }
}

// ----------------------------------------------------------------------------
// 操作符（最长匹配优先）
// ----------------------------------------------------------------------------
bool Lexer::lexOperator() {
    static const char* kOps3[] = {"**=", "//=", "...", nullptr};
    static const char* kOps2[] = {
        "**", "//", "->", "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=",
        "%=", "@=", nullptr};
    static const char* kOps1[] = {
        "+", "-", "*", "/", "%", "@", "=", "<", ">", "(", ")", "[", "]",
        "{", "}", ",", ":", ".", "?", nullptr};

    uint32_t begin = pos_;
    for (int i = 0; kOps3[i]; ++i)
        if (text_.compare(pos_, 3, kOps3[i]) == 0) {
            pos_ += 3;
            push(TokKind::Punct, begin, kOps3[i]);
            return true;
        }
    for (int i = 0; kOps2[i]; ++i)
        if (text_.compare(pos_, 2, kOps2[i]) == 0) {
            pos_ += 2;
            push(TokKind::Punct, begin, kOps2[i]);
            return true;
        }
    for (int i = 0; kOps1[i]; ++i)
        if (peek() == kOps1[i][0] && kOps1[i][1] == '\0') {
            advance();
            // D1：括号深度计数——括号内换行由 lexLine 静默吞掉（隐式续行）。
            switch (kOps1[i][0]) {
            case '(': case '[': case '{': ++parenDepth_; break;
            case ')': case ']': case '}':
                if (parenDepth_ > 0) --parenDepth_;
                break;
            default: break;
            }
            push(TokKind::Punct, begin, kOps1[i]);
            return true;
        }
    return false;
}

// ----------------------------------------------------------------------------
// 行体
// ----------------------------------------------------------------------------
void Lexer::lexLine() {
    for (;;) {
        char c = peek();
        if (c == '\0') return;

        if (c == ' ' || c == '\t') { // 行中空白
            advance();
            continue;
        }
        if (c == '#') { // 行注释
            while (!atEnd() && peek() != '\n') advance();
            continue;
        }
        if (c == '\n') {
            advance();
            if (parenDepth_ == 0) {
                push(TokKind::Newline, pos_ - 1, "");
                atLineStart_ = true;
            }
            return;
        }
        if (c == '"' || c == '\'') {
            advance();
            if (peek() == c && peek(1) == c) {
                advance(); advance();
                lexTripleString(c);
            } else {
                lexString(c);
            }
            continue;
        }
        if (isdigit(static_cast<unsigned char>(c))) {
            lexNumber();
            continue;
        }
        if (isalpha(static_cast<unsigned char>(c)) || c == '_') {
            lexIdent();
            continue;
        }
        if (lexOperator()) continue;

        // 未识别字符：报错跳过（错误恢复核心路径）。
        uint32_t begin = pos_;
        advance();
        std::string msg = "非法字符 '";
        msg += (c >= 0x20 && static_cast<unsigned char>(c) < 0x80) ? c : '?';
        msg += "'";
        if (static_cast<unsigned char>(c) >= 0x80)
            msg += "（标识符仅支持 ASCII；注释内可使用任意 UTF-8 文本）";
        diag_.error(Errc::E1001_IllegalChar, file_, SourceRange(begin, 1), msg);
    }
}

// ----------------------------------------------------------------------------
// EOF 收尾
// ----------------------------------------------------------------------------
void Lexer::finishFile() {
    if (rangeMode_) {
        // 区间模式（契约子表达式）：只补 Eof，不补行边界与缩进。
        Token eof;
        eof.kind = TokKind::Eof;
        eof.range = SourceRange(pos_, 0);
        out_.push_back(std::move(eof));
        return;
    }
    // 末行无换行时补一个 NEWLINE，保证语句边界完整。
    if (!out_.empty() && out_.back().kind != TokKind::Newline &&
        out_.back().kind != TokKind::Dedent) {
        push(TokKind::Newline, pos_, "");
    }
    while (indentStack_.size() > 1) {
        indentStack_.pop_back();
        push(TokKind::Dedent, pos_, "");
    }
    Token eof;
    eof.kind = TokKind::Eof;
    eof.range = SourceRange(pos_, 0);
    out_.push_back(std::move(eof));
}

std::vector<Token> Lexer::tokenize() {
    while (!atEnd()) {
        if (atLineStart_) handleLineStart();
        else lexLine();
    }
    finishFile();
    return out_;
}

} // namespace scourge
