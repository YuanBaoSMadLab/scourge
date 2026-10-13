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
std::unique_ptr<mlir::Pass> createLangToLLVMPass();

} // namespace aot
} // namespace scourge
