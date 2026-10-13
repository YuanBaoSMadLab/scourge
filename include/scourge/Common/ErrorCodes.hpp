// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 稳定错误码表（设计文档第六章/十二章第 5 条）
// ----------------------------------------------------------------------------
//  编号即契约：一旦发布，编号含义永不变更、永不复用。
//  分族规则：
//    E1xxx 语法（Stage A 解析器 / 词法器）
//    E2xxx 类型与形状（Stage B 类型系统）
//    E3xxx 契约（requires/ensures/invariant）
//    E4xxx 内存与并发（P1+ 预留，P0 不产出）
//    E5xxx 外部边界（模块导入 / 后续：FFI、包）
//    Wxxxx 警告（同族语义降级提示）
//
//  编码规则：枚举值即显示编号（E 族）；警告码存储值 = 10000 + 显示编号，
//  例如 W3001（契约降级）存储为 13001。errcCode() 负责换算显示串。
//
//  docs/error-codes.md 由本表人工同步维护；新增码必须：
//  1) 顺延未用编号，不复用、不插队；2) 附 JSON 诊断样例。
// ============================================================================
#pragma once

#include <cstdint>
#include <string_view>

namespace scourge {

enum class Errc : uint16_t {
    // ---- E1xxx 语法 -------------------------------------------------------
    E1001_IllegalChar = 1001,        ///< 非法字符（不属语言字符集）
    E1002_UnterminatedString = 1002, ///< 字符串未终止（行尾前未闭合）
    E1003_BadNumber = 1003,          ///< 数字字面量格式错误
    E1004_BadIndentation = 1004,     ///< 缩进不一致（含 Tab 缩进）
    E1005_UnexpectedToken = 1005,    ///< 意外的记号 / 缺少期望记号
    E1006_InvalidAssignTarget = 1006,///< 非法赋值目标
    E1007_UnsupportedFeature = 1007, ///< 明确拒绝清单中的特性（无宏/eval 等）

    // ---- E2xxx 类型与形状 -------------------------------------------------
    E2001_TypeMismatch = 2001,       ///< 类型不匹配（运算/赋值/返回/实参）
    E2002_CannotInfer = 2002,        ///< 类型推断失败，需要显式注解
    E2003_UnknownName = 2003,        ///< 未声明的标识符
    E2004_ShapeMismatch = 2004,      ///< 张量形状不兼容
    E2005_ResultNotConsumed = 2005,  ///< Result 值被丢弃（错误即值，强制消费）
    E2006_ReturnTypeConflict = 2006, ///< 多个 return 分支类型不一致
    E2007_DuplicateDefinition = 2007,///< 同作用域重复定义
    E2008_UnknownType = 2008,        ///< 未知的类型名
    E2009_BreakOutsideLoop = 2009,   ///< break/continue 不在循环内
    E2010_AwaitOutsideAsync = 2010,  ///< await 出现在非 async 函数内

    // ---- E3xxx 契约 ---------------------------------------------------------
    E3001_ContractViolated = 3001,   ///< 调用点静态违反 requires 子句
    E3002_ContractTypeError = 3002,  ///< 契约表达式类型错误（必须为 bool）
    W3001_ContractDowngraded = 13001,///< (W3001) 契约不可静态验证，降级为运行时断言

    // ---- E4xxx 内存与并发（预留，P1+ 产出）--------------------------------
    // （暂无编号占用；新码从 4001 顺延）

    // ---- E5xxx 外部边界 -----------------------------------------------------
    E5001_ModuleNotFound = 5001,     ///< import 目标模块未找到
    E5002_ImportCycle = 5002,        ///< import 构成循环依赖（DAG 前提被破坏）
    E5003_MissingExport = 5003,      ///< from-import 的成员在目标模块不存在
};

/// 输出形如 "E2001" 的稳定字符串。
std::string_view errcCode(Errc e);

/// 错误族（用于 JSON 诊断的 "category" 字段与文档分组）。
std::string_view errcCategory(Errc e);

} // namespace scourge
