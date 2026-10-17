// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 类型检查器 · 隐式单态化（批 9-③）
// ----------------------------------------------------------------------------
//  职责边界：参数注解可选的泛型模板在调用点按实参类型组合实例化——
//  实例键（mangleType）/ 模板体深克隆（cloneFuncDef）/ 调用点分派
//  （monoDispatch）。实例以 "基名__类型键" 独立命名并注册进模块导出表，
//  双模后端（BC/AOT）按名编译实例、零改动；模板本体不检查不编译。
//  其余阶段见同目录 TypeChecker*.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/Sema/TypeChecker.hpp"
#include "scourge/Sema/TypeRegistry.hpp"

namespace scourge {

// ============================================================================
// 实例键：类型 → 类型码
// ============================================================================

// 前缀式注入编码，语义见 TypeRegistry（单一事实源）：标量单字母，复合
// 类型 = 前缀 + 子类型码递归拼接。键只做实例查重比对，永不解析。
std::string TypeChecker::mangleType(const TypePtr& t) {
    return mangleTypeKey(t);
}

// ============================================================================
// FuncDef 深克隆
// ============================================================================

namespace {

std::unique_ptr<TypeExpr> cloneTypeExpr(const TypeExpr& te) {
    auto out = std::make_unique<TypeExpr>(te.kind, te.range);
    out->name = te.name;
    out->shape = te.shape;
    out->params.reserve(te.params.size());
    for (const auto& p : te.params) out->params.push_back(cloneTypeExpr(*p));
    return out;
}

std::unique_ptr<Expr> cloneExpr(const Expr& e) {
    switch (e.kind) {
    case Expr::Kind::IntLit: {
        auto& x = static_cast<const IntLit&>(e);
        return std::make_unique<IntLit>(x.range, x.value);
    }
    case Expr::Kind::FloatLit: {
        auto& x = static_cast<const FloatLit&>(e);
        return std::make_unique<FloatLit>(x.range, x.value);
    }
    case Expr::Kind::BoolLit: {
        auto& x = static_cast<const BoolLit&>(e);
        return std::make_unique<BoolLit>(x.range, x.boolValue);
    }
    case Expr::Kind::NoneLit:
        return std::make_unique<NoneLit>(e.range);
    case Expr::Kind::StrLit: {
        auto& x = static_cast<const StrLit&>(e);
        return std::make_unique<StrLit>(x.range, x.strValue);
    }
    case Expr::Kind::Name: {
        auto& x = static_cast<const Name&>(e);
        return std::make_unique<Name>(x.range, x.name);
    }
    case Expr::Kind::Unary: {
        auto& x = static_cast<const Unary&>(e);
        return std::make_unique<Unary>(x.range, x.op, cloneExpr(*x.operand));
    }
    case Expr::Kind::Binary: {
        auto& x = static_cast<const Binary&>(e);
        return std::make_unique<Binary>(x.range, x.op, cloneExpr(*x.lhs),
                                        cloneExpr(*x.rhs));
    }
    case Expr::Kind::Ternary: {
        auto& x = static_cast<const Ternary&>(e);
        return std::make_unique<Ternary>(x.range, cloneExpr(*x.cond),
                                         cloneExpr(*x.thenE),
                                         cloneExpr(*x.elseE));
    }
    case Expr::Kind::Call: {
        auto& x = static_cast<const Call&>(e);
        std::vector<ExprPtr> args;
        args.reserve(x.args.size());
        for (const auto& a : x.args) args.push_back(cloneExpr(*a));
        return std::make_unique<Call>(x.range, cloneExpr(*x.callee),
                                      std::move(args));
    }
    case Expr::Kind::Index: {
        auto& x = static_cast<const Index&>(e);
        std::vector<ExprPtr> idx;
        idx.reserve(x.indices.size());
        // 单边切片（s[a:] / s[:b]）的缺省端 indices 槽位为 nullptr，
        // 与 ListComp.cond 同判空契约——否则实例克隆解引用空指针。
        for (const auto& a : x.indices)
            idx.push_back(a ? cloneExpr(*a) : nullptr);
        auto out = std::make_unique<Index>(x.range, cloneExpr(*x.base),
                                           std::move(idx));
        out->isSlice = x.isSlice;
        return out;
    }
    case Expr::Kind::Attr: {
        auto& x = static_cast<const Attr&>(e);
        return std::make_unique<Attr>(x.range, cloneExpr(*x.base), x.name);
    }
    case Expr::Kind::ListLit: {
        auto& x = static_cast<const ListLit&>(e);
        std::vector<ExprPtr> es;
        es.reserve(x.elems.size());
        for (const auto& a : x.elems) es.push_back(cloneExpr(*a));
        auto out = std::make_unique<ListLit>(x.range, std::move(es));
        out->checkedAsTensor = x.checkedAsTensor;
        return out;
    }
    case Expr::Kind::Await: {
        auto& x = static_cast<const Await&>(e);
        return std::make_unique<Await>(x.range, cloneExpr(*x.operand));
    }
    case Expr::Kind::TupleLit: {
        auto& x = static_cast<const TupleLit&>(e);
        std::vector<ExprPtr> es;
        es.reserve(x.elems.size());
        for (const auto& a : x.elems) es.push_back(cloneExpr(*a));
        return std::make_unique<TupleLit>(x.range, std::move(es));
    }
    case Expr::Kind::DictLit: {
        auto& x = static_cast<const DictLit&>(e);
        std::vector<std::pair<ExprPtr, ExprPtr>> ps;
        ps.reserve(x.pairs.size());
        for (const auto& kv : x.pairs)
            ps.emplace_back(cloneExpr(*kv.first), cloneExpr(*kv.second));
        auto out = std::make_unique<DictLit>(x.range, std::move(ps));
        out->checkedAsSet = x.checkedAsSet;
        return out;
    }
    case Expr::Kind::SetLit: {
        auto& x = static_cast<const SetLit&>(e);
        std::vector<ExprPtr> es;
        es.reserve(x.elems.size());
        for (const auto& a : x.elems) es.push_back(cloneExpr(*a));
        return std::make_unique<SetLit>(x.range, std::move(es));
    }
    case Expr::Kind::ListComp: {
        auto& x = static_cast<const ListComp&>(e);
        return std::make_unique<ListComp>(x.range, cloneExpr(*x.elem), x.var,
                                          x.varRange, cloneExpr(*x.iter),
                                          x.cond ? cloneExpr(*x.cond) : nullptr);
    }
    }
    return nullptr; // 全 kind 已覆盖（-Wswitch 关闭时的兜底）
}

StmtList cloneStmtList(const StmtList& list);

StmtPtr cloneStmt(const Stmt& s) {
    switch (s.kind) {
    case Stmt::Kind::Assign: {
        auto& x = static_cast<const Assign&>(s);
        return std::make_unique<Assign>(x.range, cloneExpr(*x.target),
                                        x.ann ? cloneTypeExpr(*x.ann)
                                              : nullptr,
                                        cloneExpr(*x.value));
    }
    case Stmt::Kind::AugAssign: {
        auto& x = static_cast<const AugAssign&>(s);
        return std::make_unique<AugAssign>(x.range, cloneExpr(*x.target),
                                           x.op, cloneExpr(*x.value));
    }
    case Stmt::Kind::ExprStmt: {
        auto& x = static_cast<const ExprStmt&>(s);
        return std::make_unique<ExprStmt>(x.range, cloneExpr(*x.expr));
    }
    case Stmt::Kind::If: {
        auto& x = static_cast<const If&>(s);
        auto out = std::make_unique<If>(x.range, cloneExpr(*x.cond),
                                        cloneStmtList(x.thenBody));
        for (const auto& el : x.elifs)
            out->elifs.push_back({cloneExpr(*el.cond), cloneStmtList(el.body)});
        out->elseBody = cloneStmtList(x.elseBody);
        return out;
    }
    case Stmt::Kind::While: {
        auto& x = static_cast<const While&>(s);
        return std::make_unique<While>(x.range, cloneExpr(*x.cond),
                                       cloneStmtList(x.body));
    }
    case Stmt::Kind::For: {
        auto& x = static_cast<const For&>(s);
        return std::make_unique<For>(x.range, x.var, x.varRange,
                                     cloneExpr(*x.iter),
                                     cloneStmtList(x.body));
    }
    case Stmt::Kind::Return: {
        auto& x = static_cast<const Return&>(s);
        return std::make_unique<Return>(x.range,
                                        x.value ? cloneExpr(*x.value)
                                                : nullptr);
    }
    case Stmt::Kind::Break:
        return std::make_unique<Break>(s.range);
    case Stmt::Kind::Continue:
        return std::make_unique<Continue>(s.range);
    case Stmt::Kind::Pass:
        return std::make_unique<Pass>(s.range);
    case Stmt::Kind::Assert: {
        auto& x = static_cast<const Assert&>(s);
        return std::make_unique<Assert>(x.range, cloneExpr(*x.cond),
                                        x.msg ? cloneExpr(*x.msg) : nullptr);
    }
    case Stmt::Kind::Try: {
        auto& x = static_cast<const Try&>(s);
        auto out = std::make_unique<Try>(x.range, cloneStmtList(x.body));
        out->handlerBody = cloneStmtList(x.handlerBody);
        out->exceptRange = x.exceptRange;
        return out;
    }
    case Stmt::Kind::Defer: {
        auto& x = static_cast<const Defer&>(s);
        return std::make_unique<Defer>(x.range, cloneStmtList(x.body));
    }
    case Stmt::Kind::Import: {
        auto& x = static_cast<const Import&>(s);
        return std::make_unique<Import>(x.range, x.path, x.isFrom, x.items,
                                        x.alias);
    }
    case Stmt::Kind::Capability: {
        auto& x = static_cast<const Capability&>(s);
        return std::make_unique<Capability>(x.range, x.caps);
    }
    case Stmt::Kind::FuncDef: {
        auto& x = static_cast<const FuncDef&>(s);
        // 嵌套 def：名字保持原名（实例改名只发生在模板顶层——语言不允许
        // 函数内 def 与模板单态化交互，此臂仅为克隆完整性兜底）。
        auto out = std::make_unique<FuncDef>(x.range, x.name, x.nameRange);
        for (const auto& p : x.params) {
            Param np;
            np.name = p.name;
            np.range = p.range;
            if (p.type) np.type = cloneTypeExpr(*p.type);
            if (p.defaultValue) np.defaultValue = cloneExpr(*p.defaultValue);
            out->params.push_back(std::move(np));
        }
        if (x.retType) out->retType = cloneTypeExpr(*x.retType);
        out->body = cloneStmtList(x.body);
        out->isAsync = x.isAsync;
        out->isVariadic = x.isVariadic;
        out->docstring = x.docstring;
        out->docRange = x.docRange;
        return out;
    }
    }
    return nullptr;
}

StmtList cloneStmtList(const StmtList& list) {
    StmtList out;
    out.reserve(list.size());
    for (const auto& s : list) out.push_back(cloneStmt(*s));
    return out;
}

} // namespace

std::unique_ptr<FuncDef> TypeChecker::cloneFuncDef(const FuncDef& src,
                                                   const std::string& newName) {
    auto out = std::make_unique<FuncDef>(src.range, newName, src.nameRange);
    for (const auto& p : src.params) {
        Param np;
        np.name = p.name;
        np.range = p.range;
        if (p.type) np.type = cloneTypeExpr(*p.type);
        if (p.defaultValue) np.defaultValue = cloneExpr(*p.defaultValue);
        out->params.push_back(std::move(np));
    }
    if (src.retType) out->retType = cloneTypeExpr(*src.retType);
    out->body = cloneStmtList(src.body);
    for (const auto& d : src.decorators) {
        Decorator nd;
        nd.name = d.name;
        nd.range = d.range;
        for (const auto& a : d.args) nd.args.push_back(cloneExpr(*a));
        for (const auto& kv : d.kwargs)
            nd.kwargs.emplace_back(kv.first, cloneExpr(*kv.second));
        out->decorators.push_back(std::move(nd));
    }
    out->isAsync = src.isAsync;
    out->isVariadic = src.isVariadic;
    out->docstring = src.docstring;
    out->docRange = src.docRange;
    for (const auto& c : src.contracts) {
        ContractClause nc;
        nc.kind = c.kind;
        nc.range = c.range;
        nc.expr = cloneExpr(*c.expr);
        out->contracts.push_back(std::move(nc));
    }
    return out;
}

// ============================================================================
// 调用点分派
// ============================================================================

TypePtr TypeChecker::monoDispatch(const FuncSig& tmpl, const std::string& base,
                                  std::vector<TypePtr> concrete,
                                  const std::string& key, const Expr& call,
                                  Name* rewriteCallee) {
    const std::string mangled = base + "__" + key;

    // 既有实例（含在飞：注册先于体检查，体内的自递归/互相递归调用命中
    // 本条且 retPending——沿用 Q10 边界：递归/前向引用无法自动推断返回
    // 类型，须显式注解）。
    if (auto it = curOut_->funcs.find(mangled); it != curOut_->funcs.end()) {
        if (rewriteCallee) rewriteCallee->name = mangled;
        if (it->second.retPending) {
            diag_.error(Errc::E2001_TypeMismatch, file_, call.range,
                        "函数 '" + base +
                            "' 的返回类型为自动推断且调用递归/前向引用"
                            "（无法自动推断）；请为其显式注解返回类型");
            return tyErr();
        }
        return it->second.ret;
    }

    // 深度护栏：无限泛型递归（如 f(x) = f([x])）会生成无限实例序列，
    // 深度上限与 C++ 模板实例化深度 pragmatics 对齐。
    if (monoDepth_ >= 64) {
        diag_.error(Errc::E1007_UnsupportedFeature, file_, call.range,
                    "隐式单态化实例化深度超过上限（可能为无限递归泛型）；"
                    "请为 '" + base + "' 显式标注参数/返回类型");
        return tyErr();
    }

    // 克隆模板体并注册实例：exprTypes_ 按节点指针缓存类型，多实例不得
    // 共享 AST 节点——每实例一份独立克隆。
    auto def = cloneFuncDef(*tmpl.def, mangled);
    FuncSig inst;
    inst.name = mangled;
    inst.params = std::move(concrete);
    inst.ret = tyUnknown();
    inst.retPending = true; // 返回类型按实例体 return 语句推断（Q10 同款）
    inst.isPure = tmpl.isPure;
    inst.requiredArgs = tmpl.requiredArgs;
    inst.vis = Vis::Private; // 实例是模块内部实现符号，跨模块不可见
    inst.isMonoInst = true;
    inst.def = def.get();
    for (const auto& c : def->contracts)
        inst.contracts.push_back({c.kind, c.expr.get()});
    curOut_->funcs[mangled] = std::move(inst);
    FuncSig& live = curOut_->funcs[mangled]; // map 节点稳定，嵌套插入不失效
    monoDefs_.push_back(std::move(def));
    FuncDef* defPtr = monoDefs_.back().get(); // 嵌套分派会使 vector 扩容，
                                              // 先取裸指针（堆地址稳定）
    if (rewriteCallee) rewriteCallee->name = mangled;

    ++monoDepth_;
    checkFunctionBody(*defPtr, live, *curOut_);
    --monoDepth_;
    return live.ret;
}

} // namespace scourge
