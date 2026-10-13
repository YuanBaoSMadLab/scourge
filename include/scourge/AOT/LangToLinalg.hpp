// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT Lang 方言 → Linalg 降级 pass 接口（实现见
//  AOT/LangToLinalg.cpp）
// ----------------------------------------------------------------------------
//  AOT 步骤 3：把 MLIRGen 产出、且携带静态形状属性的张量计算类 lang op
//  （f64 的 binop/matmul/sum/transpose）降级为 MLIR 原生 linalg 结构化 op
//  （memref 语义），替换原 @sg_* 运行时调用路径——为后续结构化优化
//  （canonicalize/向量化）与 LangToAffine/GPU 铺路。
//
//  流水线位置：parseSourceString → 本 pass → createLangToLLVMPass（剩余
//  lang op 兜底 sg_*）→ linalg-to-loops → scf-to-cf → memref/index-to-llvm
//  → convert-to-llvm → translate。
//
//  语义纪律（与 sg_* 运行时逐字对齐，见 MLIRGen emit 侧的覆盖条件）：
//  * 只有 TypeChecker 静态检查已证（形状兼容/维度匹配）且 f64 无 panic
//    语义（溢出/除零/越界）的 op 才带属性并降级；int 与其余 op 一律
//    留给 LangToLLVM 走 sg_*（除零/溢出 panic 语义保持）。
//  * 覆盖集：binop kind ∈ {0=+, 1=-, 2=*}（'/' 有除零 panic）、matmul、
//    sum_f、transpose（2D）。
// ============================================================================
#pragma once

#include <memory>

namespace mlir {
class Pass;
}

namespace scourge {
namespace aot {

/// 创建 Lang → Linalg 降级 pass（ModulePass，partial conversion：未覆盖的
/// lang op 原样保留，由 LangToLLVM 兜底）。
std::unique_ptr<mlir::Pass> createLangToLinalgPass();

} // namespace aot
} // namespace scourge
