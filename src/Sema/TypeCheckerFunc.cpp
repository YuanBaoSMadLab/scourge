// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 类型检查器 · 模块检查主流程与函数检查
// ----------------------------------------------------------------------------
//  职责边界：check（全模块门禁编排）/ checkFunction（函数体检查）/ checkContractDefs（requires/ensures 静态验证与降级判定）。
//  其余阶段见同目录 TypeChecker*.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/Sema/TypeChecker.hpp"

#include <cmath>
#include <functional>
#include <set>

namespace scourge {

// ============================================================================
// 模块检查主流程
// ============================================================================

ModuleSymbols TypeChecker::check(Module& m) {
    file_ = m.file;
    curMod_ = m.name; // 可见性判定基准（0.9.6 L 节）
    ModuleSymbols out;
    out.name = m.name;
    declaredCaps_ = 0; // 每模块重置：声明/使用都是模块局部的
    capUses_.clear();

    scope_.push(); // 模块作用域
    collectTopLevel(m, out);

    for (auto& stmt : m.body) {
        if (stmt->kind == Stmt::Kind::FuncDef)
            checkFunction(static_cast<FuncDef&>(*stmt), out);
    }

    // 能力静态门禁（0.9.7）：使用集 ⊄ 声明集 → 每个未声明域一条 E5004，
    // 定位首个使用点。此处是双模共用的主门禁；VM/AOT 的运行时查位只是
    // 纵深防御（正常程序到不了）。
    uint64_t reportedDoms = 0;
    for (const auto& u : capUses_) {
        uint64_t bit = capBit(u.cap);
        if ((bit & declaredCaps_) || (bit & reportedDoms)) continue;
        reportedDoms |= bit; // 同域多使用点只报首个（域级门禁）
        std::string dom(capName(u.cap));
        diag_.error(Errc::E5004_CapabilityNotGranted, file_, u.range,
                    "'" + u.what + "' 需要能力 '" + dom +
                        "'——在模块首部添加 capability " + dom);
    }
    return out;
}

void TypeChecker::checkFunction(FuncDef& f, ModuleSymbols& out) {
    auto it = localFuncs_.find(f.name);
    if (it == localFuncs_.end()) return; // 签名阶段已报错

    fnRet_ = it->second.ret;
    pendingReturns_.clear();
    scope_.push();
    loopDepth_ = 0;
    const bool prevAsync = inAsyncFn_;
    inAsyncFn_ = f.isAsync;
    const bool prevInFunc = inFunc_;
    inFunc_ = true; // defer 仅函数体顶层合法（模块顶层无退出点）
    const bool prevPure = inPureFn_;
    inPureFn_ = it->second.isPure; // K 专项：@pure 函数体启用纯度强制
    for (size_t i = 0; i < f.params.size(); ++i) {
        if (!scope_.declare(f.params[i].name,
                            VarSym{it->second.params[i], f.params[i].range})) {
            diag_.error(Errc::E2007_DuplicateDefinition, file_,
                        f.params[i].range,
                        "参数 '" + f.params[i].name + "' 重复");
        }
    }

    // 契约定义侧：类型必须为 bool；不可静态验证子句降级告警（W3001）。
    checkContractDefs(f);

    checkStmtList(f.body);

    // 返回类型推断（Q10）：统一全部 return 的值类型并回填签名。
    // 递归自引用（Unknown）跳过；数值混返回取宽化（int+float → float，
    // 与 isConvertible 的唯一隐式转换方向一致）；不一致即 E2006。
    if (it->second.retPending) {
        TypePtr inferred = tyNone(); // 无 return / 全裸 return → None
        bool any = false, conflict = false;
        for (const auto& [expr, vt] : pendingReturns_) {
            if (!vt || vt->kind == Type::Kind::Unknown ||
                vt->kind == Type::Kind::Err)
                continue; // 递归自引用/错误占位
            if (!any) {
                inferred = vt;
                any = true;
                continue;
            }
            if (typeEquals(inferred, vt)) continue;
            if (TypePtr w = unifyNumeric(inferred, vt)) {
                inferred = w;
                continue;
            }
            conflict = true;
            diag_.error(Errc::E2006_ReturnTypeConflict, file_, f.nameRange,
                        "函数 '" + f.name + "' 的返回类型推断冲突：" +
                            typeToString(inferred) + " 与 " +
                            typeToString(vt));
            inferred = tyErr();
            break;
        }
        if (!conflict) {
            it->second.ret = inferred;
            it->second.retPending = false;
            out.funcs[f.name] = it->second; // 导出同步（.sgi/跨模块消费）
            // 推断为 float 的 int 返回值补记边界提升（Q3 双模一致）。
            if (inferred->kind == Type::Kind::Float) {
                for (const auto& [expr, vt] : pendingReturns_)
                    if (expr && vt->kind == Type::Kind::Int)
                        promoteF64_.insert(expr);
            }
        }
    }
    pendingReturns_.clear();
    scope_.pop();
    inAsyncFn_ = prevAsync;
    inFunc_ = prevInFunc;
    inPureFn_ = prevPure;
}

void TypeChecker::checkContractDefs(const FuncDef& f) {
    for (const auto& c : f.contracts) {
        bool isEnsures = c.kind == ContractClause::Kind::Ensures;
        if (isEnsures) checkingEnsures_ = true;

        TypePtr t = checkExpr(*c.expr);
        if (t->kind != Type::Kind::Bool && t->kind != Type::Kind::Err) {
            diag_.error(Errc::E3002_ContractTypeError, file_, c.range,
                        "契约表达式必须为 bool，实际为 " + typeToString(t));
        }

        // 可静态验证 = 只引用参数/字面量 + 算术/比较/逻辑运算。
        bool verifiable = true;
        std::function<void(const Expr*)> walk = [&](const Expr* e) {
            if (!e) return;
            switch (e->kind) {
            case Expr::Kind::Name: {
                auto& n = static_cast<const Name&>(*e);
                bool isParam = false;
                for (const auto& p : f.params) isParam |= (p.name == n.name);
                if (!isParam) verifiable = false;
                break;
            }
            case Expr::Kind::IntLit:
            case Expr::Kind::FloatLit:
            case Expr::Kind::BoolLit:
            case Expr::Kind::NoneLit:
            case Expr::Kind::StrLit:
                break;
            case Expr::Kind::Unary:
                walk(static_cast<const Unary*>(e)->operand.get());
                break;
            case Expr::Kind::Binary: {
                auto* b = static_cast<const Binary*>(e);
                walk(b->lhs.get());
                walk(b->rhs.get());
                break;
            }
            default:
                verifiable = false;
            }
        };
        walk(c.expr.get());

        if (!verifiable || isEnsures) {
            diag_.warning(
                Errc::W3001_ContractDowngraded, file_, c.range,
                std::string(isEnsures ? "ensures" : "requires") +
                    " 子句无法在调用点静态验证，降级为运行时断言"
                    "（P0 验证核心不做全量符号执行）");
        }
        checkingEnsures_ = false;
    }
}

} // namespace scourge
