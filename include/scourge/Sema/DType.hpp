// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 张量元素 dtype 单一事实源
// ----------------------------------------------------------------------------
//  新增 dtype 只改 kDTypeInfos 一行：Sema 注解校验、AOT tInt 元数据回填、
//  VM/GPU 的 int 存储分派与逐元素结果 dtype 规则全部查表消费，禁止各端
//  再写 `kind != Float` / `name != "float"` 这类平行判定。
// ============================================================================
#pragma once

#include "scourge/Sema/Type.hpp"

#include <cstdint>
#include <string_view>

namespace scourge {

/// 运行时可见的 dtype。i64/f64 之外的 dtype 需先扩 SgTensor/TensorData
/// 存储（isInt 二态不足以表达）——本表是那条改造的唯一入口。
enum class DType : uint8_t { Int, Float };

struct DTypeInfo {
    DType dtype;
    Type::Kind elemKind; ///< Sema 侧元素类型
    const char* name;    ///< 源码注解名 / 诊断串
    bool isInt;          ///< 运行时存储：true = i64 数组，false = f64 数组
};

inline constexpr DTypeInfo kDTypeInfos[] = {
    {DType::Int, Type::Kind::Int, "int", true},
    {DType::Float, Type::Kind::Float, "float", false},
};

inline const DTypeInfo* dtypeInfo(Type::Kind k) {
    for (const DTypeInfo& i : kDTypeInfos)
        if (i.elemKind == k) return &i;
    return nullptr;
}

inline const DTypeInfo* dtypeInfoOf(const TypePtr& t) {
    return t ? dtypeInfo(t->kind) : nullptr;
}

/// 该类型能否作为张量元素（int/float）。
inline bool isDType(const TypePtr& t) { return dtypeInfoOf(t) != nullptr; }

/// 元素类型的运行时存储形态；非法/未知 dtype 按 int 处理（调用点此前
/// 的 `kind != Float` 兜底语义，Sema 已拒绝非法注解，此路不可达）。
inline bool dtypeIsInt(const TypePtr& t) {
    const DTypeInfo* i = dtypeInfoOf(t);
    return i ? i->isInt : true;
}

/// 注解层的同名查询（AOT 从 TypeExpr 语法树回填时只有名字）。
inline bool dtypeIsIntByName(std::string_view name) {
    for (const DTypeInfo& i : kDTypeInfos)
        if (name == i.name) return i.isInt;
    return true;
}

/// 张量逐元素运算的结果 dtype：真除法恒 float，其余两侧 int 才 int。
/// VM（Bin 枚举）与 AOT（tOp 编码，3 = '/'）共用此规则。
inline bool dtypeElementwiseIsInt(bool isTrueDiv, bool lhsInt, bool rhsInt) {
    return !isTrueDiv && lhsInt && rhsInt;
}

/// 张量一元数学的结果 dtype：abs 等保 dtype 的函数（keepsInt）继承元素
/// dtype，sqrt/exp/log 等恒 float。VM（keepInt）与 AOT（fnid 0 = abs）共用。
inline bool dtypeUnaryIsInt(bool keepsInt, bool elemInt) {
    return keepsInt && elemInt;
}

} // namespace scourge
