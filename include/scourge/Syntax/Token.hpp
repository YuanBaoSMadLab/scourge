// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — Token 与词法单元定义
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "scourge/Common/Source.hpp"

namespace scourge {

enum class TokKind : uint8_t {
    Eof,       ///< 文件结束哨兵
    Newline,   ///< 逻辑行结束（括号内不产生）
    Indent,    ///< 缩进层级加深
    Dedent,    ///< 缩进层级回退（可连续多个）
    Ident,     ///< 标识符
    Keyword,   ///< 关键字（text 保存关键字本身）
    IntLit,    ///< 整数字面量（text 为十进制形式）
    FloatLit,  ///< 浮点字面量
    StrLit,    ///< 字符串字面量（text 为转义处理后的值）
    Punct,     ///< 标点/运算符（text 保存操作符本身，如 "+"、"->"）
};

struct Token {
    TokKind kind = TokKind::Eof;
    SourceRange range;
    std::string text;
};

/// 关键字全集（闭合集合，新增必须过 RFC 流程——设计文档十二章第 10 条）。
/// try/except：文档第四章——仅用于不可恢复条件（panic 语义）。
/// async/await：文档第八章——并发原语（Python 原生形态；Task 语义 P2 落地）。
inline bool isKeyword(std::string_view s) {
    static const char* kKeywords[] = {
        "def", "return", "if", "elif", "else", "while", "for", "in",
        "break", "continue", "pass", "import", "from",
        "and", "or", "not", "True", "False", "None", "assert",
        "try", "except", "async", "await",
    };
    for (const char* k : kKeywords)
        if (s == k) return true;
    return false;
}

} // namespace scourge
