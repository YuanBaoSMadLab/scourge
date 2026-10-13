// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 工具链 — MCP 服务器模式（设计文档第六章第 6 条：工具集成零胶水）
// ----------------------------------------------------------------------------
//  "scourge mcp"：stdio 上的 JSON-RPC 2.0 MCP 服务器（换行分隔报文）。
//  把 check / interface / run / REPL 求值暴露为 MCP tools——支持 MCP 的
//  外部 AI 代理框架（IDE / 代理运行时）零胶水接入。
//
//  协议面（MVP）：
//    initialize / notifications/initialized / ping
//    tools/list  → 4 个工具的名称、说明与 inputSchema
//    tools/call  → 按工具名分发，结果以 content 文本序列返回
//
//  纪律：
//    * 协议层只做参数抽取与结果封装；全部语义复用 Session / Runner /
//      Repl 现有实现，与 CLI 命令零分叉；
//    * 协议通道（stdout）绝不写任何日志；
//    * 协议层参数错误走 JSON-RPC -32602；工具执行错误走 result.isError。
// ============================================================================
#pragma once

#include <iosfwd>
#include <memory>

namespace scourge {

class McpServer {
public:
    McpServer(std::istream& in, std::ostream& out);
    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;
    ~McpServer();

    /// 服务循环：逐行读 JSON-RPC 请求直到流结束。返回进程退出码。
    int run();

private:
    struct State; ///< 协议状态与分发实现（McpServer.cpp）
    std::unique_ptr<State> state_;
};

/// CLI 入口：scourge mcp（main.cpp 分发）。
int cmdMcp();

} // namespace scourge
