// ============================================================================
//  Scourge 编译器 — 字节码编译器 · 内部共享辅助（Compiler*.cpp 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：编译中途放弃异常（CompileAbort）、内建名注册表
//  （builtinNames / isBuiltinName——与 VM doBuiltin 白名单同一清单语义）、
//  张量字面量判定与折叠（isTensorLiteral / tensorLitHasFloat /
//  TensorLitAcc / fillTensorLit）。原 Compiler.cpp 匿名命名空间实现原地
//  转 inline，语义逐字节不变。仅供 src/BC/Compiler*.cpp 引用。
// ============================================================================
#pragma once

#include "scourge/BC/Compiler.hpp"
#include "scourge/Common/Builtins.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scourge {
namespace bc {

/// 编译中途放弃（诊断已登记）——用异常退栈，compileModule 捕获后报 false。
struct CompileAbort {};

/// 内建名单一事实源（Common/Builtins.hpp）；isBuiltinName 由其提供。
inline constexpr auto& builtinNames = kBuiltinNames;

/// 与检查器 checkListLit 同规则：非空且元素全为数值字面量（含一元负号、
/// 嵌套数值列表）的字面量在类型系统中即张量。编译器按同一判定将其编为
/// Tensor 常量——与检查器语义零分叉，禁止静默降级为 List。
inline bool isTensorLiteral(const ListLit& ll) {
    if (ll.elems.empty()) return false;
    for (const auto& el : ll.elems) {
        switch (el->kind) {
        case Expr::Kind::IntLit:
        case Expr::Kind::FloatLit:
            continue;
        case Expr::Kind::Unary: {
            auto& u = static_cast<const scourge::Unary&>(*el);
            if (u.op != "-") return false;
            Expr::Kind k = u.operand->kind;
            if (k != Expr::Kind::IntLit && k != Expr::Kind::FloatLit)
                return false;
            continue;
        }
        case Expr::Kind::ListLit:
            if (!isTensorLiteral(static_cast<const ListLit&>(*el)))
                return false;
            continue;
        default:
            return false;
        }
    }
    return true;
}

/// 张量字面量是否含浮点字面量（含嵌套）——决定 dtype（f64 / i64）。
inline bool tensorLitHasFloat(const ListLit& ll) {
    for (const auto& el : ll.elems) {
        switch (el->kind) {
        case Expr::Kind::FloatLit:
            return true;
        case Expr::Kind::IntLit:
            break;
        case Expr::Kind::Unary: {
            auto& u = static_cast<const scourge::Unary&>(*el);
            if (u.operand->kind == Expr::Kind::FloatLit) return true;
            break;
        }
        case Expr::Kind::ListLit:
            if (tensorLitHasFloat(static_cast<const ListLit&>(*el)))
                return true;
            break;
        default:
            break;
        }
    }
    return false;
}

/// 张量字面量展平累积器：形状逐维写入，元素按 dtype 收集。
struct TensorLitAcc {
    bool isInt = true;
    std::vector<int64_t> shape;
    std::vector<double> f64;
    std::vector<int64_t> i64;
};

/// 递归填充（行主序）；返回 false = 各行形状不一致。
/// 各行一致性已由检查器校验（check 先于编译），此处防御性兜底。
inline bool fillTensorLit(const ListLit& ll, size_t depth, TensorLitAcc& acc) {
    if (depth >= acc.shape.size()) {
        acc.shape.push_back(static_cast<int64_t>(ll.elems.size()));
    } else if (acc.shape[depth] != static_cast<int64_t>(ll.elems.size())) {
        return false;
    }
    for (const auto& el : ll.elems) {
        switch (el->kind) {
        case Expr::Kind::IntLit: {
            const int64_t v = static_cast<const IntLit&>(*el).value;
            if (acc.isInt) acc.i64.push_back(v);
            else acc.f64.push_back(static_cast<double>(v));
            break;
        }
        case Expr::Kind::FloatLit: { // 预扫描已保证 isInt == false
            acc.f64.push_back(static_cast<const FloatLit&>(*el).value);
            break;
        }
        case Expr::Kind::Unary: { // isTensorLiteral 已保证 "-" + 数值字面量
            auto& u = static_cast<const scourge::Unary&>(*el);
            if (u.operand->kind == Expr::Kind::IntLit) {
                const int64_t v =
                    -static_cast<const IntLit&>(*u.operand).value;
                if (acc.isInt) acc.i64.push_back(v);
                else acc.f64.push_back(static_cast<double>(v));
            } else {
                acc.f64.push_back(
                    -static_cast<const FloatLit&>(*u.operand).value);
            }
            break;
        }
        case Expr::Kind::ListLit:
            if (!fillTensorLit(static_cast<const ListLit&>(*el), depth + 1,
                               acc))
                return false;
            break;
        default:
            return false; // isTensorLiteral 已保证不可达
        }
    }
    return true;
}

} // namespace bc
} // namespace scourge
