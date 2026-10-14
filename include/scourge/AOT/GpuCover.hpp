// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — CUDA 第 1 层 GPU 覆盖判定（单一事实源，header-only）
// ----------------------------------------------------------------------------
//  判定「该 lang op 是否由 GPU kernel 路径接管」：三个降级 pass（Affine/
//  Linalg/LLVM）在 gpuMode 下共用此谓词——
//    * LangToAffine / LangToLinalg：覆盖 op 动态合法化（跳过内联降级）；
//    * LangToLLVM：覆盖 op 的 @sg_* 调用换为 @sg_gpu_*（运行时内部再做
//      就绪判定，不可用即逐调用回退 CPU 实现）。
//  覆盖集（与 sg_gpu_ptx kernel 一一对应）：
//    * tensor_binop_tt / ts / st：kind ∈ {0=+, 1=-, 2=*}，双侧同 dtype
//      （f64 或 int——int 走溢出仿真 kernel，panic 语义与 CPU 逐字一致）；
//    * tensor_matmul：双 f64；
//    * tensor_sum_f / tensor_sum_i：f64/int 归约；
//    * tensor_transpose：f64。
//  属性纪律（动态形状放宽后）：MLIRGen 恒写 kind/dtype 属性（binop 的
//  kind/lhs_int/rhs_int 或 t_int/s_int、matmul 的 lhs_int/rhs_int），shape
//  属性仅静态时写——覆盖判定不再要求 shape 属性，运行时形状由 sg_gpu_*_try
//  的守卫校验（rank/形状一致/内维不匹配 → 返回 NULL → 回退 CPU 实现，
//  panic 语义逐字一致）。dtype 标志（lhs_int/rhs_int/t_int/s_int）缺省
//  视为 f64（与 affine 侧 intFlag 缺省语义一致）。
// ============================================================================
#pragma once

#include "llvm/ADT/StringRef.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/Operation.h"

namespace scourge {
namespace aot {
namespace gpu {

/// dtype 标志位：属性缺失或 0 视为 f64（返回 true），非零为 int（false）。
/// 注：v23 快照 Operation::getAttrOfType/getName 无 const 重载，取非 const。
inline bool isF64Flag(mlir::Operation* op, llvm::StringRef name) {
    auto a = op->template getAttrOfType<mlir::IntegerAttr>(name);
    return !a || a.getInt() == 0;
}

/// GPU 覆盖判定（第 1 层切片覆盖集；判定只读属性，无副作用）。
inline bool covers(mlir::Operation* op) {
    using mlir::Operation;
    llvm::StringRef name = op->getName().getStringRef();

    if (name == "lang.tensor_matmul") {
        // 双 f64（int matmul 不覆盖）；秩/内维由运行时 try 守卫。
        return isF64Flag(op, "lhs_int") && isF64Flag(op, "rhs_int");
    }

    if (name == "lang.tensor_binop_tt" || name == "lang.tensor_binop_ts" ||
        name == "lang.tensor_binop_st") {
        auto kind = op->template getAttrOfType<mlir::IntegerAttr>("kind");
        if (!kind) return false;
        int64_t k = kind.getInt();
        if (k < 0 || k > 2) return false; // '/'('//')'/'%'/'**' → CPU panic 语义
        // dtype 判定；shape 属性缺失（动态形状）不排除——运行时守卫兜底。
        const bool isTT = name == "lang.tensor_binop_tt";
        const bool lhsInt = isTT ? !isF64Flag(op, "lhs_int")
                                 : !isF64Flag(op, "t_int");
        const bool rhsInt = isTT ? !isF64Flag(op, "rhs_int")
                                 : !isF64Flag(op, "s_int");
        return lhsInt == rhsInt; // 同 dtype（f64/f64 或 int/int）覆盖
    }

    if (name == "lang.tensor_sum_f") return true; // 标量归约，运行时取 n
    if (name == "lang.tensor_sum_i") return true; // int 归约（溢出 panic 同 CPU）

    if (name == "lang.tensor_transpose") return true; // rank-2/f64 运行时守卫

    return false;
}

} // namespace gpu
} // namespace aot
} // namespace scourge
