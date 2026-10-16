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
    bool fromCache = false; ///< AOT obj 缓存命中（跳过检查/编译，仅链接）
};

/// 交叉编译目标描述（0.9.4 跨平台支持）。target 为空 = 本机原生构建。
/// POSIX 目标（*linux-gnu 等）走 musl 静态 sysroot（toolchain/
/// build_musl_sysroot.sh 产出）+ ld.lld 直链；Windows 目标沿用 clang 驱动。
struct CrossOpts {
    std::string target;  ///< LLVM target triple（x86_64-linux-gnu、
                         ///< aarch64-linux-gnu、riscv64-linux-gnu、
                         ///< arm-linux-gnueabihf …）
    std::string sysroot; ///< 目标 sysroot（musl 静态 libc + 头文件目录）
    std::string cpu;     ///< -mcpu（cortex-a53、cortex-a72…；可空）
    std::string march;   ///< -march（armv7-a、rv64gc…；可空）
    bool staticLink = false; ///< 静态链接（交叉 POSIX 目标必需）
};

/// 「加载 → 缓存判定 → 全量检查 → MLIR 生成 → 进程内 MLIR→LLVM IR →
///  clang 分段编译 → 链接」一条龙。
/// 中间产物（.mlir / .ll / _rt.c）与可执行文件同目录，便于检视；
/// obj 缓存（设计文档 5.2 双缓存体系的 AOT 段）位于 <stem>.sgbuild/obj/：
/// main.obj（键：格式版本/工具链/源指纹/优化档+gpuMode/入口/交叉目标）与
/// rt.obj（键：格式版本/工具链/gpuMode/运行时源文本/交叉目标，源无关）。
/// 键全等才命中，任一不符全量重编；缓存写失败静默降级为无缓存直编
/// （对齐 .sgbc 纪律）。
/// gpuMode：--gpu 开关——GPU 覆盖集 op（f64 静态形状逐元素/matmul/sum/
/// 转置）在生成代码中优先走 CUDA 动态加载路径，运行时不可用自动回退 CPU。
/// xo：交叉编译选项（target 空 = 本机原生；缓存键计入目标/优化参数）。
BuildOutcome buildOnce(const std::filesystem::path& inPath,
                       const std::filesystem::path& outPath,
                       bool gpuMode = false, bool useCache = true,
                       const CrossOpts& xo = {});

} // namespace scourge
