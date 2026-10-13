// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 工具链版本（单一事实源）
// ----------------------------------------------------------------------------
//  main.cpp 版本输出、run 命令 .sgbc 缓存指纹（工具链标签）、
//  MCP serverInfo 三处共用本常量——改版本只改这里。
//  注意：kScourgeVersionLabel 参与缓存指纹，标签变更会使旧 .sgbc
//  全量失效重编一次（预期行为，不视为回归）。
// ============================================================================
#pragma once

#include <string_view>

namespace scourge {

/// 语义版本号（MCP serverInfo 用）。
inline constexpr std::string_view kScourgeVersion = "0.9.1";

/// 完整版本标签（CLI 输出与 .sgbc 缓存工具链标签）。
inline constexpr std::string_view kScourgeVersionLabel =
    "scourge 0.9.1（AOT 构建：build 命令经进程内 MLIR → LLVM IR → 原生可执行，无外部 mlir-opt/mlir-translate 依赖）";

} // namespace scourge
