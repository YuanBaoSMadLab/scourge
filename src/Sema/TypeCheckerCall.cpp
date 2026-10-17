// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 类型检查器 · 函数调用
// ----------------------------------------------------------------------------
//  职责边界：checkCall（实参/契约/返回类型）。checkBuiltinCall（内建广度表）
//  在 TypeCheckerBuiltin.cpp——0.9.10 修正批 6 拆分遗留的整份陈旧副本
//  （两处同定义靠 COMDAT 折叠沉默链接，违反单一事实源）。
//  其余阶段见同目录 TypeChecker*.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/Sema/TypeChecker.hpp"

namespace scourge {

TypePtr TypeChecker::checkCall(const Expr& e) {
    auto& c = static_cast<const Call&>(e);
    TypePtr callee;

    if (c.callee->kind == Expr::Kind::Name) {
        auto& n = static_cast<const Name&>(*c.callee);
        // 变量遮蔽优先；随后内建；再后本模块函数。
        // （函数名单独出现才是头等值使用，由 checkName 报 E1007。）
        if (!scope_.find(n.name)) {
            TypePtr b = builtinType(n.name);
            if (b && b->kind == Type::Kind::Builtin) {
                // 能力记录（0.9.7）：调用形态的内建走此分支（不经 checkName），
                // 漏记会使 E5004 静态门禁失效（只剩运行时兜底）。
                Cap cap;
                if (capOfBuiltin(n.name, cap))
                    capUses_.push_back({n.name, cap, n.range});
                return checkBuiltinCall(n.name, c);
            }
            if (auto f = localFuncs_.find(n.name); f != localFuncs_.end())
                callee = makeFunction(f->second.params, f->second.ret);
        }
    }
    if (!callee) callee = checkExpr(*c.callee);
    if (callee->kind == Type::Kind::Err) return tyErr();
    if (callee->kind != Type::Kind::Function) {
        return fail(Errc::E2001_TypeMismatch, c.callee->range,
                    "该表达式不是可调用函数：" + typeToString(callee));
    }

    // Function 类型：params = [参数…] + 末位返回。
    // 缺省实参（Q2）：required = 无默认值参数个数，实参个数落在
    // [required, nParams] 即合法，缺失尾部实参由调用点补默认值。
    size_t nParams = callee->params.size() - 1;
    size_t required = nParams;
    const FuncSig* calleeSig = nullptr;
    if (c.callee->kind == Expr::Kind::Name) {
        auto& n = static_cast<const Name&>(*c.callee);
        if (auto f = localFuncs_.find(n.name); f != localFuncs_.end())
            calleeSig = &f->second;
    } else if (c.callee->kind == Expr::Kind::Attr) {
        auto& at = static_cast<const Attr&>(*c.callee);
        // 0.9.6：base 支持包路径链（a.b.f()）——折叠为全限定模块名再查表。
        std::string qm = qualifiedModuleName(*at.base);
        if (!qm.empty()) {
            if (auto it = imports_.find(qm); it != imports_.end()) {
                if (auto f = it->second->funcs.find(at.name);
                    f != it->second->funcs.end())
                    calleeSig = &f->second;
            }
        }
    }
    if (calleeSig) required = calleeSig->requiredArgs;
    // K 专项：@pure 函数只能调用 @pure 函数（本模块与导入同规则；
    // @extern 及未标注者默认非纯，一律拒绝）。
    if (inPureFn_ && calleeSig && !calleeSig->isPure) {
        std::string nm;
        if (c.callee->kind == Expr::Kind::Name)
            nm = static_cast<const Name&>(*c.callee).name;
        else if (c.callee->kind == Expr::Kind::Attr)
            nm = static_cast<const Attr&>(*c.callee).name;
        return fail(Errc::E1007_UnsupportedFeature, c.callee->range,
                    "@pure 函数内禁止调用非纯函数 '" + nm +
                        "'（被调函数未标注 @pure）");
    }
    // 批 9-③：泛型被调（存在未注解必填参数）不走 retPending 拒绝——
    // 返回类型由实例体按 Q10 推断；跨模块泛型调用在接口面直接拒绝。
    const bool generic = calleeSig && calleeSig->hasUnannotated;
    if (generic && c.callee->kind == Expr::Kind::Attr) {
        return fail(Errc::E2001_TypeMismatch, c.callee->range,
                    "导入函数 '" + static_cast<const Attr&>(*c.callee).name +
                        "' 存在未注解参数——隐式单态化仅限定义模块内部调用；"
                        "请在其定义处显式标注参数类型");
    }
    if (calleeSig && calleeSig->retPending && !generic) {
        // Q10 边界：被调函数返回类型推断未完成（本模块内递归 / 前向引用）。
        // 推断期无类型事实源，静态规则要求显式注解（与 Rust 同理）。
        std::string nm;
        if (c.callee->kind == Expr::Kind::Name)
            nm = static_cast<const Name&>(*c.callee).name;
        else if (c.callee->kind == Expr::Kind::Attr)
            nm = static_cast<const Attr&>(*c.callee).name;
        return fail(Errc::E2001_TypeMismatch, c.callee->range,
                    "函数 '" + nm +
                        "' 的返回类型为自动推断且尚未完成（递归 / 前向引用无法自动推断）；请为其显式注解返回类型");
    }
    // 变参 @extern（O 节 FFI 宽类型域）：实参数下限 = 命名参数个数
    //（extern 无缺省值），上限 20 与 VM 变参调用桩栈槽容量（16+4）对齐——
    // 编译期拦截，避免双模运行期栈越界分歧。
    const bool varCallee = calleeSig && calleeSig->isVariadic;
    if (varCallee) {
        if (c.args.size() < required || c.args.size() > 20) {
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "变参调用实参数不匹配：至少 " +
                            std::to_string(required) + " 个（命名参数），至多 20 个，实际 " +
                            std::to_string(c.args.size()));
        }
    } else if (c.args.size() < required || c.args.size() > nParams) {
        std::string hint = required == nParams
                               ? std::to_string(nParams)
                               : std::to_string(required) + "–" +
                                     std::to_string(nParams) + "（尾部参数有默认值）";
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "实参数量不匹配：期望 " + hint + "，实际 " +
                        std::to_string(c.args.size()));
    }
    // 实参类型检查只遍历调用点提供的实参（Q2：缺失尾部实参由默认值补位）。
    // 泛型被调：未注解参数位以实参类型具体化，逐参类型码拼接即实例键。
    std::vector<TypePtr> monoParams;
    std::string monoKey;
    bool monoFeasible = generic;
    for (size_t i = 0; i < c.args.size(); ++i) {
        TypePtr at = checkExpr(*c.args[i]);
        if (varCallee && i >= nParams) {
            // 变参额外实参：C 变参只收标量，逐个限域（无提升语义）。
            if (!(at->kind == Type::Kind::Int || at->kind == Type::Kind::Float ||
                  at->kind == Type::Kind::Bool || at->kind == Type::Kind::Str))
                fail(Errc::E2001_TypeMismatch, c.args[i]->range,
                     "变参额外实参仅支持 int/float/bool/str，实际 " +
                         typeToString(at));
            continue;
        }
        if (generic) {
            const TypePtr& pt = calleeSig->params[i];
            if (pt->kind == Type::Kind::Unknown) {
                if (at->kind == Type::Kind::Err) {
                    // 错误占位：级联抑制，不再追加诊断。
                    monoParams.push_back(at);
                    monoFeasible = false;
                    continue;
                }
                if (at->kind == Type::Kind::Unknown ||
                    at->kind == Type::Kind::Module ||
                    at->kind == Type::Kind::Builtin ||
                    at->kind == Type::Kind::Function) {
                    fail(Errc::E2002_CannotInfer, c.args[i]->range,
                         "无法从实参推断参数 " + std::to_string(i + 1) +
                             " 的类型（得到 " + typeToString(at) +
                             "）；请为该形参显式标注类型");
                    monoParams.push_back(tyErr());
                    monoFeasible = false;
                    continue;
                }
                monoParams.push_back(at);
                monoKey += mangleType(at);
                continue;
            }
            monoParams.push_back(pt);
            if (at->kind == Type::Kind::Int && pt->kind == Type::Kind::Float)
                promoteF64_.insert(c.args[i].get()); // Q3 边界提升（实参）
            if (!isConvertible(at, pt)) {
                fail(Errc::E2001_TypeMismatch, c.args[i]->range,
                     "实参类型不匹配：参数 " + std::to_string(i + 1) + " 期望 " +
                         typeToString(pt) + "，实际 " + typeToString(at));
            }
            continue;
        }
        if (at->kind == Type::Kind::Int &&
            callee->params[i]->kind == Type::Kind::Float)
            promoteF64_.insert(c.args[i].get()); // Q3 边界提升（实参）
        if (!isConvertible(at, callee->params[i])) {
            fail(Errc::E2001_TypeMismatch, c.args[i]->range,
                 "实参类型不匹配：参数 " + std::to_string(i + 1) + " 期望 " +
                     typeToString(callee->params[i]) + "，实际 " +
                     typeToString(at));
        }
    }
    if (generic) {
        // 缺失尾参（有缺省值）：类型已在收集期由缺省值定型，非泛型位。
        for (size_t i = c.args.size(); i < calleeSig->params.size(); ++i)
            monoParams.push_back(calleeSig->params[i]);
    }

    // 本模块/导入函数的 requires 静态验证
    if (c.callee->kind == Expr::Kind::Name) {
        auto& n = static_cast<const Name&>(*c.callee);
        if (auto f = localFuncs_.find(n.name); f != localFuncs_.end())
            verifyRequiresAtCall(f->second, e);
    } else if (c.callee->kind == Expr::Kind::Attr) {
        auto& at = static_cast<const Attr&>(*c.callee);
        std::string qm = qualifiedModuleName(*at.base);
        if (!qm.empty()) {
            if (auto it = imports_.find(qm); it != imports_.end()) {
                if (auto f = it->second->funcs.find(at.name);
                    f != it->second->funcs.end())
                    verifyRequiresAtCall(f->second, e);
            }
        }
    }

    // 批 9-③：泛型分派——实例查/建 + 被调名改写为实例符号（Sema 是实例
    // 命名的唯一事实源，BC/AOT 按名解析自然命中实例，后端零改动）。
    if (generic) {
        if (!monoFeasible) return tyErr();
        Name* rn = c.callee->kind == Expr::Kind::Name
                       ? static_cast<Name*>(const_cast<Expr*>(c.callee.get()))
                       : nullptr;
        return monoDispatch(*calleeSig,
                            rn ? rn->name : std::string(),
                            std::move(monoParams), monoKey, e, rn);
    }
    return callee->params.back();
}
} // namespace scourge
