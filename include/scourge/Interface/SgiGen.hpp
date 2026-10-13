// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — .sgi 接口文件生成器（设计文档第零章/第六章/第八章）
// ----------------------------------------------------------------------------
//  ".sgi" 是工具生成的接口契约：AI 代理免读实现即可正确调用（降低上下文成本），
//  同时作为接口签名哈希的规范化输入（第五章增量体系）。
//  确定性：输出 = 源码顺序 + 规范化排版；无时间戳、无环境信息。
//  仅 @pure 进入 .sgi（影响跨模块内联语义）；@jit 等实现性装饰器不进入。
// ============================================================================
#pragma once

#include <string>

#include "scourge/Common/Source.hpp"
#include "scourge/Sema/TypeChecker.hpp"
#include "scourge/Syntax/AST.hpp"

namespace scourge {

/// 生成 .sgi 文本（调用前模块必须通过全量检查）。
std::string generateSgi(const SourceMgr& src, const Module& m,
                        const ModuleSymbols& syms);

} // namespace scourge
