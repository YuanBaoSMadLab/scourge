// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 语义类型系统（设计文档第四章）
// ----------------------------------------------------------------------------
//  * 结构化类型 + 局部推断：推断不出即报错（E2002），无动态兜底。
//  * Tensor：元素类型 + 静态/动态（kDyn）形状；形状写入类型本体。
//  * Result[T, E]：错误即值；消费由检查器强制（E2005）。
//  * 类型不可变（shared_ptr<const Type>），结构相等比较即可判同。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scourge {

struct Type;
using TypePtr = std::shared_ptr<const Type>;

/// Tensor 动态维标记（对应源码中的 "?"）。
inline constexpr int64_t kDyn = -1;

struct Type {
    enum class Kind : uint8_t {
        Int,     ///< 64 位有符号整数（溢出 panic，无 UB）
        Float,   ///< IEEE 754 双精度
        Bool,
        Str,
        Bytes,   ///< P0 仅类型面；操作随标准库 v1 落地
        None,
        List,    ///< params[0] = 元素
        Dict,    ///< params[0] = 键, params[1] = 值
        Set,     ///< params[0] = 元素（0.9.3；元素限标量，语义同 Dict 键）
        Tuple,   ///< params = 各元素类型（0.9.6；异构定长不可变，元素限标量/嵌套）
        Json,    ///< 动态 JSON 值（0.9.4 M 专项）：null/bool/int/float/str/
                 ///< array/object 七形态，json_decode 产生，成员方法取值
        Tensor,  ///< params[0] = 元素；shape 为形状（kDyn = 动态维）
        Result,  ///< params[0] = OK 类型, params[1] = Err 类型
        Option,  ///< params[0] = 内层
        Range,   ///< range(n) 的返回；仅用于 for-in
        Function,///< params = [参数类型…] + params.back() 为返回类型
        Builtin, ///< 内建函数（print/str/len/range）；name = 内建名
        Module,  ///< name = 模块名
        Err,     ///< 已报错占位：抑制级联错误
        Unknown, ///< 推断失败占位：赋值上下文报 E2002
    };

    Kind kind;
    std::vector<TypePtr> params;
    std::vector<int64_t> shape; ///< 仅 Tensor
    std::string name;           ///< 仅 Module / Err 附加信息

    explicit Type(Kind k) : kind(k) {}
    Type(Kind k, std::vector<TypePtr> ps) : kind(k), params(std::move(ps)) {}
};

// ---- 工厂 -------------------------------------------------------------------
inline TypePtr makeType(Type::Kind k) { return std::make_shared<Type>(k); }
inline TypePtr makeType(Type::Kind k, std::vector<TypePtr> ps) {
    return std::make_shared<Type>(k, std::move(ps));
}
TypePtr makeTensor(TypePtr elem, std::vector<int64_t> shape);
TypePtr makeFunction(std::vector<TypePtr> paramTypes, TypePtr ret);

inline TypePtr tyInt() { return makeType(Type::Kind::Int); }
inline TypePtr tyFloat() { return makeType(Type::Kind::Float); }
inline TypePtr tyBool() { return makeType(Type::Kind::Bool); }
inline TypePtr tyStr() { return makeType(Type::Kind::Str); }
inline TypePtr tyNone() { return makeType(Type::Kind::None); }
inline TypePtr tyErr() { return makeType(Type::Kind::Err); }
inline TypePtr tyUnknown() { return makeType(Type::Kind::Unknown); }

// ---- 判定 -------------------------------------------------------------------
/// 结构相等（Tensor 形状需逐维相等，动态维与静态维视为不同）。
bool typeEquals(const TypePtr& a, const TypePtr& b);

/// 赋值/传参/返回的隐式可转换规则：
///   * 结构相等；Tensor 目标侧动态维可接受任意具体维；
///   * int → float（无精度损失方向的唯一放宽）。
/// bool 不隐式转 int（显式性优先）。
bool isConvertible(const TypePtr& from, const TypePtr& to);

/// 数值统一：int+float → float；相等返回原类型；不可统一返回 nullptr。
TypePtr unifyNumeric(const TypePtr& a, const TypePtr& b);

/// Tensor 形状统一（用于 elementwise 运算）：逐维 dyn 吸收或相等；
/// 秩不同 → false。
bool unifyShape(const std::vector<int64_t>& a, const std::vector<int64_t>& b,
                std::vector<int64_t>& out);

/// 人类可读/日志用的类型串："int" / "Tensor[int, (2, 3)]" / "Result[int, str]"。
std::string typeToString(const TypePtr& t);

// ---- 赋值放宽规则（语句级与模块级检查共用，单一事实源） ----------------------
/// Option 目标放宽（0.9.10）：None → 任意 Option（空选项）；ok() 产出
/// Result[T,str] 同构载荷同质 → 放行；裸 T 载荷同质 → 自动包装。仅变量
/// 赋值与 return 消费——传参不放宽，显式 ok() 构造避免隐式歧义。
/// 载荷用 typeEquals 严格匹配（不用 isConvertible）：int→float 放宽会让
/// AOT 按 int 位型包装、按 float 载荷读位——双模位型错位，故禁入。
inline bool optAssignable(const TypePtr& vt, const TypePtr& target) {
    if (!vt || target->kind != Type::Kind::Option ||
        target->params.size() != 1)
        return false;
    if (vt->kind == Type::Kind::None) return true;
    // ok() 产出 Result[T,str]：err 态消息即 None 态（同构），载荷同质放行。
    if (vt->kind == Type::Kind::Result && vt->params.size() == 2)
        return typeEquals(vt->params[0], target->params[0]);
    return typeEquals(vt, target->params[0]);
}

/// 元组注解 ↔ 字面量逐位匹配（0.9.10）：整体不可赋且双方皆元组时定位首
/// 错位（isConvertible 已逐参结构化，逐位全兼容则整体兼容——只在真实
/// 错位时命中）。返回空串 = 非逐位可比场景（走通用报错）。
inline std::string tupleMismatch(const TypePtr& vt, const TypePtr& ann) {
    if (!vt || !ann || vt->kind != Type::Kind::Tuple ||
        ann->kind != Type::Kind::Tuple ||
        vt->params.size() != ann->params.size())
        return "";
    for (size_t i = 0; i < ann->params.size(); ++i) {
        if (isConvertible(vt->params[i], ann->params[i])) continue;
        return "元组第 " + std::to_string(i + 1) + " 位与注解错位：字面量 " +
               typeToString(vt->params[i]) + "，注解要求 " +
               typeToString(ann->params[i]) +
               "；Tuple 注解与字面量须逐位匹配（如 (1, \"a\") 配 "
               "(int, str)）";
    }
    return "";
}

} // namespace scourge
