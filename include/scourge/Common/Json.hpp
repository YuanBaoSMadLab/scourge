// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 最小 JSON 解析器（Common/JsonWriter 的解析端对称件）
// ----------------------------------------------------------------------------
//  用途：scourge mcp 的 JSON-RPC 请求解析（外部 AI 代理侧输入）。
//  设计约束：
//    * 零依赖、递归下降、嵌套深度有上限（防病态输入拖垮栈）；
//    * 对象成员存入 std::map（键字典序）——解析端不保序，
//      JSON-RPC / MCP 语义均不依赖成员顺序；
//    * 数字区分整数（int64）与浮点（double）；
//    * 解析失败即报错并说明首个错误位置，无静默兜底（显式拒绝纪律）。
// ============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace scourge::json {

struct Value {
    enum class Kind { Null, Bool, Int, Double, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    int64_t integer = 0;
    double number = 0.0;
    std::string str;
    std::vector<Value> items;             ///< Array 元素（按出现序）
    std::map<std::string, Value> members; ///< Object 成员（键字典序）

    bool isNull() const { return kind == Kind::Null; }
    bool isBool() const { return kind == Kind::Bool; }
    bool isInt() const { return kind == Kind::Int; }
    bool isDouble() const { return kind == Kind::Double; }
    bool isNumber() const { return isInt() || isDouble(); }
    bool isString() const { return kind == Kind::String; }
    bool isArray() const { return kind == Kind::Array; }
    bool isObject() const { return kind == Kind::Object; }

    /// 对象成员查找：非对象或键不存在返回 nullptr。
    const Value* find(std::string_view key) const;
};

/// 解析一段 JSON 文本。成功返回 true 并填充 out；
/// 失败返回 false，err 说明首个错误位置（字节偏移）与原因。
bool parse(std::string_view text, Value& out, std::string& err);

} // namespace scourge::json
