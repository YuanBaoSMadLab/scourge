// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 类型元数据表实现
// ============================================================================
#include "scourge/Sema/TypeRegistry.hpp"

namespace scourge {

namespace {

// 行序必须与 Type::Kind 枚举序一致（typeInfo 按序号索引）。
constexpr TypeInfo kTypeInfos[] = {
    // kind                    name        arity  mangle format
    {Type::Kind::Int,     "int",     0, 'i', TypeFormat::Keyword,
     false, true, true, ""},
    {Type::Kind::Float,   "float",   0, 'f', TypeFormat::Keyword,
     false, true, true, ""},
    {Type::Kind::Bool,    "bool",    0, 'b', TypeFormat::Keyword,
     false, true, true, ""},
    {Type::Kind::Str,     "str",     0, 's', TypeFormat::Keyword,
     false, true, true, ""},
    {Type::Kind::Bytes,   "bytes",   0, 'y', TypeFormat::Keyword,
     false, false, true, ""},
    {Type::Kind::None,    "None",    0, 'n', TypeFormat::Keyword,
     false, false, true, ""},
    {Type::Kind::List,    "List",    1, 'L', TypeFormat::Unary,
     true, false, true, "元素类型参数"},
    {Type::Kind::Dict,    "Dict",    2, 'D', TypeFormat::Binary,
     true, false, true, "类型参数（键, 值）"},
    {Type::Kind::Set,     "Set",     1, 'S', TypeFormat::Unary,
     true, false, true, "元素类型参数"},
    {Type::Kind::Tuple,   "Tuple",   kVariadicArity, 'T', TypeFormat::Tuple,
     true, false, true, ""},
    {Type::Kind::Json,    "Json",    0, 'j', TypeFormat::Keyword,
     false, false, true, ""},
    {Type::Kind::Tensor,  "Tensor",  1, 'X', TypeFormat::Tensor,
     true, false, false, ""},
    {Type::Kind::Result,  "Result",  2, 'R', TypeFormat::Binary,
     true, false, true, "类型参数（成功, 失败）"},
    {Type::Kind::Option,  "Option",  1, 'O', TypeFormat::Unary,
     true, false, true, "类型参数"},
    {Type::Kind::Range,   "range",   0, 'r', TypeFormat::Keyword,
     false, false, false, ""},
    {Type::Kind::Function, "Fn",     kVariadicArity, 'F', TypeFormat::Function,
     false, false, false, ""},
    {Type::Kind::Builtin, "builtin", 0, '?', TypeFormat::Builtin,
     false, false, false, ""},
    {Type::Kind::Module,  "module",  0, '?', TypeFormat::Module,
     false, false, false, ""},
    {Type::Kind::Err,     "<错误>",  0, '?', TypeFormat::Unresolved,
     false, false, false, ""},
    {Type::Kind::Unknown, "<未知>",  0, '?', TypeFormat::Unresolved,
     false, false, false, ""},
};

constexpr size_t kTypeInfoCount = sizeof(kTypeInfos) / sizeof(kTypeInfos[0]);

} // namespace

const TypeInfo& typeInfo(Type::Kind k) {
    size_t i = static_cast<size_t>(k);
    if (i >= kTypeInfoCount) return kTypeInfos[static_cast<size_t>(
                                 Type::Kind::Unknown)];
    return kTypeInfos[i];
}

const TypeInfo* typeInfoByName(std::string_view name) {
    for (const TypeInfo& info : kTypeInfos)
        if (name == info.name) return &info;
    return nullptr;
}

bool isStructuralType(Type::Kind k) {
    return typeInfo(k).structural;
}

bool isScalarType(const TypePtr& t) {
    return t && typeInfo(t->kind).scalar;
}

std::string mangleTypeKey(const TypePtr& t) {
    if (!t) return "?";
    const TypeInfo& info = typeInfo(t->kind);
    if (t->kind == Type::Kind::Tensor) {
        // 形状进键：不同形状即不同实例（与 typeEquals 的形状严格相等对齐）。
        std::string out = "X";
        out += t->params.empty() ? std::string("?") : mangleTypeKey(t->params[0]);
        for (int64_t d : t->shape) out += "x" + std::to_string(d);
        return out;
    }
    if (info.mangle == '?') return "?";
    std::string out(1, info.mangle);
    for (const TypePtr& p : t->params) out += mangleTypeKey(p);
    return out;
}

} // namespace scourge
