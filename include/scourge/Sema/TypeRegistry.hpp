// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 类型元数据单一事实源
// ----------------------------------------------------------------------------
//  职责边界：新增/调整数据类型只改 TypeRegistry.cpp 的 kTypeInfos——
//  类型名、参数元数、实例键 mangle 码、类型串格式、结构类型/标量判定、
//  可注解性全部由该表派生。语义层各处（typeToString / resolveTypeExpr /
//  mangleType / isConvertible）一律查表消费，禁止再写平行 switch。
// ============================================================================
#pragma once

#include "scourge/Sema/Type.hpp"

#include <string>
#include <string_view>

namespace scourge {

/// 类型串的拼装形态（决定 typeToString 的分支，而非逐 Kind 手写）。
enum class TypeFormat : uint8_t {
    Keyword,    ///< 无参：类型名即类型串
    Unary,      ///< Name[T]
    Binary,     ///< Name[T, U]
    Tuple,      ///< (T1, T2, ...)
    Tensor,     ///< Tensor[elem, (dims)]
    Function,   ///< Fn[(params) -> ret]
    Module,     ///< "module " + name
    Builtin,    ///< "builtin " + name
    Unresolved, ///< 占位（<错误>/<未知>）
};

/// 参数个数可变（Tuple / Function）。
inline constexpr int8_t kVariadicArity = -1;

struct TypeInfo {
    Type::Kind kind;
    const char* name;  ///< 源码中的注解名（也是类型串主名）
    int8_t arity;      ///< 固定参数个数；kVariadicArity = 可变
    char mangle;       ///< 实例键前缀；'?' = 不可作实例键
    TypeFormat format;
    bool structural;   ///< 结构类型：同种别 + 参数逐一可转换
    bool scalar;       ///< 标量域（Dict 键值 / Set 元素）
    bool annotatable;  ///< 可出现在类型注解中
    const char* arityHint; ///< 元数诊断的名词片段
};

/// kind 越界时返回 Unknown 行（诊断占位，不崩）。
const TypeInfo& typeInfo(Type::Kind k);

/// 注解名反查；非类型名返回 nullptr。
const TypeInfo* typeInfoByName(std::string_view name);

bool isStructuralType(Type::Kind k);

/// 标量判定（Int/Float/Bool/Str）；nullptr 视为非标量。
bool isScalarType(const TypePtr& t);

/// 隐式单态化实例键：前缀 + 子类型码递归；Tensor 附形状维。
std::string mangleTypeKey(const TypePtr& t);

} // namespace scourge
