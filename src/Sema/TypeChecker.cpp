// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 类型/形状/契约检查器实现
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
// 模块级：收集顶层符号
// ============================================================================

void TypeChecker::collectTopLevel(Module& m, ModuleSymbols& out) {
    for (size_t i = 0; i < m.body.size(); ++i) {
        const Stmt& s = *m.body[i];
        switch (s.kind) {
        case Stmt::Kind::FuncDef: {
            auto& f = static_cast<FuncDef&>(*m.body[i]);

            // 装饰器白名单（设计文档第二章：扩展仅经装饰器引入）。
            static const char* kKnown[] = {"jit", "pure", "hardware", "irq",
                                           "realtime", "no_runtime", "extern"};
            bool pure = false;
            for (const auto& d : f.decorators) {
                bool known = false;
                for (const char* k : kKnown) known |= (d.name == k);
                if (!known) {
                    diag_.error(Errc::E1007_UnsupportedFeature, file_, d.range,
                                "未知装饰器 '@" + d.name +
                                    "'（允许：@jit @pure @hardware @irq "
                                    "@realtime @no_runtime @extern）");
                    continue;
                }
                if (d.name == "pure") pure = true;
                if (d.name == "extern") {
                    if (d.args.size() != 1 ||
                        d.args[0]->kind != Expr::Kind::StrLit) {
                        diag_.error(
                            Errc::E1007_UnsupportedFeature, file_, d.range,
                            "@extern 需要恰好一个字符串参数，如 @extern(\"c\")");
                    }
                }
            }

            if (localFuncs_.count(f.name)) {
                diag_.error(Errc::E2007_DuplicateDefinition, file_, f.nameRange,
                            "函数 '" + f.name + "' 重复定义");
                continue;
            }

            // 内建名保留（Q14 与文档对齐）：模块级 def 与内建同名即拒绝，
            // 杜绝"自定义函数被内建静默顶替"的正确性陷阱。
            if (builtinType(f.name)) {
                diag_.error(Errc::E2007_DuplicateDefinition, file_, f.nameRange,
                            "函数名 '" + f.name +
                                "' 与内建冲突（内建名不可重定义）；请改名");
                continue;
            }

            FuncSig sig;
            sig.name = f.name;
            sig.isPure = pure;
            sig.def = &f;
            sig.requiredArgs = 0;
            bool sawDefault = false;
            for (const auto& p : f.params) {
                if (!p.type) {
                    // 参数注解必填：.sgi 接口先行消费依赖完整签名（第六章第 3 条）。
                    diag_.error(Errc::E2002_CannotInfer, file_, p.range,
                                "参数 '" + p.name +
                                    "' 缺少类型注解；函数签名必须完整");
                    sig.params.push_back(tyErr());
                    continue;
                }
                TypePtr t = resolveTypeExpr(*p.type);
                sig.params.push_back(t);
                // 缺省值边界 int→float 提升（Q3：与函数体注解赋值同规则）。
                if (p.defaultValue) {
                    TypePtr dv = checkExpr(*p.defaultValue);
                    if (dv->kind == Type::Kind::Int &&
                        t->kind == Type::Kind::Float)
                        promoteF64_.insert(p.defaultValue.get());
                    sawDefault = true;
                } else if (sawDefault) {
                    diag_.error(Errc::E2001_TypeMismatch, file_, p.range,
                                "无默认值参数不能跟在有默认值参数之后");
                } else {
                    ++sig.requiredArgs;
                }
            }
            if (!f.retType) {
                // 返回类型推断（Q10，与文档对齐）：由函数体 return 语句统一
                // 推导（checkFunction 完成后回填）；递归自引用以 Unknown 通配。
                sig.ret = tyUnknown();
                sig.retPending = true;
            } else {
                sig.ret = resolveTypeExpr(*f.retType);
            }
            // @extern（0.9.4 H 专项）：签名限定——返回类型必须显式注解，
            // 参数/返回仅标量域（int/float/bool/str，返回可 none），拒绝
            // 缺省参数；函数体由外部动态库提供（双模后端均不发射）。
            bool isExtern = false;
            for (const auto& d : f.decorators) isExtern |= (d.name == "extern");
            if (isExtern) {
                sig.isExtern = true;
                if (!f.retType)
                    diag_.error(Errc::E2002_CannotInfer, file_, f.nameRange,
                                "@extern 函数返回类型必须显式注解");
                auto scalarOk = [](Type::Kind k) {
                    return k == Type::Kind::Int || k == Type::Kind::Float ||
                           k == Type::Kind::Bool || k == Type::Kind::Str;
                };
                for (size_t k = 0; k < sig.params.size(); ++k) {
                    if (!scalarOk(sig.params[k]->kind))
                        diag_.error(
                            Errc::E1007_UnsupportedFeature, file_,
                            f.params[k].range,
                            "@extern 参数类型仅支持 int/float/bool/str");
                }
                if (sig.ret && !scalarOk(sig.ret->kind) &&
                    sig.ret->kind != Type::Kind::None)
                    diag_.error(Errc::E1007_UnsupportedFeature, file_,
                                f.retType->range,
                                "@extern 返回类型仅支持 int/float/bool/str/none");
                if (sig.requiredArgs != sig.params.size())
                    diag_.error(Errc::E1007_UnsupportedFeature, file_,
                                f.nameRange,
                                "@extern 函数参数不支持缺省值");
            }
            for (const auto& c : f.contracts)
                sig.contracts.push_back({c.kind, c.expr.get()});
            localFuncs_[f.name] = std::move(sig);
            out.funcs[f.name] = localFuncs_[f.name]; // 导出：.sgi 与跨模块消费
            break;
        }
        case Stmt::Kind::Import: {
            auto& im = static_cast<const Import&>(s);
            if (imports_.count(im.module)) {
                auto mod = std::make_shared<Type>(Type::Kind::Module);
                mod->name = im.module;
                scope_.declare(im.module, VarSym{mod, s.range});
            }
            // 模块缺失/循环由 driver 层报告（E5001/E5002）。
            break;
        }
        case Stmt::Kind::Assign: {
            auto& a = static_cast<const Assign&>(s);
            if (a.target->kind != Expr::Kind::Name) {
                diag_.error(Errc::E1006_InvalidAssignTarget, file_, s.range,
                            "模块级赋值目标必须是变量名");
                break;
            }
            auto& n = static_cast<const Name&>(*a.target);
            // 内建名保留（Q14）：模块级赋值与内建同名即拒绝，与文档对齐。
            if (builtinType(n.name)) {
                diag_.error(Errc::E2007_DuplicateDefinition, file_, s.range,
                            "变量名 '" + n.name +
                                "' 与内建冲突（内建名不可重定义）；请改名");
                break;
            }
            // Q1：无初值声明拒绝——Scourge 无未初始化变量，编译器对
            // 纯声明语句会解引用空值（段错误根源），在检查期给出明确诊断。
            if (!a.value) {
                diag_.error(Errc::E2002_CannotInfer, file_, s.range,
                            "变量 '" + n.name +
                                "' 声明需要初值（Scourge 无未初始化变量）；"
                                "请写作 " +
                                n.name + ": <类型> = <初值>，或直接 " + n.name +
                                " = <初值>");
                break;
            }
            TypePtr vt = checkExpr(*a.value);
            TypePtr declared;
            if (a.ann) {
                declared = resolveTypeExpr(*a.ann);
                if (vt->kind == Type::Kind::Int &&
                    declared->kind == Type::Kind::Float)
                    promoteF64_.insert(a.value.get()); // Q3 边界提升
                if (!isConvertible(vt, declared)) {
                    fail(Errc::E2001_TypeMismatch, a.value->range,
                         "赋值类型不匹配：" + typeToString(vt) + " 无法赋给 " +
                             typeToString(declared));
                }
            } else {
                if (vt->kind == Type::Kind::Unknown) {
                    declared = fail(Errc::E2002_CannotInfer, s.range,
                                    "无法推断 '" + n.name +
                                        "' 的类型；请添加类型注解（如 " + n.name +
                                        ": int = ...）");
                } else {
                    declared = vt;
                }
            }
            if (!scope_.declare(n.name, VarSym{declared, s.range})) {
                diag_.error(Errc::E2007_DuplicateDefinition, file_, n.range,
                            "顶层名 '" + n.name + "' 重复定义");
            }
            out.globals[n.name] = declared;
            break;
        }
        case Stmt::Kind::ExprStmt: {
            auto& es = static_cast<const ExprStmt&>(s);
            if (i == 0 && es.expr && es.expr->kind == Expr::Kind::StrLit)
                break; // 模块 docstring
            diag_.error(Errc::E1007_UnsupportedFeature, file_, s.range,
                        "模块顶层仅允许 def / import / 赋值（保证 .sgi 可静态"
                        "生成；docstring 除外）");
            break;
        }
        default:
            diag_.error(Errc::E1007_UnsupportedFeature, file_, s.range,
                        "模块顶层仅允许 def / import / 赋值（保证 .sgi 可静态"
                        "生成；docstring 除外）");
        }
    }
}

// ============================================================================
// 模块检查主流程
// ============================================================================

ModuleSymbols TypeChecker::check(Module& m) {
    file_ = m.file;
    ModuleSymbols out;
    out.name = m.name;

    scope_.push(); // 模块作用域
    collectTopLevel(m, out);

    for (auto& stmt : m.body) {
        if (stmt->kind == Stmt::Kind::FuncDef)
            checkFunction(static_cast<FuncDef&>(*stmt), out);
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

// ============================================================================
// 语句检查
// ============================================================================

void TypeChecker::checkStmtList(const StmtList& list) {
    for (const auto& s : list) checkStmt(*s);
}

void TypeChecker::checkStmt(const Stmt& s) {
    switch (s.kind) {
    case Stmt::Kind::Assign: {
        auto& a = static_cast<const Assign&>(s);
        // Q1：无初值声明拒绝（与模块级一致；编译器对纯声明解引用空值）。
        if (!a.value) {
            std::string nm = a.target->kind == Expr::Kind::Name
                                 ? static_cast<const Name&>(*a.target).name
                                 : "<目标>";
            diag_.error(Errc::E2002_CannotInfer, file_, s.range,
                        "变量 '" + nm +
                            "' 声明需要初值（Scourge 无未初始化变量）；"
                            "请写作 " +
                            nm + ": <类型> = <初值>，或直接 " + nm +
                            " = <初值>");
            break;
        }
        TypePtr vt = checkExpr(*a.value);

        if (a.target->kind == Expr::Kind::Name) {
            auto& n = static_cast<const Name&>(*a.target);
            const VarSym* existing = scope_.find(n.name);
            if (a.ann) {
                TypePtr declared = resolveTypeExpr(*a.ann);
                if (vt->kind == Type::Kind::Int &&
                    declared->kind == Type::Kind::Float)
                    promoteF64_.insert(a.value.get()); // Q3 边界提升
                if (!isConvertible(vt, declared)) {
                    fail(Errc::E2001_TypeMismatch, a.value->range,
                         "赋值类型不匹配：" + typeToString(vt) + " 无法赋给 " +
                             typeToString(declared));
                }
                if (existing && !isConvertible(declared, existing->type)) {
                    diag_.error(Errc::E2001_TypeMismatch, file_,
                                a.target->range,
                                "变量 '" + n.name + "' 已声明为 " +
                                    typeToString(existing->type) +
                                    "，与注解 " + typeToString(declared) +
                                    " 不一致");
                }
                if (!existing) scope_.declare(n.name, VarSym{declared, s.range});
                break;
            }
            if (existing) {
                if (vt->kind == Type::Kind::Unknown) {
                    fail(Errc::E2002_CannotInfer, a.value->range,
                         "无法推断 '" + n.name + "' 的新值类型");
                    break;
                }
                if (vt->kind == Type::Kind::Int &&
                    existing->type->kind == Type::Kind::Float)
                    promoteF64_.insert(a.value.get()); // Q3 边界提升（重赋值）
                if (!isConvertible(vt, existing->type)) {
                    fail(Errc::E2001_TypeMismatch, s.range,
                         "变量 '" + n.name + "' 已声明为 " +
                             typeToString(existing->type) + "，不能赋 " +
                             typeToString(vt));
                }
            } else {
                if (vt->kind == Type::Kind::Unknown) {
                    vt = fail(Errc::E2002_CannotInfer, s.range,
                              "无法推断 '" + n.name +
                                  "' 的类型；请添加类型注解（如 " + n.name +
                                  ": int = ...）");
                }
                scope_.declare(n.name, VarSym{vt, s.range});
            }
        } else if (a.target->kind == Expr::Kind::Index && a.value) {
            // 下标赋值：t[i, ...] = v
            auto& ix = static_cast<const Index&>(*a.target);
            TypePtr base = checkExpr(*ix.base);
            for (const auto& idx : ix.indices) {
                TypePtr it = checkExpr(*idx);
                if (it->kind != Type::Kind::Int && it->kind != Type::Kind::Err)
                    fail(Errc::E2001_TypeMismatch, idx->range,
                         "下标必须是 int，实际为 " + typeToString(it));
            }
            if (base->kind == Type::Kind::Tensor) {
                if (ix.indices.size() != base->shape.size()) {
                    fail(Errc::E2004_ShapeMismatch, a.target->range,
                         "下标赋值需提供全部 " +
                             std::to_string(base->shape.size()) +
                             " 维索引（当前 " +
                             std::to_string(ix.indices.size()) + " 个）");
                    break;
                }
                if (!isConvertible(vt, base->params[0])) {
                    fail(Errc::E2001_TypeMismatch, a.value->range,
                         "元素类型不匹配：" + typeToString(vt) + " 无法赋给 " +
                             typeToString(base->params[0]));
                }
            } else if (base->kind == Type::Kind::List) {
                if (ix.indices.size() != 1) {
                    fail(Errc::E2004_ShapeMismatch, a.target->range,
                         "List 下标赋值只支持单索引");
                    break;
                }
                if (!isConvertible(vt, base->params[0])) {
                    fail(Errc::E2001_TypeMismatch, a.value->range,
                         "元素类型不匹配：" + typeToString(vt) + " 无法赋给 " +
                             typeToString(base->params[0]));
                }
            } else if (base->kind == Type::Kind::Dict) {
                if (!ix.indices.empty()) {
                    TypePtr kt = checkExpr(*ix.indices[0]);
                    if (!isConvertible(kt, base->params[0])) {
                        fail(Errc::E2001_TypeMismatch, ix.indices[0]->range,
                             "键类型不匹配：期望 " +
                                 typeToString(base->params[0]));
                    }
                }
                if (!isConvertible(vt, base->params[1])) {
                    fail(Errc::E2001_TypeMismatch, a.value->range,
                         "值类型不匹配：" + typeToString(vt) + " 无法赋给 " +
                             typeToString(base->params[1]));
                }
            } else if (base->kind != Type::Kind::Err) {
                fail(Errc::E2001_TypeMismatch, ix.base->range,
                     "该类型不支持下标赋值：" + typeToString(base));
            }
        }
        break;
    }
    case Stmt::Kind::AugAssign: {
        auto& a = static_cast<const AugAssign&>(s);
        const std::string& name = static_cast<const Name&>(*a.target).name;
        const VarSym* v = scope_.find(name);
        TypePtr vt = checkExpr(*a.value);
        if (!v) {
            fail(Errc::E2003_UnknownName, a.target->range,
                 "未声明的标识符 '" + name + "'");
            break;
        }
        TypePtr lhs = v->type;
        if (lhs->kind == Type::Kind::Tensor && vt->kind == Type::Kind::Tensor) {
            std::vector<int64_t> sh;
            bool ok = unifyShape(lhs->shape, vt->shape, sh) &&
                      typeEquals(lhs->params[0], vt->params[0]);
            if (!ok) {
                fail(Errc::E2004_ShapeMismatch, s.range,
                     "张量形状不兼容：" + typeToString(lhs) + " 与 " +
                         typeToString(vt));
            }
        } else if (lhs->kind == Type::Kind::Str && a.op == "+=" &&
                   vt->kind == Type::Kind::Str) {
            // ok
        } else if (lhs->kind != Type::Kind::Err && vt->kind != Type::Kind::Err) {
            if (!unifyNumeric(lhs, vt)) {
                fail(Errc::E2001_TypeMismatch, s.range,
                     "运算符 '" + a.op + "' 不支持 " + typeToString(lhs) +
                         " 与 " + typeToString(vt));
            }
        }
        break;
    }
    case Stmt::Kind::ExprStmt: {
        auto& es = static_cast<const ExprStmt&>(s);
        TypePtr t = checkExpr(*es.expr);
        // 错误即值：Result 结果被丢弃即编译错误（设计文档第四章）。
        if (t->kind == Type::Kind::Result) {
            diag_.error(Errc::E2005_ResultNotConsumed, file_, es.expr->range,
                        "Result 值被丢弃；必须显式消费：绑定变量、用 is_ok() "
                        "判断，或 unwrap()/unwrap_or() 取值");
        }
        break;
    }
    case Stmt::Kind::If: {
        auto& st = static_cast<const If&>(s);
        requireBool(checkExpr(*st.cond), st.cond->range, "if 条件");
        checkStmtList(st.thenBody);
        for (const auto& e : st.elifs) {
            requireBool(checkExpr(*e.cond), e.cond->range, "elif 条件");
            checkStmtList(e.body);
        }
        checkStmtList(st.elseBody);
        break;
    }
    case Stmt::Kind::While: {
        auto& st = static_cast<const While&>(s);
        requireBool(checkExpr(*st.cond), st.cond->range, "while 条件");
        ++loopDepth_;
        checkStmtList(st.body);
        --loopDepth_;
        break;
    }
    case Stmt::Kind::For: {
        auto& st = static_cast<const For&>(s);
        TypePtr it = checkExpr(*st.iter);
        TypePtr elem = tyErr();
        if (it->kind == Type::Kind::Range) elem = tyInt();
        else if (it->kind == Type::Kind::List) elem = it->params[0];
        else if (it->kind != Type::Kind::Err)
            fail(Errc::E2001_TypeMismatch, st.iter->range,
                 "for 只能遍历 range 或 List，实际为 " + typeToString(it));
        // 循环变量声明（Q9，Python 式复用）：与函数层已有同名绑定且类型
        // 相同 → 复用绑定（重绑定语义）；类型不同 → 明确报错。与外层
        // （模块全局/导入名）同名属正常遮蔽，照常声明。
        if (const VarSym* ex = scope_.findInBack(st.var)) {
            if (typeEquals(ex->type, elem)) {
                // 复用：沿用既有槽位与类型。
            } else {
                diag_.error(Errc::E2007_DuplicateDefinition, file_, st.varRange,
                            "循环变量 '" + st.var + "' 与本函数内已声明的 " +
                                typeToString(ex->type) + " 型变量冲突（类型 " +
                                typeToString(elem) + "）；请换用其他名称");
            }
        } else {
            scope_.declare(st.var, VarSym{elem, st.varRange});
        }
        ++loopDepth_;
        checkStmtList(st.body);
        --loopDepth_;
        break;
    }
    case Stmt::Kind::Return: {
        auto& st = static_cast<const Return&>(s);
        TypePtr vt = st.value ? checkExpr(*st.value) : tyNone();
        if (fnRet_->kind == Type::Kind::Unknown) {
            // 返回类型推断中（Q10）：收集待统一；int→float 提升待
            // 统一结果为 float 时补记（见 checkFunction 尾部）。
            pendingReturns_.push_back({st.value.get(), vt});
        } else if (fnRet_->kind != Type::Kind::Err) {
            if (vt->kind == Type::Kind::Int &&
                fnRet_->kind == Type::Kind::Float && st.value)
                promoteF64_.insert(st.value.get()); // Q3 边界提升（返回）
            if (!isConvertible(vt, fnRet_)) {
                fail(Errc::E2006_ReturnTypeConflict, s.range,
                     "返回类型不匹配：函数声明返回 " + typeToString(fnRet_) +
                         "，实际返回 " + typeToString(vt));
            }
        }
        break;
    }
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
        if (loopDepth_ == 0) {
            diag_.error(Errc::E2009_BreakOutsideLoop, file_, s.range,
                        s.kind == Stmt::Kind::Break ? "break 不在循环内"
                                                    : "continue 不在循环内");
        }
        break;
    case Stmt::Kind::Pass:
        break;
    case Stmt::Kind::Try: {
        // panic 恢复路径：两块分别全量检查；类型无约束（文档第四章）。
        auto& st = static_cast<const Try&>(s);
        checkStmtList(st.body);
        checkStmtList(st.handlerBody);
        break;
    }
    case Stmt::Kind::Assert: {
        auto& st = static_cast<const Assert&>(s);
        requireBool(checkExpr(*st.cond), st.cond->range, "assert 条件");
        if (st.msg) {
            TypePtr mt = checkExpr(*st.msg);
            if (mt->kind != Type::Kind::Str && mt->kind != Type::Kind::Err)
                fail(Errc::E2001_TypeMismatch, st.msg->range,
                     "assert 消息必须是 str，实际为 " + typeToString(mt));
        }
        break;
    }
    case Stmt::Kind::Import:
    case Stmt::Kind::FuncDef:
        break; // 顶层已处理
    }
}

void TypeChecker::requireBool(const TypePtr& t, const SourceRange& r,
                              std::string_view what) {
    if (t->kind != Type::Kind::Bool && t->kind != Type::Kind::Err) {
        diag_.error(Errc::E2001_TypeMismatch, file_, r,
                    std::string(what) +
                        " 必须为 bool（无隐式真值转换），实际为 " +
                        typeToString(t));
    }
}

// ============================================================================
// 表达式检查
// ============================================================================

TypePtr TypeChecker::checkExpr(const Expr& e) {
    TypePtr t = checkExprImpl(e);
    // 单一事实源：推断结果按节点缓存（Err/Unknown 为已报错/推断失败占位，
    // 不入表——后端查不到时走自己的保守回退，行为与未接入前一致）。
    if (t && t->kind != Type::Kind::Err && t->kind != Type::Kind::Unknown)
        exprTypes_[&e] = t;
    return t;
}

TypePtr TypeChecker::checkExprImpl(const Expr& e) {
    switch (e.kind) {
    case Expr::Kind::IntLit: return tyInt();
    case Expr::Kind::FloatLit: return tyFloat();
    case Expr::Kind::BoolLit: return tyBool();
    case Expr::Kind::NoneLit: return tyNone();
    case Expr::Kind::StrLit: return tyStr();
    case Expr::Kind::Name: return checkName(e);
    case Expr::Kind::Unary: {
        auto& u = static_cast<const Unary&>(e);
        TypePtr t = checkExpr(*u.operand);
        if (u.op == "not") {
            if (t->kind != Type::Kind::Bool && t->kind != Type::Kind::Err)
                return fail(Errc::E2001_TypeMismatch, e.range,
                            "not 要求 bool，实际为 " + typeToString(t));
            return tyBool();
        }
        if (t->kind == Type::Kind::Int || t->kind == Type::Kind::Float ||
            t->kind == Type::Kind::Tensor || t->kind == Type::Kind::Err)
            return t;
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "一元运算符 '" + u.op + "' 不支持 " + typeToString(t));
    }
    case Expr::Kind::Binary:
        return checkBinary(e);
    case Expr::Kind::Ternary: {
        auto& t3 = static_cast<const Ternary&>(e);
        requireBool(checkExpr(*t3.cond), t3.cond->range, "三元条件");
        TypePtr a = checkExpr(*t3.thenE);
        TypePtr b = checkExpr(*t3.elseE);
        if (typeEquals(a, b)) return a;
        if (TypePtr u = unifyNumeric(a, b)) return u;
        if (a->kind == Type::Kind::Tensor && b->kind == Type::Kind::Tensor &&
            typeEquals(a->params[0], b->params[0])) {
            std::vector<int64_t> sh;
            if (unifyShape(a->shape, b->shape, sh))
                return makeTensor(a->params[0], sh);
        }
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "三元分支类型不一致：" + typeToString(a) + " 与 " +
                        typeToString(b));
    }
    case Expr::Kind::Call:
        return checkCall(e);
    case Expr::Kind::Attr:
        return checkAttr(e);
    case Expr::Kind::Index:
        return checkIndex(e);
    case Expr::Kind::ListLit:
        return checkListLit(e);
    case Expr::Kind::Await: {
        // await 仅在 async def 内合法（E2010）；类型透传操作数
        // （Task 语义随 P2 并发落地，P0 阶段 await 是类型恒等）。
        auto& aw = static_cast<const Await&>(e);
        if (!inAsyncFn_) {
            return fail(Errc::E2010_AwaitOutsideAsync, e.range,
                        "await 只能出现在 async def 函数体内");
        }
        return checkExpr(*aw.operand);
    }
    case Expr::Kind::TupleLit:
        return fail(Errc::E1007_UnsupportedFeature, e.range,
                    "元组仅在张量形状注解 Tensor[T, (…)] 中使用");
    }
    return tyErr();
}

TypePtr TypeChecker::checkName(const Expr& e) {
    auto& n = static_cast<const Name&>(e);
    if (checkingEnsures_ && n.name == "result") return fnRet_;

    if (const VarSym* v = scope_.find(n.name)) return v->type;
    if (localFuncs_.count(n.name))
        return fail(Errc::E1007_UnsupportedFeature, e.range,
                    "函数 '" + n.name + "' 不能作为值使用（当前仅支持调用）");

    if (TypePtr b = builtinType(n.name)) return b;

    std::string msg = "未声明的标识符 '" + n.name + "'";
    if (n.name == "result") msg += "（result 仅在 ensures 契约中可用）";
    fail(Errc::E2003_UnknownName, e.range, msg);
    noteSimilarName(n.name);
    return tyErr();
}

TypePtr TypeChecker::builtinType(const std::string& name) const {
    // ok/err：错误即值的构造出口（P0 内建，标准库 v1 冻结后归入 result 模块）。
    if (name == "print" || name == "str" || name == "len" || name == "sum" ||
        name == "now" || name == "range" || name == "ok" || name == "err" ||
        name == "abs" || name == "sqrt" || name == "exp" || name == "log" ||
        name == "max" || name == "min" || name == "transpose" ||
        name == "reshape" ||
        // v0.9.3 标量数学/随机/排序（与 Compiler/MLIRGen builtinNames 同表）
        name == "sin" || name == "cos" || name == "tan" || name == "asin" ||
        name == "acos" || name == "atan" || name == "sinh" || name == "cosh" ||
        name == "tanh" || name == "atan2" || name == "floor" ||
        name == "ceil" || name == "round" || name == "trunc" ||
        name == "random" || name == "seed" || name == "sorted") {
        auto t = std::make_shared<Type>(Type::Kind::Builtin);
        t->name = name;
        return t;
    }
    return nullptr;
}

TypePtr TypeChecker::checkBinary(const Expr& e) {
    auto& b = static_cast<const Binary&>(e);
    const std::string& op = b.op;
    TypePtr l = checkExpr(*b.lhs);
    TypePtr r = checkExpr(*b.rhs);
    if (l->kind == Type::Kind::Err || r->kind == Type::Kind::Err) return tyErr();

    auto failOp = [&](std::string extra) -> TypePtr {
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "运算符 '" + op + "' 不支持 " + typeToString(l) + " 与 " +
                        typeToString(r) + extra);
    };

    // 逻辑
    if (op == "and" || op == "or") {
        if (l->kind == Type::Kind::Bool && r->kind == Type::Kind::Bool)
            return tyBool();
        return failOp("（逻辑运算要求两侧均为 bool）");
    }

    // 成员测试
    if (op == "in") {
        if (r->kind == Type::Kind::List && isConvertible(l, r->params[0]))
            return tyBool();
        if (r->kind == Type::Kind::Str && l->kind == Type::Kind::Str)
            return tyBool();
        if (r->kind == Type::Kind::Dict && isConvertible(l, r->params[0]))
            return tyBool();
        return failOp("");
    }

    // 矩阵乘（@）
    if (op == "@") {
        if (l->kind == Type::Kind::Tensor && r->kind == Type::Kind::Tensor) {
            if (l->shape.size() != 2 || r->shape.size() != 2) {
                return fail(Errc::E2004_ShapeMismatch, e.range,
                            "matmul（@）要求两个二维张量，实际 " +
                                typeToString(l) + " 与 " + typeToString(r));
            }
            if (l->shape[1] != kDyn && r->shape[0] != kDyn &&
                l->shape[1] != r->shape[0]) {
                return fail(Errc::E2004_ShapeMismatch, e.range,
                            "matmul 维度不匹配：" + typeToString(l) + " 与 " +
                                typeToString(r) + "（左列数 != 右行数）");
            }
            TypePtr elem = unifyNumeric(l->params[0], r->params[0]);
            if (!elem) elem = l->params[0];
            return makeTensor(elem, {l->shape[0], r->shape[1]});
        }
        return failOp("");
    }

    // 比较
    if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
        op == ">=") {
        if (l->kind == Type::Kind::Tensor || r->kind == Type::Kind::Tensor) {
            return failOp("（张量不支持整体比较；请用逐元素运算 + 归约显式表达）");
        }
        if (unifyNumeric(l, r)) return tyBool();
        if (l->kind == r->kind &&
            (l->kind == Type::Kind::Str || l->kind == Type::Kind::Bool))
            return tyBool();
        if ((op == "==" || op == "!=") &&
            (l->kind == Type::Kind::None || r->kind == Type::Kind::None))
            return tyBool();
        return failOp("");
    }

    // 算术
    if (op == "+" || op == "-" || op == "*" || op == "//" || op == "%" ||
        op == "**" || op == "/") {
        // 张量逐元素（含标量广播）
        if (l->kind == Type::Kind::Tensor || r->kind == Type::Kind::Tensor) {
            TypePtr lt = l->kind == Type::Kind::Tensor ? l->params[0] : l;
            TypePtr rt = r->kind == Type::Kind::Tensor ? r->params[0] : r;
            TypePtr elem = unifyNumeric(lt, rt);
            if (!elem && typeEquals(lt, rt)) elem = lt;
            if (!elem) return failOp("（元素类型不一致）");
            // 真除法恒为 float（与标量 int/int → float 语义一致）。
            if (op == "/") elem = tyFloat();
            if (l->kind == Type::Kind::Tensor && r->kind == Type::Kind::Tensor) {
                std::vector<int64_t> sh;
                if (!unifyShape(l->shape, r->shape, sh)) {
                    return fail(Errc::E2004_ShapeMismatch, e.range,
                                "张量形状不兼容：" + typeToString(l) + " 与 " +
                                    typeToString(r));
                }
                return makeTensor(elem, sh);
            }
            const TypePtr& t = l->kind == Type::Kind::Tensor ? l : r;
            return makeTensor(elem, t->shape);
        }
        // 字符串
        if (l->kind == Type::Kind::Str || r->kind == Type::Kind::Str) {
            if (op == "+" && l->kind == Type::Kind::Str &&
                r->kind == Type::Kind::Str)
                return tyStr();
            if (op == "*" &&
                ((l->kind == Type::Kind::Str && r->kind == Type::Kind::Int) ||
                 (l->kind == Type::Kind::Int && r->kind == Type::Kind::Str)))
                return tyStr();
            return failOp("");
        }
        // 数值
        if (TypePtr u = unifyNumeric(l, r)) {
            if (op == "/") return tyFloat(); // 真除法恒为 float
            if (op == "//" || op == "%")
                return u->kind == Type::Kind::Int ? tyInt() : tyFloat();
            return u; // + - * **
        }
        return failOp("");
    }

    return failOp("");
}

TypePtr TypeChecker::checkListLit(const Expr& e) {
    auto& ll = static_cast<const ListLit&>(e);
    if (ll.elems.empty()) return tyUnknown(); // 赋值上下文报 E2002

    // 递归推断：数值/张量嵌套 → 张量字面量；其余 → List[T]。
    std::vector<TypePtr> ts;
    for (const auto& el : ll.elems) ts.push_back(checkExpr(*el));

    bool allNumeric = true, allTensor = true;
    std::vector<int64_t> rowShape;
    for (size_t i = 0; i < ts.size(); ++i) {
        TypePtr t = ts[i];
        if (t->kind == Type::Kind::Err) return tyErr();
        allNumeric &=
            (t->kind == Type::Kind::Int || t->kind == Type::Kind::Float);
        if (t->kind == Type::Kind::Tensor) {
            if (i == 0) rowShape = t->shape;
            else if (rowShape != t->shape) {
                return fail(Errc::E2004_ShapeMismatch, e.range,
                            "张量字面量各行形状不一致：第 1 行与第 " +
                                std::to_string(i + 1) + " 行");
            }
        } else {
            allTensor = false;
        }
    }

    if (allNumeric) {
        TypePtr elem = tyInt();
        for (auto& t : ts)
            if (t->kind == Type::Kind::Float) elem = tyFloat();
        ll.checkedAsTensor = true; // 编译器编为 NewTensor（运行时构造）
        return makeTensor(elem, {static_cast<int64_t>(ts.size())});
    }
    if (allTensor) {
        TypePtr elem = ts[0]->params[0];
        for (auto& t : ts) {
            if (!typeEquals(elem, t->params[0])) {
                if (TypePtr u = unifyNumeric(elem, t->params[0])) elem = u;
            }
        }
        std::vector<int64_t> sh;
        sh.push_back(static_cast<int64_t>(ts.size()));
        for (int64_t d : rowShape) sh.push_back(d);
        ll.checkedAsTensor = true; // 编译器编为 NewTensor（运行时构造）
        return makeTensor(elem, sh);
    }

    // List[T]：元素类型统一
    TypePtr elem = ts[0];
    for (size_t i = 1; i < ts.size(); ++i) {
        if (typeEquals(elem, ts[i])) continue;
        if (TypePtr u = unifyNumeric(elem, ts[i])) {
            elem = u;
            continue;
        }
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "列表元素类型不一致：" + typeToString(elem) + " 与 " +
                        typeToString(ts[i]));
    }
    return makeType(Type::Kind::List, {elem});
}

TypePtr TypeChecker::checkIndex(const Expr& e) {
    auto& ix = static_cast<const Index&>(e);
    TypePtr base = checkExpr(*ix.base);
    if (base->kind == Type::Kind::Err) return tyErr();
    for (const auto& idx : ix.indices) {
        TypePtr it = checkExpr(*idx);
        if (it->kind != Type::Kind::Int && it->kind != Type::Kind::Err)
            fail(Errc::E2001_TypeMismatch, idx->range,
                 "下标必须是 int，实际为 " + typeToString(it));
    }

    if (base->kind == Type::Kind::Tensor) {
        size_t n = ix.indices.size();
        if (n > base->shape.size()) {
            return fail(Errc::E2004_ShapeMismatch, e.range,
                        "下标数量 " + std::to_string(n) + " 超过张量秩 " +
                            std::to_string(base->shape.size()));
        }
        std::vector<int64_t> rest(base->shape.begin() +
                                      static_cast<std::ptrdiff_t>(n),
                                  base->shape.end());
        if (rest.empty()) return base->params[0]; // 全索引 → 标量元素
        return makeTensor(base->params[0], rest);
    }
    if (base->kind == Type::Kind::List) {
        if (ix.indices.size() != 1) {
            return fail(Errc::E2004_ShapeMismatch, e.range,
                        "List 只支持单下标");
        }
        return base->params[0];
    }
    if (base->kind == Type::Kind::Str) return tyStr();
    if (base->kind == Type::Kind::Dict) return base->params[1];
    return fail(Errc::E2001_TypeMismatch, ix.base->range,
                "该类型不支持下标访问：" + typeToString(base));
}

TypePtr TypeChecker::checkAttr(const Expr& e) {
    auto& at = static_cast<const Attr&>(e);
    TypePtr base = checkExpr(*at.base);
    if (base->kind == Type::Kind::Err) return tyErr();

    // Result 消费方法（错误即值的显式出口）。
    if (base->kind == Type::Kind::Result) {
        if (at.name == "is_ok") return makeFunction({}, tyBool());
        if (at.name == "unwrap" || at.name == "unwrap_or")
            return makeFunction({base->params[0]}, base->params[0]);
        return fail(Errc::E2003_UnknownName, e.range,
                    "Result 只有 is_ok / unwrap / unwrap_or 三个成员，无 '" +
                        at.name + "'");
    }

    // 模块成员访问：foo.func / foo.CONST
    if (base->kind == Type::Kind::Module) {
        auto it = imports_.find(base->name);
        if (it != imports_.end()) {
            const ModuleSymbols& ms = *it->second;
            if (auto f = ms.funcs.find(at.name); f != ms.funcs.end()) {
                std::vector<TypePtr> ps = f->second.params;
                ps.push_back(f->second.ret);
                return std::make_shared<Type>(Type::Kind::Function, ps);
            }
            if (auto g = ms.globals.find(at.name); g != ms.globals.end())
                return g->second;
        }
        return fail(Errc::E2003_UnknownName, e.range,
                    "模块 '" + base->name + "' 没有成员 '" + at.name + "'");
    }

    return fail(Errc::E2003_UnknownName, e.range,
                "类型 " + typeToString(base) + " 没有成员 '" + at.name + "'");
}

TypePtr TypeChecker::checkCall(const Expr& e) {
    auto& c = static_cast<const Call&>(e);
    TypePtr callee;

    if (c.callee->kind == Expr::Kind::Name) {
        auto& n = static_cast<const Name&>(*c.callee);
        // 变量遮蔽优先；随后内建；再后本模块函数。
        // （函数名单独出现才是头等值使用，由 checkName 报 E1007。）
        if (!scope_.find(n.name)) {
            TypePtr b = builtinType(n.name);
            if (b && b->kind == Type::Kind::Builtin)
                return checkBuiltinCall(n.name, c);
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
        if (at.base->kind == Expr::Kind::Name) {
            auto& mn = static_cast<const Name&>(*at.base);
            if (auto it = imports_.find(mn.name); it != imports_.end()) {
                if (auto f = it->second->funcs.find(at.name);
                    f != it->second->funcs.end())
                    calleeSig = &f->second;
            }
        }
    }
    if (calleeSig) required = calleeSig->requiredArgs;
    if (calleeSig && calleeSig->retPending) {
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
    if (c.args.size() < required || c.args.size() > nParams) {
        std::string hint = required == nParams
                               ? std::to_string(nParams)
                               : std::to_string(required) + "–" +
                                     std::to_string(nParams) + "（尾部参数有默认值）";
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "实参数量不匹配：期望 " + hint + "，实际 " +
                        std::to_string(c.args.size()));
    }
    // 实参类型检查只遍历调用点提供的实参（Q2：缺失尾部实参由默认值补位）。
    for (size_t i = 0; i < c.args.size(); ++i) {
        TypePtr at = checkExpr(*c.args[i]);
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

    // 本模块/导入函数的 requires 静态验证
    if (c.callee->kind == Expr::Kind::Name) {
        auto& n = static_cast<const Name&>(*c.callee);
        if (auto f = localFuncs_.find(n.name); f != localFuncs_.end())
            verifyRequiresAtCall(f->second, e);
    } else if (c.callee->kind == Expr::Kind::Attr) {
        auto& at = static_cast<const Attr&>(*c.callee);
        if (at.base->kind == Expr::Kind::Name) {
            auto& mn = static_cast<const Name&>(*at.base);
            if (auto it = imports_.find(mn.name); it != imports_.end()) {
                if (auto f = it->second->funcs.find(at.name);
                    f != it->second->funcs.end())
                    verifyRequiresAtCall(f->second, e);
            }
        }
    }

    return callee->params.back();
}

TypePtr TypeChecker::checkBuiltinCall(const std::string& bn, const Call& c) {
    const Expr& e = c; // 诊断区间用整个调用表达式
    if (bn == "print") {
        for (const auto& a : c.args) checkExpr(*a); // 张量执行已进入字节码（P1）
        return tyNone();
    }
    if (bn == "str") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "str() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Int || t->kind == Type::Kind::Float ||
            t->kind == Type::Kind::Bool || t->kind == Type::Kind::Str ||
            t->kind == Type::Kind::Tensor || t->kind == Type::Kind::Err)
            return tyStr();
        fail(Errc::E2001_TypeMismatch, c.args[0]->range,
             "str() 不支持 " + typeToString(t));
        return tyErr();
    }
    if (bn == "len") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "len() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::List || t->kind == Type::Kind::Dict ||
            t->kind == Type::Kind::Str || t->kind == Type::Kind::Err)
            return tyInt();
        fail(Errc::E2001_TypeMismatch, c.args[0]->range,
             "len() 不支持 " + typeToString(t));
        return tyErr();
    }
    if (bn == "sum") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "sum() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Tensor)
            return t->params[0]; // 归约结果 = 元素类型
        if (t->kind == Type::Kind::Err) return tyErr();
        fail(Errc::E2001_TypeMismatch, c.args[0]->range,
             "sum() 仅支持张量归约，不支持 " + typeToString(t));
        return tyErr();
    }
    if (bn == "now") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "now() 不需要参数");
        return tyFloat();
    }
    if (bn == "ok") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "ok() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        return makeType(Type::Kind::Result, {t, tyStr()});
    }
    if (bn == "err") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "err() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err) {
            fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                 "err() 参数必须是 str，实际为 " + typeToString(t));
            return tyErr();
        }
        // OK 类型为 Unknown 通配，由赋值目标注解特化。
        return makeType(Type::Kind::Result, {tyUnknown(), tyStr()});
    }
    // abs/sqrt/exp/log：逐元素数学函数（标量或数值张量）。
    if (bn == "abs" || bn == "sqrt" || bn == "exp" || bn == "log") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        const bool keepInt = (bn == "abs"); // abs 保 dtype，sqrt/exp/log 恒 float
        if (t->kind == Type::Kind::Tensor) {
            TypePtr elem = t->params[0];
            if (elem->kind != Type::Kind::Int &&
                elem->kind != Type::Kind::Float)
                return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                            bn + "() 仅支持数值张量，实际为 " +
                                typeToString(t));
            return makeTensor(keepInt ? elem : tyFloat(), t->shape);
        }
        if (t->kind == Type::Kind::Int || t->kind == Type::Kind::Float)
            return keepInt ? t : tyFloat();
        return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                    bn + "() 仅支持数值或数值张量，实际为 " + typeToString(t));
    }
    // ---- v0.9.3 标量数学：三角/双曲（int 提升 f64，恒返回 float；依 IEEE 754）。
    // 仅标量——张量版本待覆盖集扩展后统一提供（显式拒绝，拒绝即文档）。
    if (bn == "sin" || bn == "cos" || bn == "tan" || bn == "asin" ||
        bn == "acos" || bn == "atan" || bn == "sinh" || bn == "cosh" ||
        bn == "tanh") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Float)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 仅支持标量数值（张量版本暂未提供），实际为 " +
                            typeToString(t));
        return tyFloat();
    }
    if (bn == "atan2") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "atan2() 恰好需要 2 个参数");
        for (size_t k = 0; k < 2; ++k) {
            TypePtr t = checkExpr(*c.args[k]);
            if (t->kind == Type::Kind::Err) return tyErr();
            if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Float)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            "atan2() 仅支持标量数值，实际为 " +
                                typeToString(t));
        }
        return tyFloat();
    }
    // 取整族：int 恒等返回；float → int（NaN/±inf/结果超 int64 范围运行时
    // panic——显式拒绝而非静默回绕）。round = 半到偶（与 Python 3 一致）。
    if (bn == "floor" || bn == "ceil" || bn == "round" || bn == "trunc") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind == Type::Kind::Int || t->kind == Type::Kind::Float)
            return tyInt();
        return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                    bn + "() 仅支持标量数值（张量版本暂未提供），实际为 " +
                        typeToString(t));
    }
    // random/seed：确定性伪随机（splitmix64；双模实现逐位一致，默认状态 0）。
    if (bn == "random") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "random() 不需要参数");
        return tyFloat();
    }
    if (bn == "seed") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "seed() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "seed() 参数必须是 int，实际为 " + typeToString(t));
        return tyNone();
    }
    // sorted(List[T]) → List[T]：稳定升序，返回新列表（列表无变更操作，
    // 就地排序不可表达——One Obvious Way）。
    if (bn == "sorted") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "sorted() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind != Type::Kind::List)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "sorted() 仅支持列表，实际为 " + typeToString(t));
        return t;
    }
    // max/min：张量归约 → 元素类型标量。
    if (bn == "max" || bn == "min") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Tensor) {
            TypePtr elem = t->params[0];
            if (elem->kind != Type::Kind::Int &&
                elem->kind != Type::Kind::Float)
                return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                            bn + "() 仅支持数值张量，实际为 " +
                                typeToString(t));
            return elem;
        }
        if (t->kind == Type::Kind::Err) return tyErr();
        return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                    bn + "() 仅支持张量归约，不支持 " + typeToString(t));
    }
    // transpose：二维转置 → (m, n) 变 (n, m)。
    if (bn == "transpose") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "transpose() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind != Type::Kind::Tensor || t->shape.size() != 2)
            return fail(Errc::E2004_ShapeMismatch, c.args[0]->range,
                        "transpose() 要求二维张量，实际为 " + typeToString(t));
        return makeTensor(t->params[0], {t->shape[1], t->shape[0]});
    }
    // reshape：目标形状由正 int 字面量给出（静态可校验）。
    if (bn == "reshape") {
        if (c.args.size() < 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "reshape() 需要 1 个张量 + 至少 1 个维度");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind != Type::Kind::Tensor)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "reshape() 首参必须是张量，实际为 " + typeToString(t));
        std::vector<int64_t> ns;
        int64_t prod = 1;
        for (size_t k = 1; k < c.args.size(); ++k) {
            const auto* il = dynamic_cast<const IntLit*>(c.args[k].get());
            if (!il || il->value <= 0)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            "reshape() 维度必须是正的 int 字面量");
            ns.push_back(il->value);
            prod *= il->value;
        }
        int64_t src = 1;
        bool srcStatic = true;
        for (int64_t d : t->shape) {
            if (d == kDyn) {
                srcStatic = false;
                break;
            }
            src *= d;
        }
        if (srcStatic && src != prod)
            return fail(Errc::E2004_ShapeMismatch, e.range,
                        "reshape() 元素总数不一致：" + typeToString(t) + " 共 " +
                            std::to_string(src) + " 个，目标形状共 " +
                            std::to_string(prod) + " 个");
        return makeTensor(t->params[0], ns);
    }
    // range
    if (c.args.size() != 1)
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "range() 恰好需要 1 个参数");
    TypePtr t = checkExpr(*c.args[0]);
    if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
        fail(Errc::E2001_TypeMismatch, c.args[0]->range,
             "range() 参数必须是 int，实际为 " + typeToString(t));
    return makeType(Type::Kind::Range);
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
        return makeType(Type::Kind::Option, {resolveTypeExpr(*te.params[0])});
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

} // namespace scourge
