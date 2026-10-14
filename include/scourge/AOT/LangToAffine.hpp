// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT Lang 方言 → Affine 降级 pass 接口（实现见
//  AOT/LangToAffine.cpp）
// ----------------------------------------------------------------------------
//  AOT 步骤 3b：把 MLIRGen 产出、且携带静态形状属性的张量逐元素/归约/
//  转置类 lang op（f64 全静态形状的 binop tt/ts/st kind 0-2、sum_f、
//  transpose）降级为 MLIR affine 结构化循环（affine.for + affine.load/
//  affine.store，常量上下界）——polyhedral 优化（依赖分析/循环融合/分块）
//  的落点层。matmul 仍由 LangToLinalg 消费（linalg.matmul 是正确的
//  结构化抽象）。
//
//  流水线位置：parseSourceString → 本 pass → createLangToLinalgPass
//  （matmul 等）→ createLangToLLVMPass（剩余 lang op 兜底 sg_*）→
//  linalg-to-loops → lower-affine → scf-to-cf → convert-to-llvm →
//  translate。affine op 由 createLowerAffinePass 统一降为 scf/memref。
//
//  语义纪律（与 sg_* 运行时逐字对齐，见 MLIRGen emit 侧的覆盖条件）：
//  * 只有 TypeChecker 静态检查已证（形状兼容）且 f64 无 panic 语义的
//    op 才带属性并降级；int、'/' 及动态形状一律留给 LangToLLVM 走
//    sg_*（除零/溢出/越界 panic 语义保持）。
//  * 覆盖集：binop kind ∈ {0=+, 1=-, 2=*}、sum_f、transpose（2D）；
//    ts/st 标量操作数由 MLIRGen 保证为 float（取 sf 槽位）。
// ============================================================================
#pragma once

#include <memory>

namespace mlir {
class Pass;
}

namespace scourge {
namespace aot {

/// 创建 Lang → Affine 降级 pass（ModulePass，partial conversion：未覆盖的
/// lang op 原样保留，由 LangToLinalg / LangToLLVM 兜底）。
/// gpuMode（CUDA 第 1 层）：GPU 覆盖集 op（GpuCover.hpp）跳过本 pass 的
/// 内联降级，留给 LangToLLVM 换发 @sg_gpu_* 运行时调用。
std::unique_ptr<mlir::Pass> createLangToAffinePass(bool gpuMode = false);

} // namespace aot
} // namespace scourge
