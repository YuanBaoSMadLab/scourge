// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — CUDA 第 1 层 GPU 覆盖判定（单一事实源，header-only）
// ----------------------------------------------------------------------------
//  判定「该 lang op 是否由 GPU kernel 路径接管」：三个降级 pass（Affine/
//  Linalg/LLVM）在 gpuMode 下共用此谓词——
//    * LangToAffine / LangToLinalg：覆盖 op 动态合法化（跳过内联降级）；
//    * LangToLLVM：覆盖 op 的 @sg_* 调用换为 @sg_gpu_*（运行时内部再做
//      就绪判定，不可用即逐调用回退 CPU 实现）。
//  声明补插（LangToLLVM 模块级 @sg_gpu_* extern 声明）同样以 coveredLangName
//  消费本文件的 kGpuCoveredLang 名单——覆盖集、声明集单一事实源，禁止在别处
//  平行维护名单。
//  覆盖集（与 sg_gpu_ptx kernel 一一对应）：
//    * tensor_binop_tt / ts / st：kind ∈ {0=+, 1=-, 2=*}，双侧同 dtype
//      （f64 或 int——int 走溢出仿真 kernel，panic 语义与 CPU 逐字一致）；
//    * tensor_matmul：双 f64；
//    * tensor_sum_f / tensor_sum_i：f64/int 归约；
//    * tensor_transpose：f64；
//    * tensor_reduce_i / tensor_reduce_f：max/min（isMax 为常量操作数，
//      单线程同序 kernel，NaN/±0 与 CPU 逐位一致）；
//    * tensor_un：fnid ∈ {0=abs, 1=sqrt}（常量操作数；exp/log 无硬件精确
//      实现，不覆盖）；
//    * tensor_slice / tensor_get_i / tensor_get_f / tensor_set_i /
//      tensor_set_f：无专用计算 kernel——slice 走 sg_cpy 设备内拷贝，
//      get/set 仅对设备驻留张量做 8 字节直读直写（host 驻留 try 返回
//      NULL 回退 CPU，避免无谓上传）。
//    * tensor_concat 不覆盖：多行上传收益低，维持 CPU。
//  属性纪律（动态形状放宽后）：MLIRGen 恒写 kind/dtype 属性（binop 的
//  kind/lhs_int/rhs_int 或 t_int/s_int、matmul 的 lhs_int/rhs_int），shape
//  属性仅静态时写——覆盖判定不再要求 shape 属性，运行时形状由 sg_gpu_*_try
//  的守卫校验（rank/形状一致/内维不匹配 → 返回 NULL → 回退 CPU 实现，
//  panic 语义逐字一致）。dtype 标志（lhs_int/rhs_int/t_int/s_int）缺省
//  视为 f64（与 affine 侧 intFlag 缺省语义一致）。
//  reduce/un 的 isMax/fnid 是编译期常量操作数（MLIRGen i32Const 产
//  arith.constant），covers 窥探常量值判定；非常量保守不覆盖（CPU 路径）。
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

/// 窥探第 idx 个操作数是否为 arith.constant i32/i64 常量，写入 *out。
/// MLIRGen 的 i32Const/整常量均产生 arith.constant；非常量（含块参数）返回
/// false——保守不覆盖，交 CPU 路径。
inline bool peekConstI32(mlir::Operation* op, unsigned idx, int64_t* out) {
    if (idx >= op->getNumOperands()) return false;
    mlir::Operation* def = op->getOperand(idx).getDefiningOp();
    if (!def || def->getName().getStringRef() != "arith.constant") return false;
    auto a = def->getAttrOfType<mlir::IntegerAttr>("value");
    if (!a) return false;
    *out = a.getInt();
    return true;
}

/// GPU 覆盖集 lang op 名单：covers 判定与 LangToLLVM 声明补插共用
/// （声明只需名单；covers 另需逐 op 属性/操作数逻辑）。
inline constexpr const char* kGpuCoveredLang[] = {
    "lang.tensor_binop_tt", "lang.tensor_binop_ts", "lang.tensor_binop_st",
    "lang.tensor_matmul",   "lang.tensor_sum_f",    "lang.tensor_sum_i",
    "lang.tensor_transpose", "lang.tensor_reduce_i", "lang.tensor_reduce_f",
    "lang.tensor_un",       "lang.tensor_slice",    "lang.tensor_get_i",
    "lang.tensor_get_f",    "lang.tensor_set_i",    "lang.tensor_set_f",
};

/// 该 lang op 名是否在 GPU 覆盖集（LangToLLVM 声明补插消费）。
inline bool coveredLangName(llvm::StringRef name) {
    for (const char* k : kGpuCoveredLang)
        if (name == k) return true;
    return false;
}

/// GPU 覆盖判定（第 1 层切片覆盖集；判定只读属性/操作数，无副作用）。
inline bool covers(mlir::Operation* op) {
    using mlir::Operation;
    llvm::StringRef name = op->getName().getStringRef();
    if (!coveredLangName(name)) return false;

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

    if (name == "lang.tensor_reduce_i" || name == "lang.tensor_reduce_f") {
        // isMax 为操作数 0（i32 常量）：窥探常量值，仅 0/1 覆盖。
        int64_t v = 0;
        return peekConstI32(op, 0, &v) && (v == 0 || v == 1);
    }

    if (name == "lang.tensor_un") {
        // fnid 为操作数 0（i32 常量）：仅 0=abs、1=sqrt（exp/log 非精确舍入）。
        int64_t v = 0;
        return peekConstI32(op, 0, &v) && (v == 0 || v == 1);
    }

    // slice/get/set：无计算 kernel，运行时 try 按设备驻留与索引合法性分流。
    if (name == "lang.tensor_slice" || name == "lang.tensor_get_i" ||
        name == "lang.tensor_get_f" || name == "lang.tensor_set_i" ||
        name == "lang.tensor_set_f")
        return true;

    return false;
}

} // namespace gpu
} // namespace aot
} // namespace scourge
