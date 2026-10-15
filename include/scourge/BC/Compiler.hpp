// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 字节码编译器（SG-AST → SG-BC，设计文档第五章调试模式）
// ----------------------------------------------------------------------------
//  两遍编译：
//    声明遍：收集模块级函数（含内建注册与缺省参数常量求值）；
//    编译遍：顶层伪函数 + 各函数体。
//  跨模块调用经 ModuleRegistry 解析到统一函数表——与检查器同一接口契约。
//  不支持进入字节码的构造（Dict 值等）显式报错，无静默回退。
//  张量执行已进入（P1）：字面量 → Tensor 常量、逐元素/广播、@、多维下标。
// ============================================================================
#pragma once

#include "scourge/BC/Bytecode.hpp"
#include "scourge/Common/Diagnostic.hpp"
#include "scourge/Sema/TypeChecker.hpp"
#include <unordered_set>

#include "scourge/Syntax/AST.hpp"

#include <map>
#include <string>

namespace scourge {
namespace bc {

class Compiler {
public:
    /// promoteF64：检查器标注的边界 int→float 提升表达式集（Q3 双模一致，
    /// 与 AOT storeTo 的 sitofp 对齐）；可空（工具未接线时退化为旧行为）。
    Compiler(const SourceMgr& src, DiagEngine& diag, const ModuleRegistry& registry,
             const std::unordered_set<const Expr*>* promoteF64 = nullptr);

    /// 编译一个模块（声明遍 + 编译遍一次完成；多次调用累积进同一 Program）。
    bool compileModule(const Module& m);

    Program take() { return std::move(program_); }

private:
    // ---- 符号 -----------------------------------------------------------------
    struct LocalInfo {
        std::map<std::string, uint32_t> slots;
        uint32_t nlocals = 0;
    };

    int declareFunc(const std::string& module, const std::string& name,
                    uint32_t paramCount, uint32_t requiredArgs,
                    std::vector<Value> defaults, bool isAsync);
    /// 解析当前编译上下文可见的函数名 → 函数表索引；-1 表示不可调用名。
    int lookupFunc(const std::string& name) const;
    int lookupModuleFunc(const std::string& module, const std::string& name) const;
    int builtinIndex(const std::string& name) const;
    /// 属性链折叠为规范模块全名（0.9.6 L 节）：首段经 modAlias_ 解析为真实
    /// 模块名，后续段逐级下钻（绑定别名优先，registry 已加载模块兜底）；
    /// 任一段无法解析为模块即返回空串——调用方回退到方法分派等既有路径。
    std::string foldModuleKey(const Expr& base) const;

    // ---- 代码生成 ---------------------------------------------------------------
    void compileTopLevel(const Module& m);
    void compileFunction(const FuncDef& f);
    void compileStmt(const Stmt& s);
    void compileStmtList(const StmtList& list);
    void emitDefers(); ///< return 点 LIFO 内联展开 defer 块（编译期展开）
    void compileExpr(const Expr& e);
    void emit(Op op, uint32_t a = 0, uint32_t b = 0);
    void markLine(uint32_t line);
    uint32_t here() const { return static_cast<uint32_t>(fn_->code.size()); }
    void patchJump(uint32_t at);                 ///< 目标 = 当前 pc
    void patchTo(uint32_t at, uint32_t target);  ///< 目标 = 指定 pc
    uint32_t emitJump(Op op); ///< 占位跳转，返回待 patch 位置

    uint32_t addConst(Value v);
    uint32_t addGlobal(const std::string& module, const std::string& name);
    Value constOf(const Expr& e) const; ///< 字面量（含一元负号）→ Value
    Value constTensorOf(const ListLit& ll); ///< 张量字面量 → Tensor 常量

    // ---- 基础设施 ----------------------------------------------------------------
    [[noreturn]] void unsupported(const Expr& e, const std::string& what);
    [[noreturn]] void error(const SourceRange& r, const std::string& msg);
    int lineOf(const SourceRange& r) const;

    const SourceMgr& src_;
    DiagEngine& diag_;
    const ModuleRegistry& registry_;
    const std::unordered_set<const Expr*>* promoteF64_ = nullptr;

    /// 该表达式是否需要在当前存储/传参/返回边界提升为 float（Q3）。
    bool needsF64(const Expr& e) const {
        return promoteF64_ && promoteF64_->count(&e) > 0;
    }
    FileId file_ = kInvalidFile;
    std::string moduleName_;
    Program program_;

    FuncBC* fn_ = nullptr;     ///< 当前编译函数
    LocalInfo* locals_ = nullptr; ///< 当前函数局部表
    int loopDepth_ = 0;
    std::vector<std::vector<uint32_t>> breakJumps_, continueJumps_; ///< 每层循环一组待 patch 跳转
    std::vector<size_t> loopEntryTries_;  ///< 进入循环时的 openTries_ 深度
    std::vector<uint32_t> openTries_;     ///< 未闭合 try 的 EnterTry 指令位置
    /// 当前函数已声明的 defer 块（词法序）——return 点按 LIFO 内联展开
    std::vector<const StmtList*> deferStack_;

    /// 当前模块可见的函数名 → 索引（内建 + 本模块 + from-import 函数别名）。
    std::map<std::string, int> visibleFuncs_;
    /// 规范模块全名 → 该模块函数的索引表（跨模块调用解析）。
    std::map<std::string, std::map<std::string, int>> importedFuncs_;
    /// 绑定键 → 规范模块全名（全限定名/首段/末段宽容/别名，0.9.6 L 节）。
    /// 与检查器"绑定名-真名解耦"同构：属性链折叠时还原真实模块名。
    std::map<std::string, std::string> modAlias_;
    /// from-import 全局别名 → (源模块全名, 源名)——Name 引用的跨模块
    /// LoadGlobal 重定向（0.9.6 L 节）。
    std::map<std::string, std::pair<std::string, std::string>> fromGlobals_;
};

} // namespace bc
} // namespace scourge
