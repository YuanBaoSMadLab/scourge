// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 工具链 — run 执行核心（CLI 与 MCP 服务器共用，零语义分叉）
// ----------------------------------------------------------------------------
//  从 main.cpp 提取：一次完整「加载 → 缓存判定 → 检查/编译 → VM 执行」。
//  通道纪律：print 程序输出与失败文本物理分离——
//    * print 输出：captureOut=false 流式写 stdout；true 捕获进 outcome.output；
//    * 失败文本（诊断 / panic / 中止说明）：一律写入 outcome.log，
//      CLI 写 stderr，MCP 工具结果作为第二段 content 文本返回。
// ============================================================================
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace scourge {

/// 单次执行结果（cmdRun / run --watch / MCP scourge_run 工具共用）。
struct RunOutcome {
    int exitCode = 0;
    std::vector<std::filesystem::path> modulePaths; ///< 涉及的全部模块源路径（--watch 轮询）
    std::string output; ///< print 输出（captureOut=true 时捕获）
    std::string log;    ///< 失败诊断与 panic 文本（人类通道）
};

/// 一次完整「加载 → 缓存判定 → 检查/编译 → VM 执行」。
RunOutcome runOnce(const std::filesystem::path& inPath, bool useCache,
                   bool captureOut);

} // namespace scourge
