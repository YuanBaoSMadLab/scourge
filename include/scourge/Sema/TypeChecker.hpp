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

#include "scourge/Common/Capability.hpp"
#include "scourge/Common/Diagnostic.hpp"
#include "scourge/Sema/Type.hpp"
#include "scourge/Syntax/AST.hpp"

namespace scourge {

/// 成员可见性（0.9.6 L 节）：public 默认；private 仅本模块；
/// internal 仅本包（模块名首段相同的家族；无点顶层模块等价 private）。
enum class Vis { Public, Private, Internal };

/// 函数签名（跨模块可见；.sgi 生成亦复用）。
struct FuncSig {
    std::string name;
    std::vector<TypePtr> params;
    TypePtr ret;
    size_t requiredArgs = 0; ///< 无默认值的参数个数（其余在尾部有默认值）
    bool isPure = false;
    bool retPending = false; ///< 返回类型待推断（无注解，由 return 语句统一）
    bool isExtern = false;   ///< @extern：动态库外部函数（体不发射，H 专项）
    bool isVariadic = false; ///< 参数表尾 ...（仅 @extern 合法，O 节 FFI 宽类型域）
    Vis vis = Vis::Public;   ///< @private/@internal（0.9.6 L 节）
    /// 存在未注解必填参数（批 9-③ 隐式单态化）：签名以 Unknown 占位成
    /// 泛型模板，类型事实由调用点按实参组合实例化产生；仅限定义模块内部
    /// 调用（.sgi 接口面不含——跨模块签名必须完整）。
    bool hasUnannotated = false;
    /// 隐式单态化实例（名字 = 模板基名 + "__" + 类型键）：模块内部实现
    /// 符号，Vis::Private + .sgi 跳过；进注册表供双模后端按名编译。
    bool isMonoInst = false;

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
    /// 以给定签名检查函数体（checkFunction 与单态化实例共用的主体；
    /// 上下文保存/恢复保证调用者体检查中途的嵌套实例化可重入）。
    void checkFunctionBody(FuncDef& f, FuncSig& sig, ModuleSymbols& out);
    /// 缺省值检查（收集期与实例补查共用单一事实源）：容器字面量对齐参数
    /// 注解 + int→float 边界提升，返回缺省值表达式类型。
    TypePtr checkParamDefault(const Param& p, const TypePtr& t);
    /// 泛型调用点分派（批 9-③）：查/建实例、以具体参数类型检查实例体、
    /// 返回实例返回类型；成功时把 *rewriteCallee 改写为实例符号
    ///（Sema 是实例命名的唯一事实源，BC/AOT 按名解析零改动）。
    TypePtr monoDispatch(const FuncSig& tmpl, const std::string& base,
                         std::vector<TypePtr> concrete, const std::string& key,
                         const Expr& call, Name* rewriteCallee);
    /// 类型 → 实例名类型码（前缀式注入编码：i/f/b/s/n/y/j/r 标量，
    /// L/S/D/T/R/O/X 复合递归；仅作实例键比对，永不解析回类型）。
    static std::string mangleType(const TypePtr& t);
    /// FuncDef 深克隆：exprTypes_ 按节点指针缓存类型，同一模板的多个
    /// 实例不得共享 AST 节点——每实例一份独立克隆。
    static std::unique_ptr<FuncDef> cloneFuncDef(const FuncDef& src,
                                                 const std::string& newName);
    void checkStmtList(const StmtList& list);
    void checkNested(const StmtList& list); ///< 嵌套块检查（blockDepth_ 供 defer 顶层限制）
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
    TypePtr checkListComp(const Expr& e);
    TypePtr checkDictLit(const Expr& e);
    TypePtr checkSetLit(const Expr& e);
    TypePtr checkTupleLit(const Expr& e);
    TypePtr checkName(const Expr& e);
    /// 迭代器类型 → 元素类型（for 语句与列表推导式共用；覆盖 range/List/
    /// str/Dict/Set，不支持返回 Err，报错措辞由调用方定夺）。
    TypePtr iterElemOf(const TypePtr& it) const;

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

    // ---- 跨模块（0.9.6 L 节：点分路径/别名/可见性） ----------------------------
    /// 折叠模块限定前缀：Name（"m"）或 Attr 链（"a.b"）→ 全限定模块名；
    /// 逐段验证子模块在 imports_ 中存在，失败返回空串。
    std::string qualifiedModuleName(const Expr& base) const;
    /// 可见性门禁：违规报 E5003 并返回 false（curMod_ 为当前模块全名）。
    bool checkVis(Vis v, const std::string& defModule,
                  const std::string& member, const SourceRange& r);

    TypePtr fail(Errc code, const SourceRange& r, std::string msg);

    DiagEngine& diag_;
    const ModuleRegistry& imports_;
    FileId file_ = kInvalidFile;
    std::string curMod_; ///< 当前检查模块全名（可见性判定基准）

    Scope scope_;
    std::map<std::string, FuncSig> localFuncs_;
    TypePtr fnRet_ = tyErr();       ///< 当前函数返回类型
    /// 返回类型推断中收集的 (return 值表达式, 值类型)——Q10 统一用
    std::vector<std::pair<const Expr*, TypePtr>> pendingReturns_;
    int loopDepth_ = 0;
    bool inAsyncFn_ = false;        ///< 当前函数是否 async def（await 合法性）
    bool checkingEnsures_ = false;  ///< ensures 检查时绑定 result
    bool inFunc_ = false;           ///< 当前是否在函数体内（defer 位置限制）
    /// 当前在 @pure 函数体内（K 专项：纯度静态强制——禁非纯内建/非纯函数）
    bool inPureFn_ = false;
    int blockDepth_ = 0;            ///< 嵌套块深度（if/while/for/try 体；defer 顶层限制）
    bool inDefer_ = false;          ///< 当前在 defer 块内（禁 return/break/continue/defer）
    ExprTypeMap exprTypes_;         ///< 表达式类型注解表（单一事实源，见上）
    std::unordered_set<const Expr*> promoteF64_; ///< 边界 int→float 提升集
    /// Tensor 上下文偏好（单发旗标）：赋值目标为 Tensor 注解/既有 Tensor
    /// 变量时指向右侧列表字面量，checkListLit 据此把一维数值字面量按
    /// Tensor 推断（0.9.10 F-18 A 案：默认数值字面量 = List[T]，嵌套
    /// [[..]] 恒为张量——AOT SgList 槽位只存标量，嵌套 List 不可表示）。
    /// 检查完表达式立即清空。
    const Expr* asTensorLit_ = nullptr;
    /// Dict 上下文偏好（单发旗标，同 asTensorLit_ 架构）：赋值/缺省值目标为
    /// Dict[K,V] 注解（或既有 Dict 变量）时指向目标类型，checkDictLit
    /// 据此把键/值统一对齐注解（int 元素按槽位提升）——否则字面量自推断
    /// Dict{float,int} 与变量注解 Dict{float,float} 槽位语义脱节，读取侧
    /// 按变量注解位还原即漂移。检查完表达式立即清空。
    const Type* dictTarget_ = nullptr;
    /// Set 上下文偏好（单发旗标，同 dictTarget_ 架构）：赋值/缺省值目标为
    /// Dict[K,V]/Set[T] 注解（或既有变量）时指向目标类型，checkSetLit 据此把
    /// 元素统一对齐注解（int 元素提升）；空 {} 在 Set 上下文判为空集合
    /// （checkedAsSet）。检查完表达式立即清空。
    const Type* setTarget_ = nullptr;
    /// Tuple 上下文偏好（单发旗标，同 dictTarget_ 架构，0.9.9 批3）：赋值
    /// 目标为元组注解（或既有元组变量）时指向目标类型，checkTupleLit 据此
    /// 逐位对齐——int 元素遇 float 位打提升标记并改写推断位，AOT 装箱
    /// （genTupleLit）按位消费，杜绝 int 位型存槽 / float 位读型的错位。
    /// 检查完表达式立即清空。
    const Type* tupleTarget_ = nullptr;

    // ---- 能力门禁（0.9.7）------------------------------------------------------
    /// 本模块 capability 声明集（collectTopLevel 扫描并集；check 开头重置）。
    uint64_t declaredCaps_ = 0;
    /// 能力使用点：需授权内建名 / @extern 函数名 + 域 + 首个使用位置。
    /// check 收尾统一裁决——每个未声明域报一条 E5004（定位首次使用）。
    struct CapUse {
        std::string what; ///< 内建名或 @extern 函数名（诊断提示用）
        Cap cap;
        SourceRange range;
    };
    std::vector<CapUse> capUses_;

    // ---- 隐式单态化状态（批 9-③）----------------------------------------------
    ModuleSymbols* curOut_ = nullptr; ///< check() 期间的导出符号表（实例注册目标）
    /// 实例克隆体所有权；check 收尾并入 m.body，双模后端遍历模块体按名编译。
    std::vector<std::unique_ptr<FuncDef>> monoDefs_;
    int monoDepth_ = 0; ///< 实例化深度（防无限泛型递归，如 f(x) = f([x])）
};

} // namespace scourge
