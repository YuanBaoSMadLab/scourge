// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — CUDA 第 1 层 GPU 覆盖判定（单一事实源，header-only）
// ----------------------------------------------------------------------------
//  判定「该 lang op 是否由 GPU kernel 路径接管」：三个降级 pass（Affine/
//  Linalg/LLVM）在 gpuMode 下共用此谓词——
//    * LangToAffine / LangToLinalg：覆盖 op 动态合法化（跳过内联降级）；
//    * LangToLLVM：覆盖 op 的 @sg_* 调用换为 @sg_gpu_*（运行时内部再做
//      就绪判定，不可用即逐调用回退 CPU 实现）。
//  覆盖集（f64 全静态形状，与 sg_gpu_ptx 五 kernel 一一对应）：
//    * tensor_binop_tt / ts / st：kind ∈ {0=+, 1=-, 2=*}，双侧 f64；
//    * tensor_matmul：二维 f64；
//    * tensor_sum_f：f64 归约；
//    * tensor_transpose：二维 f64。
//  属性纪律与 LangToAffine/LangToLinalg 一致：形状属性缺失 = 未覆盖
//  （动态形状/int 张量/kind 3-6 仍走 CPU 路径，panic 语义零分叉）。
//  dtype 标志（lhs_int/rhs_int/t_int/s_int）缺省视为 f64（与 affine 侧
//  intFlag 缺省语义一致）。
// ============================================================================
#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
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

/// 形状属性读取：缺失返回 false（未覆盖）；存在则拷入 out。
inline bool shapeOf(mlir::Operation* op, llvm::StringRef name,
                    llvm::SmallVectorImpl<int64_t>& out) {
    auto a = op->template getAttrOfType<mlir::DenseI64ArrayAttr>(name);
    if (!a) return false;
    llvm::append_range(out, a.asArrayRef());
    return true;
}

/// GPU 覆盖判定（CUDA 第 1 层切片覆盖集；判定只读属性，无副作用）。
inline bool covers(mlir::Operation* op) {
    using mlir::Operation;
    llvm::StringRef name = op->getName().getStringRef();

    if (name == "lang.tensor_matmul") {
        llvm::SmallVector<int64_t> l, r;
        if (!shapeOf(op, "lhs_shape", l) || !shapeOf(op, "rhs_shape", r))
            return false;
        return l.size() == 2 && r.size() == 2 && isF64Flag(op, "lhs_int") &&
               isF64Flag(op, "rhs_int");
    }

    if (name == "lang.tensor_binop_tt" || name == "lang.tensor_binop_ts" ||
        name == "lang.tensor_binop_st") {
        auto kind = op->template getAttrOfType<mlir::IntegerAttr>("kind");
        llvm::SmallVector<int64_t> s;
        if (!kind || !shapeOf(op, "shape", s)) return false;
        int64_t k = kind.getInt();
        if (k < 0 || k > 2) return false; // '/'('//')'/'%'/'**' → CPU panic 语义
        const bool isTT = name == "lang.tensor_binop_tt";
        const bool lhsInt = isTT ? !isF64Flag(op, "lhs_int")
                                 : !isF64Flag(op, "t_int");
        const bool rhsInt = isTT ? !isF64Flag(op, "rhs_int")
                                 : !isF64Flag(op, "s_int");
        return !lhsInt && !rhsInt;
    }

    if (name == "lang.tensor_sum_f") {
        llvm::SmallVector<int64_t> s;
        return shapeOf(op, "shape", s);
    }

    if (name == "lang.tensor_transpose") {
        llvm::SmallVector<int64_t> s;
        return shapeOf(op, "shape", s) && s.size() == 2 &&
               isF64Flag(op, "t_int");
    }

    return false;
}

} // namespace gpu
} // namespace aot
} // namespace scourge
