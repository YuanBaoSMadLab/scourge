// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 字节码 VM — JSON（M 专项：json_decode 内建 + Json 成员方法）
// ----------------------------------------------------------------------------
//  职责边界：只实现 json_decode 与 Json 值的 16 个成员方法；doBuiltin 白名单
//  分派进来（VM.cpp 尾段一行路由），VMMethod.cpp 按 self.kind==Json 一行
//  路由方法调用。本文件是 VM 侧唯一实现处。
//  语义契约（与 src/Tool/BuilderRTJson.hpp 的 sg_json_* 逐字一致）：
//    * 数字判定：文本含 '.' / 'e' / 'E' → double；否则 strtoll，溢出
//      (ERANGE±inf) → 解析失败；下溢按 0 接受。strtod 上溢(±inf) 同失败。
//    * \uXXXX 解码为 UTF-8（含代理对配对；孤立代理 = 解析失败）。
//    * 嵌套深度上限 128（超限单独 panic 消息）；其余解析失败统一一句话。
//    * 对象重复键：后者覆盖值、保留首次插入位置（DictData::set 语义）。
//    * dump：紧凑编码；对象按插入序；float %.17g；控制字符 \u00xx（小写
//      hex）；UTF-8 字节透传。
//    * get(k) 缺键返回 null（不 panic）；get(i) 越界 panic；as_* 形态不符
//      panic（消息带形态名）。
// ============================================================================
#include "scourge/BC/VM.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace scourge {
namespace bc {

namespace {

constexpr int kJsonMaxDepth = 128;

Value makeJsonScalar(JsonData::JK jk) {
    Value v;
    v.kind = Value::Kind::Json;
    v.json = std::make_shared<JsonData>();
    v.json->jk = jk;
    return v;
}

const char* jsonKindName(JsonData::JK jk) {
    switch (jk) {
    case JsonData::JK::Null: return "null";
    case JsonData::JK::Bool: return "bool";
    case JsonData::JK::Int: return "int";
    case JsonData::JK::Float: return "float";
    case JsonData::JK::Str: return "str";
    case JsonData::JK::Arr: return "array";
    case JsonData::JK::Obj: return "object";
    }
    return "?";
}

// ---- 解析器（递归下降；与 C 运行时适配层同一规范） ---------------------------

struct JsonParser {
    const char* p;
    const char* end;
    int depth = 0;

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }

    [[noreturn]] void fail(const std::string& msg) {
        // 转成 VM panic（由调用方包一层 panicHere）
        throw JsonError{msg};
    }
    struct JsonError {
        std::string msg;
    };

    bool consume(char c) {
        if (p < end && *p == c) {
            ++p;
            return true;
        }
        return false;
    }

    void expect(char c) {
        if (!consume(c)) fail("json_decode() 输入不是有效 JSON");
    }

    // \uXXXX → UTF-8 码点（代理对配对；返回 -1 = 解析失败）
    int hex4() {
        int v = 0;
        for (int k = 0; k < 4; ++k) {
            if (p >= end) return -1;
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return -1;
        }
        return v;
    }

    // 解析字符串字面量体（含转义）→ UTF-8 字节。失败返回 false。
    bool parseStringBody(std::string& out) {
        // 入口已消费开引号
        while (true) {
            if (p >= end) return false;
            unsigned char c = static_cast<unsigned char>(*p++);
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) return false;
                char e = *p++;
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    int cp = hex4();
                    if (cp < 0) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // 高代理：必须紧跟 \uDC00-\uDFFF
                        if (p + 1 < end && p[0] == '\\' && p[1] == 'u') {
                            p += 2;
                            int lo = hex4();
                            if (lo < 0xDC00 || lo > 0xDFFF) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) +
                                 (lo - 0xDC00);
                        } else {
                            return false; // 孤立高代理
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false; // 孤立低代理
                    }
                    // 码点 → UTF-8
                    if (cp < 0x80) {
                        out.push_back(char(cp));
                    } else if (cp < 0x800) {
                        out.push_back(char(0xC0 | (cp >> 6)));
                        out.push_back(char(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        out.push_back(char(0xE0 | (cp >> 12)));
                        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(char(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(char(0xF0 | (cp >> 18)));
                        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
                        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(char(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return false; // 非法转义
                }
                continue;
            }
            if (c < 0x20) return false; // 裸控制字符
            out.push_back(char(c));
        }
    }

    Value parseValue() {
        skipWs();
        if (p >= end) fail("json_decode() 输入不是有效 JSON");
        char c = *p;
        if (c == '{') return parseObj();
        if (c == '[') return parseArr();
        if (c == '"') {
            ++p;
            std::string s;
            if (!parseStringBody(s)) fail("json_decode() 输入不是有效 JSON");
            Value v = makeJsonScalar(JsonData::JK::Str);
            v.json->s = std::move(s);
            return v;
        }
        if (c == 't') {
            if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
                p += 4;
                Value v = makeJsonScalar(JsonData::JK::Bool);
                v.json->b = true;
                return v;
            }
            fail("json_decode() 输入不是有效 JSON");
        }
        if (c == 'f') {
            if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
                p += 5;
                return makeJsonScalar(JsonData::JK::Bool);
            }
            fail("json_decode() 输入不是有效 JSON");
        }
        if (c == 'n') {
            if (end - p >= 4 && memcmp(p, "null", 4) == 0) {
                p += 4;
                return makeJsonScalar(JsonData::JK::Null);
            }
            fail("json_decode() 输入不是有效 JSON");
        }
        return parseNumber();
    }

    Value parseNumber() {
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        bool isFloat = false;
        while (p < end) {
            char d = *p;
            if (d >= '0' && d <= '9') {
                ++p;
            } else if (d == '.' || d == 'e' || d == 'E') {
                isFloat = true;
                ++p;
                if (p < end && (*p == '-' || *p == '+')) ++p;
            } else {
                break;
            }
        }
        if (p == start) fail("json_decode() 输入不是有效 JSON");
        std::string text(start, p);
        Value v = makeJsonScalar(isFloat ? JsonData::JK::Float
                                         : JsonData::JK::Int);
        if (!isFloat) {
            errno = 0;
            char* ep = nullptr;
            long long iv = strtoll(text.c_str(), &ep, 10);
            if (errno == ERANGE) fail("json_decode() 输入不是有效 JSON");
            v.json->i = static_cast<int64_t>(iv);
            return v;
        }
        errno = 0;
        v.json->f = strtod(text.c_str(), nullptr);
        if (errno == ERANGE && std::isinf(v.json->f))
            fail("json_decode() 输入不是有效 JSON"); // 上溢；下溢 0 接受
        return v;
    }

    Value parseArr() {
        if (++depth > kJsonMaxDepth)
            fail("json_decode() 嵌套深度超上限 128");
        expect('[');
        Value v = makeJsonScalar(JsonData::JK::Arr);
        v.json->arr = std::make_shared<std::vector<Value>>();
        skipWs();
        if (consume(']')) {
            --depth;
            return v;
        }
        while (true) {
            v.json->arr->push_back(parseValue());
            skipWs();
            if (consume(']')) break;
            expect(',');
        }
        --depth;
        return v;
    }

    Value parseObj() {
        if (++depth > kJsonMaxDepth)
            fail("json_decode() 嵌套深度超上限 128");
        expect('{');
        Value v = makeJsonScalar(JsonData::JK::Obj);
        v.json->obj = std::make_shared<DictData>();
        skipWs();
        if (consume('}')) {
            --depth;
            return v;
        }
        while (true) {
            skipWs();
            if (p >= end || *p != '"') fail("json_decode() 输入不是有效 JSON");
            ++p;
            std::string key;
            if (!parseStringBody(key)) fail("json_decode() 输入不是有效 JSON");
            skipWs();
            expect(':');
            Value val = parseValue();
            v.json->obj->set(Value::makeStr(std::move(key)), std::move(val));
            skipWs();
            if (consume('}')) break;
            expect(',');
        }
        --depth;
        return v;
    }
};

} // namespace

// ---- dump（紧凑编码；与 BuilderRTJson.hpp 的 sg_json_dump 逐字一致） --------

namespace {

void jsonDumpStr(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(char(c)); // UTF-8 字节透传
            }
        }
    }
    out.push_back('"');
}

void jsonDumpOf(const JsonData& j, std::string& out) {
    switch (j.jk) {
    case JsonData::JK::Null: out += "null"; break;
    case JsonData::JK::Bool: out += j.b ? "true" : "false"; break;
    case JsonData::JK::Int: out += std::to_string(j.i); break;
    case JsonData::JK::Float: {
        char buf[40];
        snprintf(buf, sizeof(buf), "%.17g", j.f);
        out += buf;
        break;
    }
    case JsonData::JK::Str: jsonDumpStr(j.s, out); break;
    case JsonData::JK::Arr: {
        out.push_back('[');
        if (j.arr) {
            bool first = true;
            for (const Value& e : *j.arr) {
                if (!first) out.push_back(',');
                first = false;
                if (e.kind == Value::Kind::Json && e.json)
                    jsonDumpOf(*e.json, out);
                else
                    out += "null"; // 内部不变量被破坏时的确定性兜底
            }
        }
        out.push_back(']');
        break;
    }
    case JsonData::JK::Obj: {
        out.push_back('{');
        if (j.obj) {
            bool first = true;
            for (const auto& e : j.obj->entries) {
                if (!first) out.push_back(',');
                first = false;
                jsonDumpStr(e.key.s, out);
                out.push_back(':');
                if (e.val.kind == Value::Kind::Json && e.val.json)
                    jsonDumpOf(*e.val.json, out);
                else
                    out += "null";
            }
        }
        out.push_back('}');
        break;
    }
    }
}

} // namespace

std::string jsonDump(const Value& v) {
    // 契约（VM.hpp）：非 Json 入参属内部错误，由调用方（toString 的 Json
    // 分支）保证 kind；此处不再设防御分支（panicHere 是 VM 成员，自由函数
    // 不可用——调用方保证即唯一防线）。
    std::string out;
    jsonDumpOf(*v.json, out);
    return out;
}

// ---- json_decode 内建 --------------------------------------------------------

void VM::jsonBuiltin(const std::string& n, std::vector<Value>& a) {
    if (n == "json_decode") {
        if (a.size() != 1) panicHere("json_decode() 恰好需要 1 个参数");
        if (a[0].kind != Value::Kind::Str)
            panicHere("json_decode() 参数必须是 str");
        JsonParser ps{a[0].s.data(), a[0].s.data() + a[0].s.size(), 0};
        try {
            Value v = ps.parseValue();
            ps.skipWs();
            if (ps.p != ps.end) ps.fail("json_decode() 输入不是有效 JSON");
            push(std::move(v));
        } catch (JsonParser::JsonError& err) {
            panicHere(err.msg);
        }
        return;
    }
    panicHere("VM 内部错误：jsonBuiltin 未处理 '" + n + "'");
}

// ---- Json 成员方法 ------------------------------------------------------------

void VM::jsonMethod(Method m, const Value& self, const std::vector<Value>& args) {
    const JsonData& j = *self.json;
    auto needArgs = [&](size_t k) {
        if (args.size() != k) panicHere("Json 方法参数个数不符");
    };
    switch (m) {
    case Method::JsonIsNull: needArgs(0); push(Value::makeBool(j.jk == JsonData::JK::Null)); return;
    case Method::JsonIsBool: needArgs(0); push(Value::makeBool(j.jk == JsonData::JK::Bool)); return;
    case Method::JsonIsInt: needArgs(0); push(Value::makeBool(j.jk == JsonData::JK::Int)); return;
    case Method::JsonIsFloat: needArgs(0); push(Value::makeBool(j.jk == JsonData::JK::Float)); return;
    case Method::JsonIsStr: needArgs(0); push(Value::makeBool(j.jk == JsonData::JK::Str)); return;
    case Method::JsonIsList: needArgs(0); push(Value::makeBool(j.jk == JsonData::JK::Arr)); return;
    case Method::JsonIsObj: needArgs(0); push(Value::makeBool(j.jk == JsonData::JK::Obj)); return;
    case Method::JsonAsInt: {
        needArgs(0);
        if (j.jk != JsonData::JK::Int)
            panicHere(std::string("as_int() 仅支持整数形态（实际为 ") +
                      jsonKindName(j.jk) + "）");
        push(Value::makeInt(j.i));
        return;
    }
    case Method::JsonAsFloat: {
        needArgs(0);
        if (j.jk == JsonData::JK::Float) push(Value::makeFloat(j.f));
        else if (j.jk == JsonData::JK::Int)
            push(Value::makeFloat(double(j.i)));
        else
            panicHere(std::string("as_float() 仅支持数值形态（实际为 ") +
                      jsonKindName(j.jk) + "）");
        return;
    }
    case Method::JsonAsBool: {
        needArgs(0);
        if (j.jk != JsonData::JK::Bool)
            panicHere(std::string("as_bool() 仅支持布尔形态（实际为 ") +
                      jsonKindName(j.jk) + "）");
        push(Value::makeBool(j.b));
        return;
    }
    case Method::JsonAsStr: {
        needArgs(0);
        if (j.jk != JsonData::JK::Str)
            panicHere(std::string("as_str() 仅支持字符串形态（实际为 ") +
                      jsonKindName(j.jk) + "）");
        push(Value::makeStr(j.s));
        return;
    }
    case Method::JsonAt: {
        needArgs(1);
        if (j.jk != JsonData::JK::Arr)
            panicHere(std::string("at() 仅支持数组形态（实际为 ") +
                      jsonKindName(j.jk) + "）");
        if (args[0].kind != Value::Kind::Int)
            panicHere("at() 数组下标必须是 int");
        int64_t idx = args[0].i;
        if (idx < 0 || !j.arr || idx >= int64_t(j.arr->size()))
            panicHere("at() 数组下标越界");
        push((*j.arr)[size_t(idx)]);
        return;
    }
    case Method::JsonGet: {
        needArgs(1);
        if (j.jk != JsonData::JK::Obj)
            panicHere(std::string("get() 仅支持对象形态（实际为 ") +
                      jsonKindName(j.jk) + "）");
        if (args[0].kind != Value::Kind::Str)
            panicHere("get() 对象键必须是 str");
        // 缺键返回 null（与 JSON null 表达缺失同一出口；文档写明）
        uint32_t e = j.obj->find(args[0]);
        if (e == DictData::kEmpty) {
            push(makeJsonScalar(JsonData::JK::Null));
            return;
        }
        push(j.obj->entries[e].val);
        return;
    }
    case Method::JsonLen: {
        needArgs(0);
        if (j.jk == JsonData::JK::Arr)
            push(Value::makeInt(j.arr ? int64_t(j.arr->size()) : 0));
        else if (j.jk == JsonData::JK::Obj)
            push(Value::makeInt(j.obj ? int64_t(j.obj->entries.size()) : 0));
        else
            panicHere(std::string("len() 仅支持数组或对象形态（实际为 ") +
                      jsonKindName(j.jk) + "）");
        return;
    }
    case Method::JsonKeys: {
        needArgs(0);
        if (j.jk != JsonData::JK::Obj)
            panicHere(std::string("keys() 仅支持对象形态（实际为 ") +
                      jsonKindName(j.jk) + "）");
        auto out = std::make_shared<std::vector<Value>>();
        if (j.obj) {
            out->reserve(j.obj->entries.size());
            for (const auto& e : j.obj->entries)
                out->push_back(Value::makeStr(e.key.s));
        }
        push(Value::makeList(std::move(out)));
        return;
    }
    case Method::JsonDump:
        needArgs(0);
        push(Value::makeStr(jsonDump(self)));
        return;
    default:
        panicHere("VM 内部错误：jsonMethod 未处理该方法");
    }
}

} // namespace bc
} // namespace scourge
