// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  scourge-mcp.exe — MCP 服务器独立入口（stdio JSON-RPC 2.0，供外部 AI 代理接入）
// ----------------------------------------------------------------------------
//  与 scg mcp 子命令共用 Tool/McpServer 实现（单一事实源）；独立 exe 便于
//  代理配置里直接指向专用可执行，免去 argv0 子命令转发。
//  退出码：0 正常收尾；1 协议/IO 错误。
// ============================================================================
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "scourge/Tool/McpServer.hpp"

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); // 中文诊断在 Windows 控制台正常显示
#endif
    return scourge::cmdMcp();
}
