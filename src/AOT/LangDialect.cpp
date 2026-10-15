// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — Lang 方言注册（AOT 步骤 2）
// ----------------------------------------------------------------------------
//  方言类与 op 类均由 mlir-tblgen 生成（LangOps.td 为单一事实源）：
//  LangDialect.cpp.inc 提供构造函数与析构，LangOps.cpp.inc 提供 op 定义
//  （通用形式解析/打印 + 类型校验）。
//  v23 惯例：initialize() 由本文件手写（上游 Arith 等方言同此）——在此
//  登记全部 op（与 kLowerings 表一一对应，v0.9.4 起 137 个）。
// ============================================================================
#include "mlir/Dialect/LLVMIR/LLVMDialect.h" // Lang_Ptr 校验器引用 LLVMPointerType（须先于 LangOps.h）
#include "scourge/AOT/LangOps.h"

using namespace ::scourge::aot::lang;

#include "LangDialect.cpp.inc"

// op 方法定义 + 显式 TypeID（v23：cpp.inc 的 GET_OP_CLASSES 段；
// 全工程仅此 TU 包含——包装头 LangOps.h 只携类体，避免链接期重复符号）。
#define GET_OP_CLASSES
#include "LangOps.cpp.inc"

namespace scourge {
namespace aot {
namespace lang {

void LangDialect::initialize() {
    addOperations<
#define GET_OP_LIST
#include "LangOps.cpp.inc"
        >();
}

} // namespace lang
} // namespace aot
} // namespace scourge
