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

} // namespace scourge
