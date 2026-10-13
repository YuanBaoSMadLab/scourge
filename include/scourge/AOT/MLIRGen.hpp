// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT MLIR 生成器（SG-AST → 文本 MLIR，设计文档 Stage C）
// ----------------------------------------------------------------------------
//  首条 AOT 竖切：把已通过全量检查的模块图生成为单一文本 MLIR 模块
//  （func / arith / cf / llvm 方言），随后由 Builder 驱动
//    mlir-opt --convert-to-llvm → mlir-translate --mlir-to-llvmir
//    → clang 链接内嵌 C 运行时
//  得到原生可执行文件。
//
//  语义纪律（与字节码后端零分叉）：
//  * 静态类型子集：int / float / bool / str / None / range；其余类型
//    （List / Dict / Tensor / Result / Bytes / Option 等）显式 E1007。
//  * 函数内变量复刻 Compiler 的渐进式局部槽：赋值即建槽、读取先局部后全局。
//  * 溢出/除零/越界等 panic 语义全部下沉到内嵌 C 运行时（sg_* 函数），
//    消息文本与 VM::doBinOp / doBuiltin 逐字一致。
// ============================================================================
#pragma once

#include <string>

namespace scourge {

class Session;

namespace aot {

/// 对已加载并全量检查通过的会话生成文本 MLIR。
/// entry 为入口模块名（load 的文件 stem）；out 收到完整 MLIR 文本。
/// 返回 false 表示生成失败（诊断已写入 session.diag()）。
bool generateMLIR(Session& session, const std::string& entry, std::string& out);

} // namespace aot
} // namespace scourge
