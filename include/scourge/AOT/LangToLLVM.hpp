// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT Lang 方言降级 pass 接口（实现见 AOT/LangToLLVM.cpp）
// ----------------------------------------------------------------------------
//  AOT 步骤 2：把 MLIRGen 产出的 Lang 高层语义 op 一一还原为对内嵌 C 运行时
//  @sg_* 函数的 func.call。流水线位置：parseSourceString → 本 pass →
//  createConvertToLLVMPass → translateModuleToLLVMIR。
// ============================================================================
#pragma once

#include <memory>

namespace mlir {
class Pass;
}

namespace scourge {
namespace aot {

/// 创建 Lang → sg_* 运行时调用降级 pass（ModulePass）。
/// gpuMode（CUDA 第 1 层）：GPU 覆盖集 op（GpuCover.hpp）的调用换为
/// @sg_gpu_*（ABI 与 @sg_* 一致；运行时内部就绪判定，不可用回退 CPU），
/// 并补插对应外部声明。
std::unique_ptr<mlir::Pass> createLangToLLVMPass(bool gpuMode = false);

} // namespace aot
} // namespace scourge
