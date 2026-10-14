// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 类型/形状/契约检查器（设计文档第四章，Stage B 核心）
// ----------------------------------------------------------------------------
//  职责与纪律：
//  * 局部推断：变量从初始化表达式推断；模块级函数返回类型可由 return
//    语句推断（0.9.2：多分支取数值宽化、无 return 为 None、递归/前向
//    引用须显式注解）——推断完成即回填 .sgi 接口（设计文档第六章第 3 条）。
//  * 形状检查：Tensor 逐维统一（"?" 动态维吸收具体维），静态不一致即 E2004。
//  * 契约：requires 可静态验证子句在调用点常量求值（E3001）；
//    不可静态验证子句降级为运行时断言并告警（W3001）。
//  * 错误即值：Result 类型的调用结果被丢弃即 E2005（编译器强制消费）。
//  * 失败路径全部以 Type::Err 占位返回，抑制级联错误，保证一次报全错。
// ============================================================================
#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "scourge/Common/Diagnostic.hpp"
#include "scourge/Sema/Type.hpp"
#include "scourge/Syntax/AST.hpp"

namespace scourge {

/// 函数签名（跨模块可见；.sgi 生成亦复用）。
struct FuncSig {
    std::string name;
    std::vector<TypePtr> params;
    TypePtr ret;
    size_t requiredArgs = 0; ///< 无默认值的参数个数（其余在尾部有默认值）
    bool isPure = false;
    bool retPending = false; ///< 返回类型待推断（无注解，由 return 语句统一）

    /// 契约子句引用（表达式借用自 AST，生命周期由 Module 持有保证）。
    struct ContractRef {
        ContractClause::Kind kind;
        const Expr* expr;
    };
    std::vector<ContractRef> contracts;
    const FuncDef* def = nullptr; ///< 供错误提示回溯（诊断/提示用，运行期只读）

    /// 显式拷贝（默认浅拷贝已足够：params/ret/contracts 均为共享/借用语义）。
    /// 检出便于并行检出的模块符号快照脱离源模块独立传递。
    FuncSig copy() const { return *this; }
};

/// 模块导出符号表。
struct ModuleSymbols {
    std::string name;
    std::map<std::string, FuncSig> funcs;
    std::map<std::string, TypePtr> globals;
};

/// 模块名 → 已检查符号表（driver 按 DAG 依赖序填充）。
using ModuleRegistry = std::map<std::string, std::shared_ptr<ModuleSymbols>>;

/// 表达式类型注解表（CINN InferSymbolicShapeContext 思想的前置推导产物）：
/// 检查器是类型/形状推导的单一事实源，checkExpr 的结果按 AST 节点缓存于此；
/// 后端（AOT MLIRGen / 字节码 Compiler）直接查表消费，emit 处只查不重算。
/// 键为节点指针，生命周期由 Session 持有的 AST 保证；Err/Unknown 占位不入表。
using ExprTypeMap = std::unordered_map<const Expr*, TypePtr>;

class TypeChecker {
public:
    TypeChecker(const SourceMgr& src, DiagEngine& diag,
                const ModuleRegistry& imports);

    /// 检查一个模块；返回其导出符号（调用方存入 registry 供下游模块使用）。
    ModuleSymbols check(Module& m);

    /// 取走本轮检查积累的表达式类型注解表（move 语义；检查器实例即弃）。
    ExprTypeMap takeExprTypes() { return std::move(exprTypes_); }

    /// 需要在存储/传参/返回/缺省值边界做 int→float 提升的表达式
    /// （Q3 双模一致：VM 侧据此插转换，与 AOT storeTo 的 sitofp 对齐）。
    std::unordered_set<const Expr*> takePromoteF64() {
        return std::move(promoteF64_);
    }

private:
    // ---- 作用域 ---------------------------------------------------------------
    struct VarSym {
        TypePtr type;
        SourceRange decl;
    };
    class Scope {
    public:
        void push() { stack_.emplace_back(); }
        void pop() { stack_.pop_back(); }
        /// 声明；同层重复返回 false。
        bool declare(const std::string& name, VarSym sym) {
            return stack_.back().emplace(name, std::move(sym)).second;
        }
        const VarSym* find(const std::string& name) const {
            for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
                auto f = it->find(name);
                if (f != it->end()) return &f->second;
            }
            return nullptr;
        }
        /// 仅查最内层（函数层）绑定——for 循环变量复用判定用：
        /// 与外层（模块全局/导入名）同名属正常遮蔽，不算冲突。
        const VarSym* findInBack(const std::string& name) const {
            auto f = stack_.back().find(name);
            return f == stack_.back().end() ? nullptr : &f->second;
        }
    private:
        std::vector<std::unordered_map<std::string, VarSym>> stack_;
    };

    // ---- 主流程 ---------------------------------------------------------------
    void collectTopLevel(Module& m, ModuleSymbols& out);
    void checkFunction(FuncDef& f, ModuleSymbols& out);
    void checkStmtList(const StmtList& list);
    void checkStmt(const Stmt& s);
    void checkContractDefs(const FuncDef& f);

    // ---- 表达式 ----------------------------------------------------------------
    TypePtr checkExpr(const Expr& e);   ///< 公共出口：推断结果写入注解表
    TypePtr checkExprImpl(const Expr& e); ///< 分派实现（switch 各 case）
    TypePtr checkBinary(const Expr& e);
    TypePtr checkCall(const Expr& e);
    TypePtr checkBuiltinCall(const std::string& name, const Call& c);
    TypePtr checkAttr(const Expr& e);
    TypePtr checkIndex(const Expr& e);
    TypePtr checkListLit(const Expr& e);
    TypePtr checkName(const Expr& e);

    // ---- 契约静态验证（调用点） -------------------------------------------------
    struct ConstVal {
        double f = 0;
        int64_t i = 0;
        bool isInt = true;
    };
    bool evalConst(const Expr* e,
                   const std::map<std::string, ConstVal>& env, ConstVal& out) const;
    void verifyRequiresAtCall(const FuncSig& sig, const Expr& call);
    static std::string renderContractExpr(const Expr* e);

    // ---- 基础设施 ---------------------------------------------------------------
    TypePtr resolveTypeExpr(const TypeExpr& te);
    TypePtr builtinType(const std::string& name) const;
    void requireBool(const TypePtr& t, const SourceRange& r, std::string_view what);
    void noteSimilarName(const std::string& name);

    TypePtr fail(Errc code, const SourceRange& r, std::string msg);

    DiagEngine& diag_;
    const ModuleRegistry& imports_;
    FileId file_ = kInvalidFile;

    Scope scope_;
    std::map<std::string, FuncSig> localFuncs_;
    TypePtr fnRet_ = tyErr();       ///< 当前函数返回类型
    /// 返回类型推断中收集的 (return 值表达式, 值类型)——Q10 统一用
    std::vector<std::pair<const Expr*, TypePtr>> pendingReturns_;
    int loopDepth_ = 0;
    bool inAsyncFn_ = false;        ///< 当前函数是否 async def（await 合法性）
    bool checkingEnsures_ = false;  ///< ensures 检查时绑定 result
    ExprTypeMap exprTypes_;         ///< 表达式类型注解表（单一事实源，见上）
    std::unordered_set<const Expr*> promoteF64_; ///< 边界 int→float 提升集
};

} // namespace scourge
