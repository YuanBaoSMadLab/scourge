// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 语义类型系统实现
// ============================================================================
#include "scourge/Sema/Type.hpp"
#include "scourge/Sema/TypeRegistry.hpp"

namespace scourge {

TypePtr makeTensor(TypePtr elem, std::vector<int64_t> shape) {
    auto t = std::make_shared<Type>(Type::Kind::Tensor,
                                    std::vector<TypePtr>{std::move(elem)});
    t->shape = std::move(shape);
    return t;
}

TypePtr makeFunction(std::vector<TypePtr> paramTypes, TypePtr ret) {
    paramTypes.push_back(std::move(ret)); // 约定：最后一个元素为返回类型
    return std::make_shared<Type>(Type::Kind::Function, std::move(paramTypes));
}

bool typeEquals(const TypePtr& a, const TypePtr& b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    if (a->kind == Type::Kind::Tensor) {
        if (a->shape.size() != b->shape.size()) return false;
        for (size_t i = 0; i < a->shape.size(); ++i)
            if (a->shape[i] != b->shape[i]) return false;
    }
    if (a->params.size() != b->params.size()) return false;
    for (size_t i = 0; i < a->params.size(); ++i)
        if (!typeEquals(a->params[i], b->params[i])) return false;
    return true;
}

bool isConvertible(const TypePtr& from, const TypePtr& to) {
    if (!from || !to) return false;
    if (from->kind == Type::Kind::Err || to->kind == Type::Kind::Err)
        return true; // 错误占位不再制造级联
    if (from->kind == Type::Kind::Unknown || to->kind == Type::Kind::Unknown)
        return true; // 通配：err() 的 OK 类型 / 空列表等待上下文特化
    if (typeEquals(from, to)) return true;

    if (from->kind == Type::Kind::Int && to->kind == Type::Kind::Float)
        return true;

    // 结构类型：同种别 + 参数逐一可转换（Tensor 另有形状规则）。
    if (isStructuralType(from->kind) && from->kind == to->kind) {
        if (from->params.size() != to->params.size()) return false;
        if (from->kind == Type::Kind::Tensor) {
            if (from->shape.size() != to->shape.size()) return false;
            for (size_t i = 0; i < from->shape.size(); ++i) {
                if (to->shape[i] != kDyn && to->shape[i] != from->shape[i])
                    return false; // 动态维只允许出现在目标侧
            }
        }
        for (size_t i = 0; i < from->params.size(); ++i)
            if (!isConvertible(from->params[i], to->params[i])) return false;
        return true;
    }
    return false;
}

TypePtr unifyNumeric(const TypePtr& a, const TypePtr& b) {
    if (!a || !b) return nullptr;
    auto isNum = [](const TypePtr& t) {
        return t->kind == Type::Kind::Int || t->kind == Type::Kind::Float;
    };
    if (!isNum(a) || !isNum(b)) return nullptr;
    if (a->kind == Type::Kind::Float || b->kind == Type::Kind::Float)
        return tyFloat();
    return tyInt();
}

bool unifyShape(const std::vector<int64_t>& a, const std::vector<int64_t>& b,
                std::vector<int64_t>& out) {
    if (a.size() != b.size()) return false;
    out.resize(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == kDyn) out[i] = b[i];
        else if (b[i] == kDyn) out[i] = a[i];
        else if (a[i] == b[i]) out[i] = a[i];
        else return false;
    }
    return true;
}

std::string typeToString(const TypePtr& t) {
    if (!t) return "<null>";
    const TypeInfo& info = typeInfo(t->kind);
    switch (info.format) {
    case TypeFormat::Keyword:
    case TypeFormat::Unresolved:
        return info.name;
    case TypeFormat::Module:
        return std::string("module ") + t->name;
    case TypeFormat::Builtin:
        return std::string("builtin ") + t->name;
    case TypeFormat::Unary:
        return std::string(info.name) + "[" + typeToString(t->params[0]) + "]";
    case TypeFormat::Binary:
        return std::string(info.name) + "[" + typeToString(t->params[0]) +
               ", " + typeToString(t->params[1]) + "]";
    case TypeFormat::Tuple: {
        // 元组类型串 = 源码字面量形态（空 ()；单元素无尾逗号——类型串仅
        // 诊断用，与值 repr 的 "(1,)" 形态无对齐要求）。
        std::string s = "(";
        for (size_t i = 0; i < t->params.size(); ++i) {
            if (i) s += ", ";
            s += typeToString(t->params[i]);
        }
        return s + ")";
    }
    case TypeFormat::Tensor: {
        std::string s = "Tensor[" + typeToString(t->params[0]) + ", (";
        for (size_t i = 0; i < t->shape.size(); ++i) {
            if (i) s += ", ";
            s += t->shape[i] == kDyn ? std::string("?")
                                     : std::to_string(t->shape[i]);
        }
        s += ")]";
        return s;
    }
    case TypeFormat::Function: {
        // params = [参数…] + 末位返回
        std::string s = "Fn[(";
        for (size_t i = 0; i + 1 < t->params.size(); ++i) {
            if (i) s += ", ";
            s += typeToString(t->params[i]);
        }
        s += ") -> " +
             typeToString(t->params.empty() ? tyNone() : t->params.back()) +
             "]";
        return s;
    }
    }
    return "<未知>";
}

} // namespace scourge
