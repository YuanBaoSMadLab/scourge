// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — JsonWriter 实现
// ============================================================================
#include "scourge/Common/JsonWriter.hpp"

#include <cstdio>

namespace scourge {

void JsonWriter::writeIndent() {
    if (!pretty_) return;
    out_.push_back('\n');
    out_.append(static_cast<size_t>(depth_) * 2, ' ');
}

void JsonWriter::writeSep() {
    if (counts_.empty()) return;
    // 成员分隔逗号只出现在「下一个成员开始之前」；键已写、值未写时
    //（pendingKey_）绝不补逗号——否则产出 "k":,v 形式的非法 JSON。
    if (!pendingKey_ && counts_.back() > 0) out_.push_back(',');
    pendingKey_ = false;
    ++counts_.back();
}

void JsonWriter::beginObject() {
    writeSep();
    out_.push_back('{');
    ++depth_;
    counts_.push_back(0);
}

void JsonWriter::endObject() {
    counts_.pop_back();
    --depth_;
    writeIndent();
    out_.push_back('}');
}

void JsonWriter::beginArray() {
    writeSep();
    out_.push_back('[');
    ++depth_;
    counts_.push_back(0);
}

void JsonWriter::endArray() {
    counts_.pop_back();
    --depth_;
    writeIndent();
    out_.push_back(']');
}

void JsonWriter::key(std::string_view k) {
    writeSep();
    writeEscaped(k);
    out_.push_back(':');
    if (pretty_) out_.push_back(' ');
    // 键不算成员结束：成员计数在值写入时补上；值写入前不补逗号。
    if (!counts_.empty()) --counts_.back();
    pendingKey_ = true;
}

void JsonWriter::writeEscaped(std::string_view s) {
    out_.push_back('"');
    for (char raw : s) {
        unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"': out_ += "\\\""; break;
        case '\\': out_ += "\\\\"; break;
        case '\n': out_ += "\\n"; break;
        case '\r': out_ += "\\r"; break;
        case '\t': out_ += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out_ += buf;
            } else {
                out_.push_back(static_cast<char>(c)); // UTF-8 原样输出
            }
        }
    }
    out_.push_back('"');
}

void JsonWriter::valueString(std::string_view s) {
    writeSep();
    writeEscaped(s);
}

void JsonWriter::valueInt(int64_t v) {
    writeSep();
    out_ += std::to_string(v);
}

void JsonWriter::valueBool(bool b) {
    writeSep();
    out_ += b ? "true" : "false";
}

void JsonWriter::valueNull() {
    writeSep();
    out_ += "null";
}

void JsonWriter::pairString(std::string_view k, std::string_view s) {
    key(k);
    valueString(s);
}

void JsonWriter::pairInt(std::string_view k, int64_t v) {
    key(k);
    valueInt(v);
}

void JsonWriter::pairBool(std::string_view k, bool b) {
    key(k);
    valueBool(b);
}

} // namespace scourge
