// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 类型/形状/契约检查器实现（驱动：共享工具与横切辅助）
// ----------------------------------------------------------------------------
//  职责边界：构造 / fail / 契约静态验证（evalConst / verifyRequiresAtCall /
//  renderContractExpr）/ 类型注解解析（resolveTypeExpr）/ 诊断辅助
//  （noteSimilarName）/ 跨模块可见性（qualifiedModuleName / checkVis）。
//  检查阶段分块：Sym（顶层收集）→ Func（主流程+函数）→ Stmt（语句）→
//  Expr（表达式骨架）→ Lit（字面量索引属性）→ Call（调用内建），
//  见同目录 TypeChecker*.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/Sema/TypeChecker.hpp"

#include <cmath>
#include <functional>
#include <set>

namespace scourge {

TypeChecker::TypeChecker(const SourceMgr& /*src*/, DiagEngine& diag,
                         const ModuleRegistry& imports)
    : diag_(diag), imports_(imports) {}

TypePtr TypeChecker::fail(Errc code, const SourceRange& r, std::string msg) {
    diag_.error(code, file_, r, std::move(msg));
    return tyErr();
}
// ============================================================================
// 契约静态验证（调用点常量求值）
// ============================================================================

bool TypeChecker::evalConst(const Expr* e,
                            const std::map<std::string, ConstVal>& env,
                            ConstVal& out) const {
    switch (e->kind) {
    case Expr::Kind::IntLit:
        out = {double(static_cast<const IntLit*>(e)->value),
               static_cast<const IntLit*>(e)->value, true};
        return true;
    case Expr::Kind::FloatLit:
        out = {static_cast<const FloatLit*>(e)->value, 0, false};
        return true;
    case Expr::Kind::BoolLit:
        out = {static_cast<const BoolLit*>(e)->boolValue ? 1.0 : 0.0,
               static_cast<const BoolLit*>(e)->boolValue ? 1 : 0, true};
        return true;
    case Expr::Kind::Name: {
        auto it = env.find(static_cast<const Name*>(e)->name);
        if (it == env.end()) return false;
        out = it->second;
        return true;
    }
    case Expr::Kind::Unary: {
        auto* u = static_cast<const Unary*>(e);
        ConstVal v;
        if (!evalConst(u->operand.get(), env, v)) return false;
        if (u->op == "-") {
            out = {-v.f, -v.i, v.isInt};
            return true;
        }
        if (u->op == "+") {
            out = v;
            return true;
        }
        return false;
    }
    case Expr::Kind::Binary: {
        auto* b = static_cast<const Binary*>(e);
        ConstVal a, c;
        if (!evalConst(b->lhs.get(), env, a) || !evalConst(b->rhs.get(), env, c))
            return false;
        bool isInt = a.isInt && c.isInt;
        auto asI = [](double d) { return static_cast<int64_t>(d); };
        if (b->op == "+") out = {a.f + c.f, asI(a.f + c.f), isInt};
        else if (b->op == "-") out = {a.f - c.f, asI(a.f - c.f), isInt};
        else if (b->op == "*") out = {a.f * c.f, asI(a.f * c.f), isInt};
        else if (b->op == "/") {
            if (c.f == 0) return false;
            out = {a.f / c.f, 0, false};
        } else if (b->op == "//") {
            if (c.f == 0) return false;
            if (isInt) out = {double(a.i / c.i), a.i / c.i, true};
            else out = {std::floor(a.f / c.f), 0, false};
        } else if (b->op == "%") {
            if (c.f == 0) return false;
            if (isInt) out = {double(a.i % c.i), a.i % c.i, true};
            else out = {std::fmod(a.f, c.f), 0, false};
        } else if (b->op == "**") {
            out = {std::pow(a.f, c.f), 0, false};
        } else if (b->op == "==") out = {a.f == c.f ? 1.0 : 0.0, a.f == c.f, true};
        else if (b->op == "!=") out = {a.f != c.f ? 1.0 : 0.0, a.f != c.f, true};
        else if (b->op == "<") out = {a.f < c.f ? 1.0 : 0.0, a.f < c.f, true};
        else if (b->op == "<=") out = {a.f <= c.f ? 1.0 : 0.0, a.f <= c.f, true};
        else if (b->op == ">") out = {a.f > c.f ? 1.0 : 0.0, a.f > c.f, true};
        else if (b->op == ">=") out = {a.f >= c.f ? 1.0 : 0.0, a.f >= c.f, true};
        else return false;
        return true;
    }
    default:
        return false;
    }
}

void TypeChecker::verifyRequiresAtCall(const FuncSig& sig, const Expr& call) {
    if (sig.contracts.empty() || !sig.def) return;

    auto& c = static_cast<const Call&>(call);
    // 参数名 → 常量值（仅当实参为字面量时可得）。
    std::map<std::string, ConstVal> env;
    const FuncDef& def = *sig.def;
    for (size_t i = 0; i < def.params.size() && i < c.args.size(); ++i) {
        ConstVal v;
        if (evalConst(c.args[i].get(), {}, v)) env[def.params[i].name] = v;
    }

    for (const auto& cr : sig.contracts) {
        if (cr.kind != ContractClause::Kind::Requires) continue;
        ConstVal v;
        if (!evalConst(cr.expr, env, v)) continue; // 非常量 → 运行时断言（已告警）
        if (v.f == 0.0) {
            diag_.error(Errc::E3001_ContractViolated, file_, call.range,
                        "调用违反前置条件 requires: " +
                            renderContractExpr(cr.expr) + "（常量求值为假）");
        }
    }
}

std::string TypeChecker::renderContractExpr(const Expr* e) {
    switch (e->kind) {
    case Expr::Kind::IntLit:
        return std::to_string(static_cast<const IntLit*>(e)->value);
    case Expr::Kind::FloatLit:
        return std::to_string(static_cast<const FloatLit*>(e)->value);
    case Expr::Kind::BoolLit:
        return static_cast<const BoolLit*>(e)->boolValue ? "True" : "False";
    case Expr::Kind::Name:
        return static_cast<const Name*>(e)->name;
    case Expr::Kind::Unary:
        return static_cast<const Unary*>(e)->op + " " +
               renderContractExpr(static_cast<const Unary*>(e)->operand.get());
    case Expr::Kind::Binary: {
        auto* b = static_cast<const Binary*>(e);
        return renderContractExpr(b->lhs.get()) + " " + b->op + " " +
               renderContractExpr(b->rhs.get());
    }
    default:
        return "<表达式>";
    }
}

// ============================================================================
// 类型注解解析（语法层 → 语义层）
// ============================================================================

TypePtr TypeChecker::resolveTypeExpr(const TypeExpr& te) {
    if (te.kind == TypeExpr::Kind::Tensor) {
        TypePtr elem = resolveTypeExpr(*te.params[0]);
        std::vector<int64_t> shape;
        for (const auto& d : te.shape)
            shape.push_back(d.isDynamic ? kDyn : d.value);
        return makeTensor(elem, shape);
    }

    // 元组类型注解 (T1, T2, ...)：params 即各元素类型（0.9.6）。
    if (te.kind == TypeExpr::Kind::Tuple) {
        std::vector<TypePtr> ps;
        ps.reserve(te.params.size());
        for (const auto& p : te.params) ps.push_back(resolveTypeExpr(*p));
        return makeType(Type::Kind::Tuple, std::move(ps));
    }

    const std::string& n = te.name;
    auto noParams = [&](TypePtr t) -> TypePtr {
        if (!te.params.empty()) {
            return fail(Errc::E2001_TypeMismatch, te.range,
                        n + " 不接受泛型参数");
        }
        return t;
    };
    if (n == "int") return noParams(tyInt());
    if (n == "float") return noParams(tyFloat());
    if (n == "bool") return noParams(tyBool());
    if (n == "str") return noParams(tyStr());
    if (n == "bytes") return noParams(makeType(Type::Kind::Bytes));
    if (n == "None") return noParams(tyNone());
    if (n == "Json") return noParams(makeType(Type::Kind::Json));

    if (n == "List") {
        if (te.params.size() != 1)
            return fail(Errc::E2001_TypeMismatch, te.range,
                        "List 需要恰好 1 个元素类型参数");
        return makeType(Type::Kind::List, {resolveTypeExpr(*te.params[0])});
    }
    if (n == "Dict") {
        if (te.params.size() != 2)
            return fail(Errc::E2001_TypeMismatch, te.range,
                        "Dict 需要恰好 2 个类型参数（键, 值）");
        return makeType(Type::Kind::Dict, {resolveTypeExpr(*te.params[0]),
                                           resolveTypeExpr(*te.params[1])});
    }
    if (n == "Set") {
        if (te.params.size() != 1)
            return fail(Errc::E2001_TypeMismatch, te.range,
                        "Set 需要恰好 1 个元素类型参数");
        return makeType(Type::Kind::Set, {resolveTypeExpr(*te.params[0])});
    }
    if (n == "Result") {
        if (te.params.size() != 2)
            return fail(Errc::E2001_TypeMismatch, te.range,
                        "Result 需要恰好 2 个类型参数（成功, 失败）");
        return makeType(Type::Kind::Result, {resolveTypeExpr(*te.params[0]),
                                             resolveTypeExpr(*te.params[1])});
    }
    if (n == "Option") {
        if (te.params.size() != 1)
            return fail(Errc::E2001_TypeMismatch, te.range,
                        "Option 需要恰好 1 个类型参数");
        TypePtr p = resolveTypeExpr(*te.params[0]);
        // 嵌套拒绝（0.9.9 批3）：None 态与 Some(内层 Option) 形态无法
        // 区分，语义歧义——载荷直接用 T（外层 Option 已可表三层状态）。
        if (p->kind == Type::Kind::Option)
            return fail(Errc::E2001_TypeMismatch, te.range,
                        "Option 不支持嵌套（Option[Option[T]]）；载荷直接"
                        "用 T");
        return makeType(Type::Kind::Option, {p});
    }

    return fail(Errc::E2008_UnknownType, te.range, "未知的类型名 '" + n + "'");
}

// ============================================================================
// 辅助
// ============================================================================

void TypeChecker::noteSimilarName(const std::string& name) {
    // 编辑距离 ≤ 2 的确定性建议（fixit 由规则生成，非模型生成）。
    auto dist = [](const std::string& a, const std::string& b) {
        std::vector<size_t> dp(b.size() + 1);
        for (size_t j = 0; j <= b.size(); ++j) dp[j] = j;
        for (size_t i = 1; i <= a.size(); ++i) {
            size_t prev = dp[0];
            dp[0] = i;
            for (size_t j = 1; j <= b.size(); ++j) {
                size_t tmp = dp[j];
                dp[j] = std::min({dp[j] + 1, dp[j - 1] + 1,
                                  prev + (a[i - 1] == b[j - 1] ? 0 : 1)});
                prev = tmp;
            }
        }
        return dp[b.size()];
    };
    for (const auto& kv : localFuncs_) {
        if (dist(name, kv.first) <= 2) {
            Diagnostic note;
            note.code = Errc::E2003_UnknownName;
            note.severity = Severity::Note;
            note.message = "是否有意使用 '" + kv.first + "'？";
            diag_.attachNote(std::move(note));
            return; // 只提示一个最优候选，保持确定性
        }
    }
}

// ============================================================================
// 跨模块（0.9.6 L 节：点分路径/别名/可见性）
// ============================================================================

std::string TypeChecker::qualifiedModuleName(const Expr& base) const {
    // 最内层书写名（首段或别名）→ scope_ 中 Module 类型的真实全限定名
    // （Type::Module::name 由 Import case 按真实模块名填充，别名天然解耦）。
    const Expr* leaf = &base;
    std::vector<std::string> trail; // Attr 链成员段（逆序：外层先收）
    while (leaf->kind == Expr::Kind::Attr) {
        trail.push_back(static_cast<const Attr&>(*leaf).name);
        leaf = static_cast<const Attr&>(*leaf).base.get();
    }
    if (leaf->kind != Expr::Kind::Name) return {};
    const VarSym* sym =
        scope_.find(static_cast<const Name&>(*leaf).name);
    if (!sym || !sym->type || sym->type->kind != Type::Kind::Module)
        return {};
    std::string cur = sym->type->name;
    // 逐段消化书写链（按书写序）：宽容首段绑定（import a.b → a 即 a.b）
    // 使规范形式 a.b.f() 的中间段与真名尾段重合——重合段按路径重写跳过，
    // 其余段下钻子模块（对应全名须在 registry 存在）。重写优先于下钻，
    // 与 checkAttr 的成员解析同序，两路对同一链解析结果一致。
    for (size_t k = trail.size(); k-- > 0;) {
        const std::string& seg = trail[k];
        std::string suffix = '.' + seg;
        if (cur.size() > suffix.size() &&
            cur.compare(cur.size() - suffix.size(), suffix.size(), suffix) ==
                0)
            continue;
        cur += suffix;
        if (!imports_.count(cur)) return {};
    }
    return cur;
}

bool TypeChecker::checkVis(Vis v, const std::string& defModule,
                           const std::string& member, const SourceRange& r) {
    if (v == Vis::Public) return true;
    if (v == Vis::Private) {
        if (curMod_ == defModule) return true;
        diag_.error(Errc::E5003_MissingExport, file_, r,
                    "'" + defModule + "." + member +
                        "' 标注为 @private，仅定义模块内部可见");
        return false;
    }
    // internal：本包 = 模块名首段相同的家族；无点顶层模块无包 → 等价 private。
    size_t cd = curMod_.find('.'), dd = defModule.find('.');
    std::string curPkg =
        cd == std::string::npos ? "" : curMod_.substr(0, cd);
    std::string defPkg =
        dd == std::string::npos ? "" : defModule.substr(0, dd);
    if (!curPkg.empty() && curPkg == defPkg) return true;
    diag_.error(Errc::E5003_MissingExport, file_, r,
                "'" + defModule + "." + member + "' 标注为 @internal，仅包 '" +
                    (defPkg.empty() ? defModule : defPkg) + "' 内可见");
    return false;
}

} // namespace scourge
