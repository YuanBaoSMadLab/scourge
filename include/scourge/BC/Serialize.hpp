// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge SG-BC 序列化 — Program ↔ 字节流（设计文档第五章"增量一切"）
// ----------------------------------------------------------------------------
//  用途：run 命令的 .sgbc 缓存。缓存命中 = 工具链版本 + 源指纹 + 入口 +
//  模块集完全一致；任何不匹配一律按缓存未命中处理（全量重编），
//  绝不加载可疑数据——"显式拒绝，无静默回退"。
//
//  格式纪律：
//    * 小端布局，逐字段显式编码（不做结构体整体 memcpy，跨构建稳定）；
//    * 读取全程边界校验，任何越界 / 非法枚举 / 计数虚高 / 尾随垃圾
//      → decode 失败；
//    * 内容完整性由源指纹与工具链标签守护（本地缓存，非对抗性威胁模型）。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "scourge/BC/Bytecode.hpp"

namespace scourge {
namespace bc {

/// 缓存头元数据（随 Program 一同编码）。
struct CacheMeta {
    std::string toolchain;            ///< 工具链标签（kVersion；不匹配即失效）
    std::string entry;                ///< 入口模块名
    std::vector<std::string> modules; ///< 依赖层序模块名（被依赖者在前）
    std::vector<std::string> paths;   ///< 各模块源文件路径（panic 定位）
    uint64_t sourceHash = 0;          ///< 源指纹（FNV-1a 64）
};

/// FNV-1a 64：源指纹的确定性哈希（无加密需求，只求稳定防意外损坏）。
uint64_t fnv1a64(const void* data, size_t len,
                 uint64_t seed = 0xcbf29ce484222325ull);
inline uint64_t fnv1a64(std::string_view s,
                        uint64_t seed = 0xcbf29ce484222325ull) {
    return fnv1a64(s.data(), s.size(), seed);
}

/// 编码（确定性：相同输入逐字节相同，缓存文件可复现）。
std::string encodeProgram(const Program& p, const CacheMeta& meta);

/// 解码 + 全量结构校验；任何不一致返回 false（输出参数内容未定）。
bool decodeProgram(const std::string& bytes, Program& p, CacheMeta& meta);

} // namespace bc
} // namespace scourge
