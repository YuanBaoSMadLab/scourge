// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 工具链 — build 命令：AOT 原生可执行构建（设计文档第二章 Stage C）
// ----------------------------------------------------------------------------
//  流水线：加载 → 全量检查（与 check/run 同一门禁）→ MLIR 生成 →
//          mlir-opt --convert-to-llvm → mlir-translate --mlir-to-llvmir →
//          clang 链接内嵌 C 运行时。
//  工具链定位：环境变量 SCOURGE_LLVM_BIN（默认 D:/LLVM/bin）。
// ============================================================================
#pragma once

#include <filesystem>
#include <string>

namespace scourge {

/// 一次 AOT 构建结果。
struct BuildOutcome {
    int exitCode = 0;
    std::string log; ///< 失败诊断与流水线工具输出（人类通道，CLI 写 stderr）
    std::filesystem::path exe; ///< 成功时的可执行产物路径
};

/// 「加载 → 全量检查 → MLIR 生成 → 进程内 MLIR→LLVM IR → clang」一条龙。
/// 中间产物（.mlir / .ll / _rt.c）与可执行文件同目录，便于检视。
/// gpuMode：--gpu 开关——GPU 覆盖集 op（f64 静态形状逐元素/matmul/sum/
/// 转置）在生成代码中优先走 CUDA 动态加载路径，运行时不可用自动回退 CPU。
BuildOutcome buildOnce(const std::filesystem::path& inPath,
                       const std::filesystem::path& outPath,
                       bool gpuMode = false);

} // namespace scourge
