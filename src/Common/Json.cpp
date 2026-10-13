// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 最小 JSON 解析器实现（递归下降）
// ============================================================================
#include "scourge/Common/Json.hpp"

#include <charconv>
#include <cstdlib>

namespace scourge::json {

namespace {

/// 嵌套深度上限：防病态深嵌套输入拖垮栈（JSON-RPC 报文远用不到这么深）。
constexpr int kMaxDepth = 64;

class Parser {
public:
    Parser(std::string_view text, std::string& err) : text_(text), err_(err) {}

    bool run(Value& out) {
        skipWs();
        if (!parseValue(out, 0)) return false;
        skipWs();
        if (pos_ != text_.size()) return fail("JSON 文本结束后存在尾随内容");
        return true;
    }

private:
    std::string_view text_;
    std::string& err_;
    size_t pos_ = 0;

    bool fail(std::string_view msg) {
        err_ = "偏移 " + std::to_string(pos_) + ": " + std::string(msg);
        return false;
    }

    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return text_[pos_]; }

    void skipWs() {
        while (!eof()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    bool expect(char c) {
        if (eof() || peek() != c) {
            std::string msg = "期望 '";
            msg += c;
            msg += "'";
            return fail(msg);
        }
        ++pos_;
        return true;
    }

    bool parseValue(Value& v, int depth) {
        if (depth > kMaxDepth) return fail("JSON 嵌套过深");
        if (eof()) return fail("JSON 文本意外结束");
        switch (peek()) {
        case '{': return parseObject(v, depth);
        case '[': return parseArray(v, depth);
        case '"': return parseString(v);
        case 't':
        case 'f': return parseBool(v);
        case 'n': return parseNull(v);
        default: return parseNumber(v);
        }
    }

    bool literal(std::string_view lit) {
        if (text_.substr(pos_, lit.size()) != lit) return fail("无效字面量");
        pos_ += lit.size();
        return true;
    }

    bool parseNull(Value& v) {
        if (!literal("null")) return false;
        v = Value{};
        return true;
    }

    bool parseBool(Value& v) {
        bool val = peek() == 't';
        if (!literal(val ? "true" : "false")) return false;
        v = Value{};
        v.kind = Value::Kind::Bool;
        v.boolean = val;
        return true;
    }

    bool parseNumber(Value& v) {
        size_t begin = pos_;
        if (!eof() && peek() == '-') ++pos_;
        while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        bool isDouble = false;
        if (!eof() && peek() == '.') {
            isDouble = true;
            ++pos_;
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            isDouble = true;
            ++pos_;
            if (!eof() && (peek() == '+' || peek() == '-')) ++pos_;
            while (!eof() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        std::string_view span = text_.substr(begin, pos_ - begin);
        if (span.empty() || span == "-") return fail("无效数字");
        v = Value{};
        if (isDouble) {
            v.kind = Value::Kind::Double;
            v.number = std::strtod(std::string(span).c_str(), nullptr);
            return true;
        }
        v.kind = Value::Kind::Int;
        auto [p, ec] = std::from_chars(span.data(), span.data() + span.size(),
                                       v.integer);
        if (ec != std::errc{}) return fail("整数超出 int64 范围");
        return true;
    }

    bool parseString(Value& v) {
        v = Value{};
        v.kind = Value::Kind::String;
        if (!expect('"')) return false;
        std::string& out = v.str;
        for (;;) {
            if (eof()) return fail("字符串未终止");
            char c = peek();
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c == '\\') {
                ++pos_;
                if (eof()) return fail("转义序列未终止");
                char e = peek();
                ++pos_;
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!parseHex4(cp)) return false;
                    // 代理对：高代理后必须跟低代理（组合为增补平面码点）；
                    // 孤立/非法代理 → U+FFFD 替换符（解码尽力而为，不静默丢字符）。
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                            text_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint32_t low;
                            if (!parseHex4(low)) return false;
                            if (low >= 0xDC00 && low <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            else
                                cp = 0xFFFD;
                        } else {
                            cp = 0xFFFD;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        cp = 0xFFFD;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("无效转义字符");
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20)
                return fail("字符串含未转义控制字符");
            out += c;
            ++pos_;
        }
    }

    bool parseHex4(uint32_t& out) {
        if (pos_ + 4 > text_.size()) return fail("\\u 转义需要 4 位十六进制");
        out = 0;
        for (size_t i = 0; i < 4; ++i) {
            char h = text_[pos_ + i];
            out <<= 4;
            if (h >= '0' && h <= '9')
                out |= static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f')
                out |= static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
                out |= static_cast<uint32_t>(h - 'A' + 10);
            else
                return fail("\\u 转义含非十六进制字符");
        }
        pos_ += 4;
        return true;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool parseArray(Value& v, int depth) {
        v = Value{};
        v.kind = Value::Kind::Array;
        if (!expect('[')) return false;
        skipWs();
        if (!eof() && peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            v.items.emplace_back();
            if (!parseValue(v.items.back(), depth + 1)) return false;
            skipWs();
            if (eof()) return fail("数组未终止");
            if (peek() == ',') {
                ++pos_;
                skipWs();
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return true;
            }
            return fail("数组元素后期望 ',' 或 ']'");
        }
    }

    bool parseObject(Value& v, int depth) {
        v = Value{};
        v.kind = Value::Kind::Object;
        if (!expect('{')) return false;
        skipWs();
        if (!eof() && peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skipWs();
            if (eof() || peek() != '"') return fail("对象键必须是字符串");
            Value key;
            if (!parseString(key)) return false;
            skipWs();
            if (!expect(':')) return false;
            skipWs();
            if (!parseValue(v.members[key.str], depth + 1)) return false;
            skipWs();
            if (eof()) return fail("对象未终止");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return true;
            }
            return fail("对象成员后期望 ',' 或 '}'");
        }
    }
};

} // namespace

const Value* Value::find(std::string_view key) const {
    if (kind != Kind::Object) return nullptr;
    auto it = members.find(std::string(key));
    return it == members.end() ? nullptr : &it->second;
}

bool parse(std::string_view text, Value& out, std::string& err) {
    err.clear();
    out = Value{};
    Parser p(text, err);
    return p.run(out);
}

} // namespace scourge::json
