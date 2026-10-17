// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 工具链版本与编译器自体指纹（单一事实源）
// ----------------------------------------------------------------------------
//  main.cpp 版本输出、run 命令 .sgbc 缓存指纹（工具链标签）、
//  MCP serverInfo、各级缓存键的自体指纹（selfTag）共用本头——改版本
//  或指纹逻辑只改这里。
//  注意：kScourgeVersionLabel / selfTag 均参与缓存指纹，任一变更都会
//  使旧缓存（.sgbc 与 AOT obj）全量失效重编一次（预期行为，不视为回归）。
// ============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

// OS 头（selfTag 取编译器自身路径）。BuilderRT*.hpp 原始字符串内嵌的是
// 生成给用户程序的 C 源码文本，不受此处的宏污染影响。
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace scourge {

/// 语义版本号（MCP serverInfo 用）。
inline constexpr std::string_view kScourgeVersion = "0.1.0";

/// 完整版本标签（CLI 输出与 .sgbc 缓存工具链标签）。
inline constexpr std::string_view kScourgeVersionLabel =
    "scourge 0.1.0（AOT 构建：build 命令经进程内 MLIR → LLVM IR → 原生可执行，无外部 mlir-opt/mlir-translate 依赖）";

/// 编译器自身可执行文件绝对路径（自体指纹与 std/ 安装布局解析共用；
/// 取值失败返回空路径）。
inline std::filesystem::path exePath() {
    std::string path;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) path.assign(buf, n);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) path.assign(buf, static_cast<size_t>(n));
#endif
    return std::filesystem::path(path);
}

/// 编译器自体指纹：自身可执行文件路径 + 大小 + 修改时间。各级缓存键的
/// 指纹源（原 Builder.cpp 本地助手，.sgbc 缓存接入后上移共享——单一
/// 事实源）：任何 TU 重编 → 重链 scourge.exe → 指纹变化 → 缓存全量
/// 失效，无需逐 TU 登记时间戳。路径取值失败时降级为空路径——大小/mtime
/// 退化为 0，指纹仍稳定（仅失去失效灵敏度，不产生错误命中）。
inline std::string selfTag() {
    std::string path = exePath().string();
    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(path, ec);
    uint64_t sizeV = ec ? 0 : static_cast<uint64_t>(size);
    ec.clear();
    auto t = std::filesystem::last_write_time(path, ec);
    uint64_t mtime =
        ec ? 0 : static_cast<uint64_t>(t.time_since_epoch().count());
    return path + "|" + std::to_string(sizeV) + "|" + std::to_string(mtime);
}

} // namespace scourge
