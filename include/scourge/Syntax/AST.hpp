// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AST（SG-AST）
// ----------------------------------------------------------------------------
//  风格：LLVM 式“Kind 枚举 + static_cast”扁平继承，无虚函数——
//  消费端用 switch(kind) 遍历，结构一目了然，便于后续 MLIR 生成阶段复用。
//  节点生命周期：Parser 构造，unique_ptr 持有；模块级 Module 统一拥有。
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "scourge/Common/Source.hpp"

namespace scourge {

// ============================================================================
// 类型注解（语法层；语义解析在 Sema/Type）
// ============================================================================
struct TypeExpr;
using TypeExprPtr = std::unique_ptr<TypeExpr>;

struct TypeExpr {
    enum class Kind : uint8_t { Named, Tensor, Tuple };

    Kind kind;
    SourceRange range;

    /// Named：类型名与泛型参数（int / List[int] / Dict[str, int] /
    /// Result[T, E] / Option[T] / 自定义），参数语义由 Sema 按名字解释。
    /// Tensor：元素类型 params[0] + 形状 shape。
    /// Tuple：元组类型 (int, str)（0.9.6），params = 各元素类型注解。
    std::string name;
    std::vector<TypeExprPtr> params;

    /// Tensor 专用：形状维度（"?" 表示动态维）
    struct Dim {
        bool isDynamic = false;
        int64_t value = 0;
    };
    std::vector<Dim> shape;

    explicit TypeExpr(Kind k, SourceRange r) : kind(k), range(r) {}
};

// ============================================================================
// 表达式
// ============================================================================
struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    enum class Kind : uint8_t {
        IntLit,   ///< value
        FloatLit, ///< value
        BoolLit,  ///< boolValue
        NoneLit,
        StrLit,  ///< strValue
        Name,    ///< name
        Unary,   ///< op, operand
        Binary,  ///< op, lhs, rhs
        Ternary, ///< cond, thenE, elseE（a if c else b）
        Call,    ///< callee, args
        Index,   ///< base, indices（t[i] / t[i, j]）
        Attr,    ///< base, name（模块成员访问）
        ListLit, ///< elems
        Await,   ///< operand（仅 async def 内合法，E2010）
        TupleLit,///< elems（仅形状注解使用；值位置报错）
        DictLit, ///< pairs（k1,v1,...；0.9.3 字典字面量）
        SetLit   ///< elems（e1,e2,...；0.9.3 集合字面量，元素限标量）
    };

    Kind kind;
    SourceRange range;
    explicit Expr(Kind k, SourceRange r) : kind(k), range(r) {}
    virtual ~Expr() = default;
};

struct IntLit : Expr {
    int64_t value = 0;
    IntLit(SourceRange r, int64_t v) : Expr(Kind::IntLit, r), value(v) {}
};

struct FloatLit : Expr {
    double value = 0;
    FloatLit(SourceRange r, double v) : Expr(Kind::FloatLit, r), value(v) {}
};

struct BoolLit : Expr {
    bool boolValue = false;
    BoolLit(SourceRange r, bool v) : Expr(Kind::BoolLit, r), boolValue(v) {}
};

struct NoneLit : Expr {
    explicit NoneLit(SourceRange r) : Expr(Kind::NoneLit, r) {}
};

struct StrLit : Expr {
    std::string strValue;
    StrLit(SourceRange r, std::string v)
        : Expr(Kind::StrLit, r), strValue(std::move(v)) {}
};

struct Name : Expr {
    std::string name;
    Name(SourceRange r, std::string n) : Expr(Kind::Name, r), name(std::move(n)) {}
};

struct Unary : Expr {
    std::string op; ///< "-" / "+" / "not"
    ExprPtr operand;
    Unary(SourceRange r, std::string o, ExprPtr e)
        : Expr(Kind::Unary, r), op(std::move(o)), operand(std::move(e)) {}
};

struct Binary : Expr {
    std::string op;
    ExprPtr lhs, rhs;
    Binary(SourceRange r, std::string o, ExprPtr l, ExprPtr rh)
        : Expr(Kind::Binary, r), op(std::move(o)), lhs(std::move(l)),
          rhs(std::move(rh)) {}
};

struct Ternary : Expr {
    ExprPtr cond, thenE, elseE;
    Ternary(SourceRange r, ExprPtr c, ExprPtr t, ExprPtr e)
        : Expr(Kind::Ternary, r), cond(std::move(c)), thenE(std::move(t)),
          elseE(std::move(e)) {}
};

struct Call : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    Call(SourceRange r, ExprPtr c, std::vector<ExprPtr> a)
        : Expr(Kind::Call, r), callee(std::move(c)), args(std::move(a)) {}
};

struct Index : Expr {
    ExprPtr base;
    std::vector<ExprPtr> indices; ///< 支持多维下标 t[i, j]
    Index(SourceRange r, ExprPtr b, std::vector<ExprPtr> i)
        : Expr(Kind::Index, r), base(std::move(b)), indices(std::move(i)) {}
};

struct Attr : Expr {
    ExprPtr base;
    std::string name;
    Attr(SourceRange r, ExprPtr b, std::string n)
        : Expr(Kind::Attr, r), base(std::move(b)), name(std::move(n)) {}
};

struct ListLit : Expr {
    std::vector<ExprPtr> elems;
    /// 检查器 checkListLit 判定为张量时置位（编译器据此编为 NewTensor，
    /// 而非 NewList——单一事实源，禁止检查/执行语义分叉）。
    mutable bool checkedAsTensor = false;
    explicit ListLit(SourceRange r, std::vector<ExprPtr> es)
        : Expr(Kind::ListLit, r), elems(std::move(es)) {}
};

/// await 表达式（P0：类型透传操作数；Task/调度器语义随 P2 并发落地）。
struct Await : Expr {
    ExprPtr operand;
    explicit Await(SourceRange r, ExprPtr e)
        : Expr(Kind::Await, r), operand(std::move(e)) {}
};

/// 元组字面量 (a, b, c)：值位置 = 异构定长不可变元组（0.9.6）；形状注解
/// Tensor[T, (…)] 中的括号组不产此节点。
struct TupleLit : Expr {
    std::vector<ExprPtr> elems;
    explicit TupleLit(SourceRange r, std::vector<ExprPtr> es)
        : Expr(Kind::TupleLit, r), elems(std::move(es)) {}
};

/// 字典字面量 {"a": 1, "b": 2}；空 {} 的类型由赋值注解推断。
struct DictLit : Expr {
    std::vector<std::pair<ExprPtr, ExprPtr>> pairs;
    /// 检查器判为空集合时置位（注解 Set[T] 上下文的空 {}）——编译器据此
    /// 编为 NewSet(0)，而非空 Dict（单一事实源，禁止检查/执行语义分叉）。
    mutable bool checkedAsSet = false;
    explicit DictLit(SourceRange r,
                     std::vector<std::pair<ExprPtr, ExprPtr>> ps)
        : Expr(Kind::DictLit, r), pairs(std::move(ps)) {}
};

/// 集合字面量 {1, 2, 3}（0.9.3）：无冒号花括号；元素限标量，插入序去重
/// （语义与 Dict 键一致：数值跨类型同键、NaN 拒绝、迭代序 = 首插序）。
struct SetLit : Expr {
    std::vector<ExprPtr> elems;
    explicit SetLit(SourceRange r, std::vector<ExprPtr> es)
        : Expr(Kind::SetLit, r), elems(std::move(es)) {}
};

// ============================================================================
// 语句
// ============================================================================
struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;
using StmtList = std::vector<StmtPtr>;

struct Stmt {
    enum class Kind : uint8_t {
        Assign,    ///< target, ann?, value
        AugAssign, ///< target, op, value
        ExprStmt,  ///< expr
        If,        ///< cond, thenBody, elifs, elseBody
        While,     ///< cond, body
        For,       ///< var, iter, body
        Return,    ///< value?
        Break,
        Continue,
        Pass,
        Assert, ///< cond, msg?
        Try,    ///< body, handlerBody（panic 恢复路径，文档第四章）
        Defer,  ///< body（函数级确定性清理钩子，return 前 LIFO 展开）
        Import, ///< module, isFrom, names
        Capability, ///< 域名列表（0.9.7 能力声明，仅模块顶层合法）
        FuncDef ///< 见下
    };

    Kind kind;
    SourceRange range;
    explicit Stmt(Kind k, SourceRange r) : kind(k), range(r) {}
    virtual ~Stmt() = default;
};

struct Param {
    std::string name;
    SourceRange range;
    TypeExprPtr type;     ///< 可空：推断不出时报 E2002
    ExprPtr defaultValue; ///< 可空；仅字面量/一元负号字面量
};

struct ContractClause {
    enum class Kind : uint8_t { Requires, Ensures, Invariant };
    Kind kind;
    ExprPtr expr;      ///< 契约表达式（必须为 bool）
    SourceRange range; ///< 表达式在源文件中的真实位置
};

struct Decorator {
    std::string name;
    SourceRange range;
    std::vector<ExprPtr> args;              ///< 位置参数（如 @extern("c")）
    std::vector<std::pair<std::string, ExprPtr>> kwargs; ///< @realtime(deadline_us=...)
};

struct FuncDef : Stmt {
    std::string name;
    SourceRange nameRange;
    std::vector<Param> params;
    TypeExprPtr retType; ///< 可空
    StmtList body;
    std::vector<Decorator> decorators;
    bool isAsync = false;              ///< async def（文档第八章并发原语）
    bool isVariadic = false;           ///< 参数表尾 `...`（仅 @extern 合法，Sema 把关）
    std::string docstring;             ///< 首语句字符串（解码后文本）
    SourceRange docRange;              ///< docstring 在源文件中的区间（含引号）
    std::vector<ContractClause> contracts;

    FuncDef(SourceRange r, std::string n, SourceRange nr)
        : Stmt(Kind::FuncDef, r), name(std::move(n)), nameRange(nr) {}
};

struct Assign : Stmt {
    ExprPtr target;  ///< Name / Index（下标赋值）
    TypeExprPtr ann; ///< 可空：x: int = ...
    ExprPtr value;
    Assign(SourceRange r, ExprPtr t, TypeExprPtr a, ExprPtr v)
        : Stmt(Kind::Assign, r), target(std::move(t)), ann(std::move(a)),
          value(std::move(v)) {}
};

struct AugAssign : Stmt {
    ExprPtr target;
    std::string op;
    ExprPtr value;
    AugAssign(SourceRange r, ExprPtr t, std::string o, ExprPtr v)
        : Stmt(Kind::AugAssign, r), target(std::move(t)), op(std::move(o)),
          value(std::move(v)) {}
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt(SourceRange r, ExprPtr e) : Stmt(Kind::ExprStmt, r), expr(std::move(e)) {}
};

struct If : Stmt {
    ExprPtr cond;
    StmtList thenBody;
    /// elif 链：每个 elif = {cond, body}；else 并入 elseBody。
    struct Elif {
        ExprPtr cond;
        StmtList body;
    };
    std::vector<Elif> elifs;
    StmtList elseBody;
    If(SourceRange r, ExprPtr c, StmtList t)
        : Stmt(Kind::If, r), cond(std::move(c)), thenBody(std::move(t)) {}
};

struct While : Stmt {
    ExprPtr cond;
    StmtList body;
    While(SourceRange r, ExprPtr c, StmtList b)
        : Stmt(Kind::While, r), cond(std::move(c)), body(std::move(b)) {}
};

struct For : Stmt {
    std::string var;
    SourceRange varRange;
    ExprPtr iter;
    StmtList body;
    For(SourceRange r, std::string v, SourceRange vr, ExprPtr it, StmtList b)
        : Stmt(Kind::For, r), var(std::move(v)), varRange(vr), iter(std::move(it)),
          body(std::move(b)) {}
};

struct Return : Stmt {
    ExprPtr value; ///< 可空（return None）
    explicit Return(SourceRange r, ExprPtr v) : Stmt(Kind::Return, r),
                                                value(std::move(v)) {}
};

struct Break : Stmt {
    explicit Break(SourceRange r) : Stmt(Kind::Break, r) {}
};

struct Continue : Stmt {
    explicit Continue(SourceRange r) : Stmt(Kind::Continue, r) {}
};

struct Pass : Stmt {
    explicit Pass(SourceRange r) : Stmt(Kind::Pass, r) {}
};

struct Assert : Stmt {
    ExprPtr cond;
    ExprPtr msg; ///< 可空字符串
    Assert(SourceRange r, ExprPtr c, ExprPtr m)
        : Stmt(Kind::Assert, r), cond(std::move(c)), msg(std::move(m)) {}
};

/// try/except：panic 恢复路径（文档第四章——仅保留给不可恢复条件）。
/// except 不分类别（panic 类别单一化，One Obvious Way）；仅允许单条 except。
struct Try : Stmt {
    StmtList body;        ///< 受保护块
    StmtList handlerBody; ///< except 块（panic 后执行）
    SourceRange exceptRange;
    Try(SourceRange r, StmtList b) : Stmt(Kind::Try, r), body(std::move(b)) {}
};

/// defer：函数级确定性清理钩子（Go 风格）。仅允许出现在函数体顶层；
/// return（显式与函数尾隐式）按 LIFO 内联展开执行词法上位于该 return
/// 之前的 defer 块。panic 传播不经过 return 点，故不触发 defer——
/// 资源清理在 panic 路径请用 try/except（确定性红线：展开点静态可枚举）。
struct Defer : Stmt {
    StmtList body;
    Defer(SourceRange r, StmtList b) : Stmt(Kind::Defer, r), body(std::move(b)) {}
};

struct Import : Stmt {
    std::vector<std::string> path; ///< 点分路径段：import a.b → ["a","b"]（0.9.6 包/子模块）
    bool isFrom = false;           ///< true: from a.b import x as y；false: import a.b [as c]
    /// from 导入项；alias 非空表示 "name as alias"。
    struct Item {
        std::string name;
        std::string alias;
    };
    std::vector<Item> items;      ///< 仅 isFrom 时使用
    std::string alias;            ///< 仅非 from：import a.b as c 的 c（可空）

    Import(SourceRange r, std::vector<std::string> p, bool f,
           std::vector<Item> ns, std::string al = {})
        : Stmt(Kind::Import, r), path(std::move(p)), isFrom(f),
          items(std::move(ns)), alias(std::move(al)) {}

    /// 全限定模块名（点分连接）——加载/注册表/字节码符号统一以此为准。
    std::string module() const {
        std::string out;
        for (size_t i = 0; i < path.size(); ++i) {
            if (i) out += '.';
            out += path[i];
        }
        return out;
    }
};

/// 能力声明（0.9.7）：`capability io, net`——模块顶层软关键字语句。
/// 语义是"本模块声明使用这些能力域"：静态门禁（TypeChecker E5004）与
/// 运行时门禁（Program.capMask / SG_CAPS）共用这里的声明集。
struct Capability : Stmt {
    struct Item {
        std::string name;  ///< 域规范名（io/net/env/proc/time/signal/ffi）
        SourceRange range; ///< 域名记号位置（未知域名诊断定位）
    };
    std::vector<Item> caps;

    Capability(SourceRange r, std::vector<Item> cs)
        : Stmt(Kind::Capability, r), caps(std::move(cs)) {}
};

/// 模块 = 一个 .sg 文件的顶层语句序列。
struct Module {
    FileId file = kInvalidFile;
    std::string name; ///< 文件 stem，如 "foo.sg" → "foo"
    StmtList body;
};

} // namespace scourge
