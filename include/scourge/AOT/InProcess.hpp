// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge AOT — 进程内 MLIR → LLVM IR 转换接口（Builder 阶段 2+3 合并）
// ----------------------------------------------------------------------------
//  输入 MLIRGen 产出的文本 MLIR，输出 LLVM IR 文本（.ll）。
//  等价于原 mlir-opt --convert-to-llvm + mlir-translate --mlir-to-llvmir
//  两步子进程调用；失败诊断经 diag 返回（供调用方写入 .buildcap）。
// ============================================================================
#pragma once

#include <string>

namespace scourge {
namespace aot {

/// 进程内把文本 MLIR 转换为 LLVM IR 并写出 .ll。返回 true 成功（diag 可能
/// 携带非致命诊断）；返回 false 失败，diag 携带完整错误文本。
/// gpuMode（CUDA 第 1 层）：GPU 覆盖集 op 不做 affine/linalg 内联降级，
/// 改由 LangToLLVM 换发 @sg_gpu_* 运行时调用（.mlir 输入两种模式逐字节
/// 相同——GPU 决策完全发生在本管线内部）。
bool convertToLLVMInProcess(const std::string& mlirText,
                            const std::string& llPath, std::string& diag,
                            bool gpuMode = false);

} // namespace aot
} // namespace scourge
