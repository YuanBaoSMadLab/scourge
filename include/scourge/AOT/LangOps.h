// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — Lang 高层语义方言包装头（AOT 步骤 2）
// ----------------------------------------------------------------------------
//  汇集 LangDialect / Lang op 声明与类体；mlir-tblgen 生成的 .inc 落在
//  build 目录（严禁泄漏源码树），经 CMake 的 lang_gen include 路径可见。
//  v23 布局：类体在 LangOps.h.inc 的 GET_OP_CLASSES 段（可多处包含）；
//  方法定义与显式 TypeID 在 LangOps.cpp.inc 的 GET_OP_CLASSES 段——
//  只能由 LangDialect.cpp 单一 TU 包含（否则链接期重复符号）。
// ============================================================================
#pragma once

#include "mlir/IR/Builders.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OpImplementation.h"

#include "LangDialect.h.inc"
#define GET_OP_CLASSES
#include "LangOps.h.inc"
