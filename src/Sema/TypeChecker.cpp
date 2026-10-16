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
            static const char* kKnown[] = {"jit",  "pure",    "hardware", "irq",
                                           "realtime", "no_runtime", "extern",
                                           "private", "internal"};
            bool pure = false;
            bool sawPriv = false, sawInter = false;
            for (const auto& d : f.decorators) {
                bool known = false;
                for (const char* k : kKnown) known |= (d.name == k);
                if (!known) {
                    diag_.error(Errc::E1007_UnsupportedFeature, file_, d.range,
                                "未知装饰器 '@" + d.name +
                                    "'（允许：@jit @pure @hardware @irq "
                                    "@realtime @no_runtime @extern @private "
                                    "@internal）");
                    continue;
                }
                if (d.name == "pure") pure = true;
                // 可见性双标冲突即拒绝（0.9.6 L 节）。
                if (d.name == "private") {
                    if (sawPriv || sawInter)
                        diag_.error(Errc::E1007_UnsupportedFeature, file_,
                                    d.range,
                                    "可见性装饰器重复：@private 与 @internal "
                                    "至多标注其一");
                    sawPriv = true;
                }
                if (d.name == "internal") {
                    if (sawPriv || sawInter)
                        diag_.error(Errc::E1007_UnsupportedFeature, file_,
                                    d.range,
                                    "可见性装饰器重复：@private 与 @internal "
                                    "至多标注其一");
                    sawInter = true;
                }
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
            // 可见性（0.9.6 L 节）：@private 仅本模块；@internal 仅本包。
            sig.vis = sawPriv   ? Vis::Private
                      : sawInter ? Vis::Internal
                                 : Vis::Public;
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
                    // Dict/Set 上下文偏好：缺省值容器字面量对齐参数注解。
                    if (p.defaultValue->kind == Expr::Kind::DictLit &&
                        t->kind == Type::Kind::Dict && t->params.size() >= 2)
                        dictTarget_ = t.get();
                    if (p.defaultValue->kind == Expr::Kind::DictLit &&
                        t->kind == Type::Kind::Set && t->params.size() >= 1)
                        setTarget_ = t.get();
                    if (p.defaultValue->kind == Expr::Kind::SetLit &&
                        t->kind == Type::Kind::Set && t->params.size() >= 1)
                        setTarget_ = t.get();
                    TypePtr dv = checkExpr(*p.defaultValue);
                    dictTarget_ = nullptr;
                    setTarget_ = nullptr;
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
                sig.isVariadic = f.isVariadic;
                // FFI 门禁记账（0.9.7）：@extern 即外部代码边界，恒需 ffi。
                capUses_.push_back({f.name, Cap::Ffi, f.nameRange});
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
                // 变参（O 节 FFI 宽类型域）：... 形态已由解析器收口
                //（末位、非首参），此处仅裁决 @extern 资格。
            } else if (f.isVariadic) {
                diag_.error(Errc::E1007_UnsupportedFeature, file_, f.nameRange,
                            "变参标记 ... 仅允许用于 @extern 函数声明"
                            "（C ABI 变参调用，本语言函数定参）");
            }
            for (const auto& c : f.contracts)
                sig.contracts.push_back({c.kind, c.expr.get()});
            localFuncs_[f.name] = std::move(sig);
            out.funcs[f.name] = localFuncs_[f.name]; // 导出：.sgi 与跨模块消费
            break;
        }
        case Stmt::Kind::Import: {
            auto& im = static_cast<const Import&>(s);
            const std::string full = im.module();
            // 绑定名 → Module 类型；Type::Module::name 存真实全限定名，
            // 成员访问经 checkAttr 按其查 registry——别名由此天然解耦。
            auto declareModule = [&](const std::string& bind,
                                     const std::string& qual) {
                auto mod = std::make_shared<Type>(Type::Kind::Module);
                mod->name = qual;
                if (!scope_.declare(bind, VarSym{mod, s.range}))
                    diag_.error(Errc::E2007_DuplicateDefinition, file_,
                                s.range,
                                "顶层名 '" + bind +
                                    "' 重复定义（import 绑定与既有名冲突；"
                                    "可用 as 别名或全限定形式）");
            };
            if (!im.isFrom) {
                if (imports_.count(full)) {
                    if (im.alias.empty()) {
                        // import a.b：绑定首段（Python 式 a.b.f()）+ 宽容绑定
                        // 末段（AI 幻觉写法 b.f() 亦可编译）；单段即现状。
                        declareModule(im.path[0], full);
                        if (im.path.size() > 1)
                            declareModule(im.path.back(), full);
                    } else {
                        // import a.b as c：仅别名绑定（Python 语义）。
                        declareModule(im.alias, full);
                    }
                }
            } else {
                // from <mod> import x [as y]：按 函数 → 全局 → 子模块 序解析。
                auto rit = imports_.find(full);
                const ModuleSymbols* ms =
                    rit == imports_.end() ? nullptr : rit->second.get();
                for (const auto& item : im.items) {
                    const std::string& bind =
                        item.alias.empty() ? item.name : item.alias;
                    if (ms) {
                        if (auto f = ms->funcs.find(item.name);
                            f != ms->funcs.end()) {
                            if (!checkVis(f->second.vis, ms->name, item.name,
                                          s.range))
                                continue;
                            // 先查重再绑定：from 名与本地 def/既有导入冲突
                            // 即 E2007（冲突时不污染 localFuncs_）。
                            if (scope_.find(bind)) {
                                diag_.error(Errc::E2007_DuplicateDefinition,
                                            file_, s.range,
                                            "顶层名 '" + bind +
                                                "' 重复定义（from 导入与"
                                                "既有名冲突；请用 as 别名）");
                                continue;
                            }
                            // FuncSig 复制进 localFuncs_：checkCall 的 Name
                            // 分支自动获得签名/缺省参数/@pure/retPending。
                            FuncSig cp = f->second.copy();
                            cp.name = bind;
                            localFuncs_[bind] = std::move(cp);
                            std::vector<TypePtr> ps = f->second.params;
                            ps.push_back(f->second.ret);
                            scope_.declare(
                                bind,
                                VarSym{
                                    std::make_shared<Type>(Type::Kind::Function,
                                                           ps),
                                    s.range});
                            continue;
                        }
                        if (auto g = ms->globals.find(item.name);
                            g != ms->globals.end()) {
                            // 模块级全局无装饰器挂点，恒 public；函数级
                            // @private/@internal 见 funcs 分支（0.9.6 L 节）。
                            if (scope_.find(bind)) {
                                diag_.error(Errc::E2007_DuplicateDefinition,
                                            file_, s.range,
                                            "顶层名 '" + bind +
                                                "' 重复定义（from 导入与"
                                                "既有名冲突；请用 as 别名）");
                                continue;
                            }
                            scope_.declare(bind, VarSym{g->second, s.range});
                            continue;
                        }
                    }
                    // 子模块：registry 中存在 "mod.item" 全名（加载器已把
                    // from-import 的子模块候选保证入图）。
                    std::string sub = full + "." + item.name;
                    if (imports_.count(sub)) {
                        declareModule(bind, sub);
                        continue;
                    }
                    diag_.error(Errc::E5003_MissingExport, file_, s.range,
                                "模块 '" + full + "' 没有可导出的成员 '" +
                                    item.name + "'");
                }
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
            // Dict/Set 上下文偏好（单发旗标，同 asListLit_）：注解为
            // Dict[K,V]/Set[T] 时字面量键/值/元素统一对齐注解（checkDictLit /
            // checkSetLit 消费，检查完清空）。dictAnn/setAnn 必须存活到
            // checkExpr 之后——旗标是裸指针，若把 resolveTypeExpr 的返回值塞
            // 进 if 作用域，语句结束即析构，消费块读悬垂 Type（非确定性
            // AV/栈溢出的根源）。
            TypePtr dictAnn;
            if (a.value->kind == Expr::Kind::DictLit && a.ann) {
                dictAnn = resolveTypeExpr(*a.ann);
                if (dictAnn->kind == Type::Kind::Dict &&
                    dictAnn->params.size() >= 2)
                    dictTarget_ = dictAnn.get();
                if (dictAnn->kind == Type::Kind::Set &&
                    dictAnn->params.size() >= 1)
                    setTarget_ = dictAnn.get();
            }
            TypePtr setAnn;
            if (a.value->kind == Expr::Kind::SetLit && a.ann) {
                setAnn = resolveTypeExpr(*a.ann);
                if (setAnn->kind == Type::Kind::Set &&
                    setAnn->params.size() >= 1)
                    setTarget_ = setAnn.get();
            }
            TypePtr vt = checkExpr(*a.value);
            dictTarget_ = nullptr;
            setTarget_ = nullptr;
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
                        "模块顶层仅允许 def / import / 赋值 / capability"
                        "（保证 .sgi 可静态生成；docstring 除外）");
            break;
        }
        case Stmt::Kind::Capability: {
            // 能力声明（0.9.7）：模块顶层并集收集；域名合法性已由解析器
            // 查表保证（未知域名 E1005），此处只记账。
            auto& cp = static_cast<const Capability&>(s);
            for (const auto& it : cp.caps) {
                Cap cap;
                if (capFromName(it.name, cap)) declaredCaps_ |= capBit(cap);
            }
            break;
        }
        default:
            diag_.error(Errc::E1007_UnsupportedFeature, file_, s.range,
                        "模块顶层仅允许 def / import / 赋值 / capability"
                        "（保证 .sgi 可静态生成；docstring 除外）");
        }
    }
}

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

// ============================================================================
// 语句检查
// ============================================================================

void TypeChecker::checkStmtList(const StmtList& list) {
    for (const auto& s : list) checkStmt(*s);
}

void TypeChecker::checkNested(const StmtList& list) {
    // 嵌套块（if/while/for/try 体）：深度 +1 供 defer 顶层限制判定。
    ++blockDepth_;
    checkStmtList(list);
    --blockDepth_;
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
        // List 上下文偏好（单发旗标）：注解为 List[...] 或重赋值既有 List
        // 变量时，数值列表字面量按 List[T] 推断而非默认 Tensor（0.9.3：
        // List 数值元素；checkListLit 消费旗标，检查完立即清空）。
        if (a.value->kind == Expr::Kind::ListLit &&
            a.target->kind == Expr::Kind::Name) {
            auto& n0 = static_cast<const Name&>(*a.target);
            bool listCtx = a.ann && a.ann->kind == TypeExpr::Kind::Named &&
                           a.ann->name == "List";
            if (!listCtx) {
                if (const VarSym* pre = scope_.find(n0.name))
                    listCtx = pre->type->kind == Type::Kind::List;
            }
            if (listCtx) asListLit_ = a.value.get();
        }
        // Dict/Set 上下文偏好（单发旗标，同 asListLit_）：注解为
        // Dict[K,V]/Set[T] 或既有 Dict/Set 变量重赋值时，字面量键/值/元素
        // 统一对齐目标（checkDictLit / checkSetLit 消费，检查完清空）。
        // dictAnn/setAnn 存活到 checkExpr 之后，理由同上。
        TypePtr dictAnn;
        if (a.value->kind == Expr::Kind::DictLit) {
            if (a.ann) {
                dictAnn = resolveTypeExpr(*a.ann);
                if (dictAnn->kind == Type::Kind::Dict &&
                    dictAnn->params.size() >= 2)
                    dictTarget_ = dictAnn.get();
                if (dictAnn->kind == Type::Kind::Set &&
                    dictAnn->params.size() >= 1)
                    setTarget_ = dictAnn.get();
            } else if (a.target->kind == Expr::Kind::Name) {
                if (const VarSym* pre = scope_.find(
                        static_cast<const Name&>(*a.target).name)) {
                    if (pre->type->kind == Type::Kind::Dict &&
                        pre->type->params.size() >= 2)
                        dictTarget_ = pre->type.get();
                    if (pre->type->kind == Type::Kind::Set &&
                        pre->type->params.size() >= 1)
                        setTarget_ = pre->type.get();
                }
            }
        }
        TypePtr setAnn;
        if (a.value->kind == Expr::Kind::SetLit) {
            if (a.ann) {
                setAnn = resolveTypeExpr(*a.ann);
                if (setAnn->kind == Type::Kind::Set &&
                    setAnn->params.size() >= 1)
                    setTarget_ = setAnn.get();
            } else if (a.target->kind == Expr::Kind::Name) {
                if (const VarSym* pre = scope_.find(
                        static_cast<const Name&>(*a.target).name))
                    if (pre->type->kind == Type::Kind::Set &&
                        pre->type->params.size() >= 1)
                        setTarget_ = pre->type.get();
            }
        }
        TypePtr vt = checkExpr(*a.value);
        asListLit_ = nullptr;
        dictTarget_ = nullptr;
        setTarget_ = nullptr;

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
            if (base->kind != Type::Kind::Dict) {
                // 字典键的类型由下方 Dict 分支检查（键不限于 int）。
                for (const auto& idx : ix.indices) {
                    TypePtr it = checkExpr(*idx);
                    if (it->kind != Type::Kind::Int &&
                        it->kind != Type::Kind::Err)
                        fail(Errc::E2001_TypeMismatch, idx->range,
                             "下标必须是 int，实际为 " + typeToString(it));
                }
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
            } else if (base->kind == Type::Kind::Tuple) {
                // 元组不可变：下标赋值显式拒绝（E2001；与 VM StoreIndex 的
                // 类型分派一致——检查器拦截后双模均不可达运行时分支）。
                fail(Errc::E2001_TypeMismatch, ix.base->range, "元组不可变");
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
        checkNested(st.thenBody);
        for (const auto& e : st.elifs) {
            requireBool(checkExpr(*e.cond), e.cond->range, "elif 条件");
            checkNested(e.body);
        }
        checkNested(st.elseBody);
        break;
    }
    case Stmt::Kind::While: {
        auto& st = static_cast<const While&>(s);
        requireBool(checkExpr(*st.cond), st.cond->range, "while 条件");
        ++loopDepth_;
        checkNested(st.body);
        --loopDepth_;
        break;
    }
    case Stmt::Kind::For: {
        auto& st = static_cast<const For&>(s);
        TypePtr it = checkExpr(*st.iter);
        TypePtr elem = tyErr();
        if (it->kind == Type::Kind::Range) elem = tyInt();
        else if (it->kind == Type::Kind::List) elem = it->params[0];
        else if (it->kind == Type::Kind::Str)
            elem = tyStr(); // 字节遍历（与 str 下标同域：按字节，非码点感知）
        else if (it->kind == Type::Kind::Dict)
            elem = it->params[0]; // 字典遍历键（插入序，与 VM/AOT 一致）
        else if (it->kind == Type::Kind::Set && it->params.size() >= 1)
            elem = it->params[0]; // 集合遍历元素（首插序，与 VM/AOT 一致）
        else if (it->kind != Type::Kind::Err)
            fail(Errc::E2001_TypeMismatch, st.iter->range,
                 "for 只能遍历 range、List、dict、Set 或 str，实际为 " +
                     typeToString(it));
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
        checkNested(st.body);
        --loopDepth_;
        break;
    }
    case Stmt::Kind::Return: {
        auto& st = static_cast<const Return&>(s);
        if (inDefer_)
            diag_.error(Errc::E1007_UnsupportedFeature, file_, s.range,
                        "defer 块内不允许 return（展开点在 return 之后，"
                        "跳转语义无定义）");
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
        if (inDefer_)
            diag_.error(Errc::E1007_UnsupportedFeature, file_, s.range,
                        s.kind == Stmt::Kind::Break
                            ? "defer 块内不允许 break"
                            : "defer 块内不允许 continue");
        else if (loopDepth_ == 0) {
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
        checkNested(st.body);
        checkNested(st.handlerBody);
        break;
    }
    case Stmt::Kind::Defer: {
        // defer 位置三限：函数体内、函数体顶层、不嵌套 defer（展开点
        // 静态可枚举是双模一致的根基，见 AST Defer 契约注释）。
        auto& st = static_cast<const Defer&>(s);
        if (!inFunc_) {
            diag_.error(Errc::E1007_UnsupportedFeature, file_, s.range,
                        "defer 只能出现在函数体内（模块顶层无函数退出点）");
        } else if (blockDepth_ > 0) {
            diag_.error(Errc::E1007_UnsupportedFeature, file_, s.range,
                        "defer 只能写在函数体顶层（不能嵌套在 "
                        "if/while/for/try 内）");
        } else if (inDefer_) {
            diag_.error(Errc::E1007_UnsupportedFeature, file_, s.range,
                        "defer 块内不允许嵌套 defer");
        }
        const bool prev = inDefer_;
        inDefer_ = true;
        checkNested(st.body);
        inDefer_ = prev;
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
    case Stmt::Kind::Capability:
        // 能力声明只属模块顶层（会话/依赖闭包在模块边界合并，块内声明
        // 无法进入 Program.capMask——门禁语义不成立，直接拒绝）。
        diag_.error(Errc::E1007_UnsupportedFeature, file_, s.range,
                    "capability 声明只能写在模块顶层（不能出现在函数/块内）");
        break;
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
    case Expr::Kind::DictLit:
        return checkDictLit(e);
    case Expr::Kind::SetLit:
        return checkSetLit(e);
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
        return checkTupleLit(e);
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

    if (TypePtr b = builtinType(n.name)) {
        // 能力记录（0.9.7）：需授权内建的每个调用名只记首个使用点，
        // check 收尾统一按域裁决（E5004）。
        Cap cap;
        if (capOfBuiltin(n.name, cap))
            capUses_.push_back({n.name, cap, n.range});
        return b;
    }

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
        name == "random" || name == "seed" || name == "sorted" ||
        // 0.9.4 IO 子系统：函数式文件/目录/标准输入（与 Compiler/MLIRGen 同表）
        name == "input" || name == "read_text" || name == "write_text" ||
        name == "append_text" || name == "exists" || name == "list_dir" ||
        // 0.9.4 F/G 专项：网络客户端（TCP/UDP/HTTP）与睡眠（同表）
        name == "net_connect" || name == "net_send" || name == "net_recv" ||
        name == "net_close" || name == "net_udp_send" ||
        name == "net_http_get" || name == "sleep" ||
        name == "net_bind" || name == "net_accept" ||
        name == "net_set_timeout" ||
        // 0.9.8 HTTP 服务端（同表）
        name == "net_http_read_request" || name == "net_http_write_response" ||
        // 0.9.4 M 专项：标准库广度（统计/倒序/哈希；同表）
        name == "mean" || name == "var" || name == "std" ||
        name == "median" ||
        name == "reversed" || name == "sha256" || name == "crc32" ||
        // 0.9.4 M 专项：JSON（动态 Json 值；同表）
        name == "json_decode" ||
        // 0.9.4 H 专项：系统编程（args/env/时间/进程/信号；同表）
        name == "args" || name == "env" || name == "has_env" ||
        name == "set_env" || name == "unset_env" || name == "epoch" ||
        name == "epoch_ms" || name == "localtime" || name == "gmtime" ||
        name == "system" || name == "spawn_wait" || name == "signal_caught" ||
        name == "signal_reset" ||
        // 0.9.6 O 节 FFI 宽类型域：数据地址导出（同表）
        name == "data_ptr" ||
        // format()：首参字面量时 Parser 已降糖为 str()+concat 链，能到达
        // 此处的只有非字面量首参/空实参形态（checkBuiltinCall 定向报错）
        name == "format") {
        auto t = std::make_shared<Type>(Type::Kind::Builtin);
        t->name = name;
        return t;
    }
    return nullptr;
}

/// 元组元素两两可比性（==/!= 语义，与 VM valuesEqual / AOT genTupleEq
/// 同规则）：数值族互通（按 double 比较）、双 str、None 对任意标量/元组
/// （异类恒 false）、嵌套元组递归同构。容器元素不可比——AOT 无容器相等
/// 先例，显式拒绝保持双模覆盖一致。
static bool tupleElemsComparable(const TypePtr& a, const TypePtr& b) {
    auto isNum = [](const TypePtr& t) {
        return t->kind == Type::Kind::Int || t->kind == Type::Kind::Float ||
               t->kind == Type::Kind::Bool;
    };
    if (isNum(a) && isNum(b)) return true;
    if (a->kind == Type::Kind::Str && b->kind == Type::Kind::Str) return true;
    if (a->kind == Type::Kind::None && b->kind == Type::Kind::None) return true;
    if (a->kind == Type::Kind::None || b->kind == Type::Kind::None)
        return true; // 异类 None：值恒 false（合法可比）
    if (a->kind == Type::Kind::Tuple && b->kind == Type::Kind::Tuple) {
        if (a->params.size() != b->params.size()) return false;
        for (size_t k = 0; k < a->params.size(); ++k)
            if (!tupleElemsComparable(a->params[k], b->params[k]))
                return false;
        return true;
    }
    return false;
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
        if (r->kind == Type::Kind::Set && r->params.size() >= 1 &&
            isConvertible(l, r->params[0]))
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
        // 元组 ==/!=：仅同构元组（长度 + 逐元素可比性），运行时逐位展开。
        if ((op == "==" || op == "!=") && l->kind == Type::Kind::Tuple &&
            r->kind == Type::Kind::Tuple) {
            if (l->params.size() != r->params.size())
                return fail(Errc::E2001_TypeMismatch, e.range,
                            "元组比较长度不一致：" + typeToString(l) + " 与 " +
                                typeToString(r));
            for (size_t k = 0; k < l->params.size(); ++k) {
                if (!tupleElemsComparable(l->params[k], r->params[k]))
                    return fail(Errc::E2001_TypeMismatch, e.range,
                                "元组第 " + std::to_string(k + 1) +
                                    " 个元素不可比较：" +
                                    typeToString(l->params[k]) + " 与 " +
                                    typeToString(r->params[k]));
            }
            return tyBool();
        }
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

    if (allNumeric && &e == asListLit_) {
        // List 注解/重赋值上下文：数值字面量按 List[T] 推断（int/float 混排
        // 统一为 float，与下方 List 路径及 AOT genListLit 的元素统一一致）。
        // checkedAsTensor 保持 false——编译器/AOT 均据此走 NewList。
        TypePtr elem = tyInt();
        for (auto& t : ts)
            if (t->kind == Type::Kind::Float) elem = tyFloat();
        if (elem->kind == Type::Kind::Float) {
            // int 元素做 float 提升标记（Q3 边界，与 AOT genListLit 按元素
            // 种类统一对齐）：VM 路径经 promoteF64_ 集合插 IntToFloat。
            for (size_t i = 0; i < ts.size(); ++i)
                if (ts[i]->kind == Type::Kind::Int)
                    promoteF64_.insert(ll.elems[i].get());
        }
        return makeType(Type::Kind::List, {elem});
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

TypePtr TypeChecker::checkDictLit(const Expr& e) {
    auto& dl = static_cast<const DictLit&>(e);
    if (dl.pairs.empty()) {
        // 空 {} 在 Set[T] 上下文 → 空集合（单一事实源 checkedAsSet，编译器
        // 据此编 NewSet(0)）；否则按注解推断为字典。返回新建 Set 类型——
        // 注解表持有，不能共享 setTarget_ 指向的注解本体所有权。
        if (setTarget_ && setTarget_->params.size() >= 1) {
            dl.checkedAsSet = true;
            return makeType(Type::Kind::Set, {setTarget_->params[0]});
        }
        return tyUnknown(); // 空字典 {}：类型由注解推断
    }

    std::vector<TypePtr> ks, vs;
    for (const auto& p : dl.pairs) {
        ks.push_back(checkExpr(*p.first));
        vs.push_back(checkExpr(*p.second));
    }
    // 键类型：限标量（int/float/bool/str），跨对统一（int∘float → float）。
    TypePtr kt = tyErr();
    for (size_t i = 0; i < ks.size(); ++i) {
        TypePtr k = ks[i];
        if (k->kind == Type::Kind::Err) return tyErr(); // 级联抑制
        if (k->kind != Type::Kind::Int && k->kind != Type::Kind::Float &&
            k->kind != Type::Kind::Bool && k->kind != Type::Kind::Str)
            return fail(Errc::E2001_TypeMismatch, dl.pairs[i].first->range,
                        "字典键必须是 int/float/bool/str，实际为 " +
                            typeToString(k));
        if (kt->kind == Type::Kind::Err) {
            kt = k;
        } else if (!typeEquals(kt, k)) {
            if (TypePtr u = unifyNumeric(kt, k)) {
                kt = u;
            } else {
                return fail(Errc::E2001_TypeMismatch, e.range,
                            "字典键类型不一致：" + typeToString(kt) + " 与 " +
                                typeToString(k));
            }
        }
    }
    // 值类型：跨对统一（int∘float → float；容器值需类型一致）。
    TypePtr vt = tyErr();
    for (size_t i = 0; i < vs.size(); ++i) {
        TypePtr v = vs[i];
        if (v->kind == Type::Kind::Err) return tyErr();
        if (vt->kind == Type::Kind::Err) {
            vt = v;
        } else if (!typeEquals(vt, v)) {
            if (TypePtr u = unifyNumeric(vt, v)) {
                vt = u;
            } else {
                return fail(Errc::E2001_TypeMismatch, e.range,
                            "字典值类型不一致：" + typeToString(vt) + " 与 " +
                                typeToString(v));
            }
        }
    }
    // Dict 上下文偏好（单发旗标消费）：赋值/缺省值目标为 Dict[K,V] 时
    // 键/值统一对齐注解——字面量自推断 Dict{float,int} 与变量注解
    // Dict{float,float} 槽位语义脱节是双模漂移源；对齐后字面量注解 =
    // 变量注解（读取侧按变量注解位还原），被提升的 int 键/值打 Q3 标记。
    if (dictTarget_) {
        const Type& tg = *dictTarget_;
        if (isConvertible(kt, tg.params[0])) {
            if (tg.params[0]->kind == Type::Kind::Float)
                for (size_t i = 0; i < ks.size(); ++i)
                    if (ks[i]->kind == Type::Kind::Int)
                        promoteF64_.insert(dl.pairs[i].first.get());
            kt = tg.params[0];
        }
        if (isConvertible(vt, tg.params[1])) {
            if (tg.params[1]->kind == Type::Kind::Float)
                for (size_t i = 0; i < vs.size(); ++i)
                    if (vs[i]->kind == Type::Kind::Int)
                        promoteF64_.insert(dl.pairs[i].second.get());
            vt = tg.params[1];
        }
    }
    // 统一为 float 的 int 键/值做提升标记（Q3 边界，与 ListLit 同规则；
    // VM 路径插 IntToFloat，AOT genDictLit 按元素种类统一）。
    if (kt->kind == Type::Kind::Float)
        for (size_t i = 0; i < ks.size(); ++i)
            if (ks[i]->kind == Type::Kind::Int)
                promoteF64_.insert(dl.pairs[i].first.get());
    if (vt->kind == Type::Kind::Float)
        for (size_t i = 0; i < vs.size(); ++i)
            if (vs[i]->kind == Type::Kind::Int)
                promoteF64_.insert(dl.pairs[i].second.get());
    return makeType(Type::Kind::Dict, {kt, vt});
}

TypePtr TypeChecker::checkSetLit(const Expr& e) {
    auto& sl = static_cast<const SetLit&>(e);
    if (sl.elems.empty()) return tyUnknown(); // 防御：解析器不产空 SetLit

    std::vector<TypePtr> ts;
    for (const auto& el : sl.elems) ts.push_back(checkExpr(*el));
    // 元素类型：限标量（int/float/bool/str），跨元素统一（int∘float → float；
    // 语义与 Dict 键一致：数值跨类型同键去重，故元素类型必须可统一）。
    TypePtr et = tyErr();
    for (size_t i = 0; i < ts.size(); ++i) {
        TypePtr t = ts[i];
        if (t->kind == Type::Kind::Err) return tyErr(); // 级联抑制
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Float &&
            t->kind != Type::Kind::Bool && t->kind != Type::Kind::Str)
            return fail(Errc::E2001_TypeMismatch, sl.elems[i]->range,
                        "集合元素必须是 int/float/bool/str，实际为 " +
                            typeToString(t));
        if (et->kind == Type::Kind::Err) {
            et = t;
        } else if (!typeEquals(et, t)) {
            if (TypePtr u = unifyNumeric(et, t)) {
                et = u;
            } else {
                return fail(Errc::E2001_TypeMismatch, e.range,
                            "集合元素类型不一致：" + typeToString(et) + " 与 " +
                                typeToString(t));
            }
        }
    }
    // Set 上下文偏好（单发旗标消费）：赋值/缺省值目标为 Set[T] 时元素统一
    // 对齐注解——对齐后字面量注解 = 变量注解，被提升的 int 元素打 Q3 标记。
    if (setTarget_ && setTarget_->params.size() >= 1 &&
        isConvertible(et, setTarget_->params[0])) {
        if (setTarget_->params[0]->kind == Type::Kind::Float)
            for (size_t i = 0; i < ts.size(); ++i)
                if (ts[i]->kind == Type::Kind::Int)
                    promoteF64_.insert(sl.elems[i].get());
        et = setTarget_->params[0];
    }
    // 统一为 float 的 int 元素做提升标记（Q3 边界，与 Dict 键同规则；
    // VM 路径插 IntToFloat，AOT genSetLit 按元素种类统一）。
    if (et->kind == Type::Kind::Float)
        for (size_t i = 0; i < ts.size(); ++i)
            if (ts[i]->kind == Type::Kind::Int)
                promoteF64_.insert(sl.elems[i].get());
    return makeType(Type::Kind::Set, {et});
}

TypePtr TypeChecker::checkTupleLit(const Expr& e) {
    auto& tl = static_cast<const TupleLit&>(e);
    // 异构定长不可变：逐元素检查，元素白名单 = int/float/bool/str/None/
    // 嵌套元组。容器与 Result 元素显式拒绝——AOT 位槽 ABI 下 Tensor 的
    // tInt/tRank 元数据经位槽编解码会静默丢失，Result/Dict 的打印本就
    // AOT unsupported（拒绝即文档，与 0.9.3 Set 元素白名单同哲学）。
    std::vector<TypePtr> ts;
    bool hasErr = false;
    for (const auto& el : tl.elems) {
        TypePtr t = checkExpr(*el);
        switch (t->kind) {
        case Type::Kind::Int:
        case Type::Kind::Float:
        case Type::Kind::Bool:
        case Type::Kind::Str:
        case Type::Kind::None:
        case Type::Kind::Tuple:
            break;
        case Type::Kind::Err:
            hasErr = true; // 级联抑制：先收集完剩余元素诊断再整体报 Err
            continue;
        default:
            fail(Errc::E1007_UnsupportedFeature, el->range,
                 "元组元素类型不支持 " + typeToString(t) +
                     "（仅限 int/float/bool/str/None 与嵌套元组）");
            hasErr = true;
            continue;
        }
        ts.push_back(std::move(t));
    }
    if (hasErr) return tyErr();
    return makeType(Type::Kind::Tuple, std::move(ts));
}

TypePtr TypeChecker::checkIndex(const Expr& e) {
    auto& ix = static_cast<const Index&>(e);
    TypePtr base = checkExpr(*ix.base);
    if (base->kind == Type::Kind::Err) return tyErr();
    if (base->kind == Type::Kind::Dict) {
        // 字典下标 = 键（K 类型，可 int→float 放宽）；其余容器仍限 int 下标。
        if (ix.indices.size() != 1)
            return fail(Errc::E2004_ShapeMismatch, e.range,
                        "dict 只支持单下标");
        TypePtr it = checkExpr(*ix.indices[0]);
        if (it->kind != Type::Kind::Err &&
            !isConvertible(it, base->params[0]))
            return fail(Errc::E2001_TypeMismatch, ix.indices[0]->range,
                        "字典键类型不匹配：需要 " +
                            typeToString(base->params[0]) + "，实际为 " +
                            typeToString(it));
        return base->params[1];
    }
    if (base->kind == Type::Kind::Set)
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "Set 不支持下标（集合无序；成员判定用 'in'，遍历用 for）");
    if (base->kind == Type::Kind::Tuple) {
        // 元组定长：下标限编译期整数字面量，越界静态拒绝（运行期无任何
        // panic 路径——VM normIndex / AOT list_get_i 的越界分支均不可达）。
        if (ix.indices.size() != 1)
            return fail(Errc::E2004_ShapeMismatch, e.range, "元组只支持单下标");
        const Expr& ie = *ix.indices[0];
        if (ie.kind != Expr::Kind::IntLit)
            return fail(Errc::E2001_TypeMismatch, ie.range,
                        "元组下标必须是编译期整数字面量");
        const int64_t i = static_cast<const IntLit&>(ie).value;
        const int64_t n = static_cast<int64_t>(base->params.size());
        if (i >= n)
            return fail(Errc::E2004_ShapeMismatch, e.range,
                        "元组下标越界：长度 " + std::to_string(n) + "，实际 " +
                            std::to_string(i));
        return base->params[static_cast<size_t>(i)];
    }
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

    // str 成员方法（0.9.4）：纯函数签名表（唯一事实源）；实现分派在
    // VM callValueMethod 与 AOT lang.str_*（kLowerings 降级），panic 消息
    // 三方逐字一致。upper/lower 仅 ASCII；下标/查找按 UTF-8 字节。
    if (base->kind == Type::Kind::Str) {
        if (at.name == "upper" || at.name == "lower" || at.name == "trim" ||
            at.name == "trim_start" || at.name == "trim_end")
            return makeFunction({}, tyStr());
        if (at.name == "split")
            return makeFunction({tyStr()},
                                makeType(Type::Kind::List, {tyStr()}));
        if (at.name == "join")
            return makeFunction({makeType(Type::Kind::List, {tyStr()})},
                                tyStr());
        if (at.name == "replace")
            return makeFunction({tyStr(), tyStr()}, tyStr());
        if (at.name == "find" || at.name == "index")
            return makeFunction({tyStr()}, tyInt());
        if (at.name == "contains" || at.name == "startswith" ||
            at.name == "endswith")
            return makeFunction({tyStr()}, tyBool());
    }

    // List 成员方法（0.9.4）：contains/index/count，实参 = 元素类型
    // （int 键遇 float 列表经 isConvertible 放行，VM/AOT 各自提升 f64，
    // 与 'in' 运算符同一唯一放宽）。
    if (base->kind == Type::Kind::List && !base->params.empty()) {
        TypePtr elem = base->params[0];
        if (at.name == "contains") return makeFunction({elem}, tyBool());
        if (at.name == "index" || at.name == "count")
            return makeFunction({elem}, tyInt());
    }

    // Json 成员方法（0.9.4 M 专项）：动态 JSON 值的只读取值面。全部纯
    // 函数；at(i) 数组下标越界 panic、get(k) 对象缺键返回 null（两分：
    // 下标越界是编程错误，缺键是数据常态）。
    if (base->kind == Type::Kind::Json) {
        TypePtr retJson = makeType(Type::Kind::Json);
        if (at.name == "is_null" || at.name == "is_bool" ||
            at.name == "is_int" || at.name == "is_float" ||
            at.name == "is_str" || at.name == "is_list" ||
            at.name == "is_obj")
            return makeFunction({}, tyBool());
        if (at.name == "as_int") return makeFunction({}, tyInt());
        if (at.name == "as_float") return makeFunction({}, tyFloat());
        if (at.name == "as_bool") return makeFunction({}, tyBool());
        if (at.name == "as_str") return makeFunction({}, tyStr());
        if (at.name == "at") return makeFunction({tyInt()}, retJson);
        if (at.name == "get") return makeFunction({tyStr()}, retJson);
        if (at.name == "len") return makeFunction({}, tyInt());
        if (at.name == "keys")
            return makeFunction({},
                                makeType(Type::Kind::List, {tyStr()}));
        if (at.name == "dump") return makeFunction({}, tyStr());
    }

    // 模块成员访问：foo.func / foo.CONST / 包路径中间段 a.b（0.9.6）。
    if (base->kind == Type::Kind::Module) {
        auto it = imports_.find(base->name);
        if (it != imports_.end()) {
            const ModuleSymbols& ms = *it->second;
            if (auto f = ms.funcs.find(at.name); f != ms.funcs.end()) {
                if (!checkVis(f->second.vis, ms.name, at.name, e.range))
                    return tyErr();
                std::vector<TypePtr> ps = f->second.params;
                ps.push_back(f->second.ret);
                return std::make_shared<Type>(Type::Kind::Function, ps);
            }
            if (auto g = ms.globals.find(at.name); g != ms.globals.end()) {
                // 模块级全局无装饰器挂点，恒 public（0.9.6 L 节）。
                return g->second;
            }
        }
        // 宽容路径重写（0.9.6）：import a.b 绑定首段 a → 真名 a.b，规范
        // 书写 a.b.CONST 的中间段 b 与真名尾段重合——重合段透传同一
        // Module 类型。与 qualifiedModuleName 同序（重写优先于下钻）。
        std::string suffix = '.' + at.name;
        if (base->name.size() > suffix.size() &&
            base->name.compare(base->name.size() - suffix.size(),
                               suffix.size(), suffix) == 0)
            return base;
        // 子模块链：base.name 与成员名拼接的全名在 registry 中存在即返回
        // Module 类型（下一段访问继续进入本分支逐级解析，支撑 a.b.c()）。
        std::string sub = base->name + "." + at.name;
        if (imports_.count(sub)) {
            auto mod = std::make_shared<Type>(Type::Kind::Module);
            mod->name = sub;
            return mod;
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
        std::string qm = qualifiedModuleName(*at.base);
        if (!qm.empty()) {
            if (auto it = imports_.find(qm); it != imports_.end()) {
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
    // K 专项：@pure 函数内的内建纯度门。白名单制——数学/转换/排序类
    // 内建为纯；一切涉及 I/O、时间、随机、环境、网络、进程的内建（含
    // 后续并行线新增者，未列入白名单即非纯）在 @pure 函数体内拒绝。
    if (inPureFn_) {
        static const std::unordered_set<std::string> kPureBuiltins = {
            "str",   "len",  "sum",      "range", "ok",     "err",
            "abs",   "sqrt", "exp",      "log",   "max",    "min",
            "transpose", "reshape",
            "sin",   "cos",  "tan",      "asin",  "acos",   "atan",
            "sinh",  "cosh", "tanh",     "atan2", "floor",  "ceil",
            "round", "trunc", "sorted",
            // 0.9.4 M 专项：统计/倒序/哈希（全部为纯函数）
            "mean", "var", "std", "median", "reversed", "sha256", "crc32",
            // format()/f-string：降糖为 str()+concat，纯字符串计算
            "format",
            // 0.9.4 M 专项：JSON 解码（纯函数，不触碰外部状态）
            "json_decode",
            // 0.9.6 O 节：地址导出为纯读取（解引用才是非纯边界）
            "data_ptr"};
        if (!kPureBuiltins.count(bn)) {
            return fail(Errc::E1007_UnsupportedFeature, e.range,
                        "@pure 函数内禁止调用非纯内建 '" + bn +
                            "'（纯度白名单：数学/转换/排序类）");
        }
    }
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
            t->kind == Type::Kind::Tensor || t->kind == Type::Kind::Tuple ||
            t->kind == Type::Kind::Err)
            return tyStr();
        fail(Errc::E2001_TypeMismatch, c.args[0]->range,
             "str() 不支持 " + typeToString(t));
        return tyErr();
    }
    // format()：合法形态（字面量首参）已在 Parser 降糖，能到达这里的都是
    // 非法形态——给出定向错误而非笼统的未知内建。
    if (bn == "format") {
        if (c.args.empty())
            return fail(Errc::E1007_UnsupportedFeature, e.range,
                        "format() 首参必须是字符串字面量（编译期展开）");
        return fail(Errc::E1007_UnsupportedFeature, c.args[0]->range,
                    "format() 首参必须是字符串字面量（编译期展开）");
    }
    if (bn == "len") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "len() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::List || t->kind == Type::Kind::Dict ||
            t->kind == Type::Kind::Set || t->kind == Type::Kind::Str ||
            t->kind == Type::Kind::Tuple || t->kind == Type::Kind::Err)
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
    if (bn == "data_ptr") {
        // O 节 FFI 宽类型域：数据地址导出。仅连续原生缓冲（张量/str）——
        // List 元素为 tagged 值非连续数组，透传地址无 C 数组语义，明确拒绝。
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "data_ptr() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Tensor &&
            (t->params[0]->kind == Type::Kind::Int ||
             t->params[0]->kind == Type::Kind::Float))
            return tyInt();
        if (t->kind == Type::Kind::Str) return tyInt();
        fail(Errc::E2001_TypeMismatch, c.args[0]->range,
             "data_ptr() 仅支持 Tensor[int]/Tensor[float]/str"
             "（List 元素为 tagged 值，非连续原生数组，禁止透传指针），得到 " +
                 typeToString(t));
        return tyErr();
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
    // ---- IO 内建（0.9.4）：函数式文件/目录/标准输入。路径一律 str（窄字节
    // 串）；失败即 panic（可被 try/except 捕获），无 Result 化——调用方取舍。
    if (bn == "input") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "input() 不需要参数");
        return tyStr();
    }
    if (bn == "read_text" || bn == "exists" || bn == "list_dir") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind != Type::Kind::Str)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 路径参数必须是 str，实际为 " +
                            typeToString(t));
        if (bn == "exists") return tyBool();
        if (bn == "list_dir") return makeType(Type::Kind::List, {tyStr()});
        return tyStr(); // read_text
    }
    if (bn == "write_text" || bn == "append_text") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 2 个参数");
        for (size_t k = 0; k < 2; ++k) {
            TypePtr t = checkExpr(*c.args[k]);
            if (t->kind == Type::Kind::Err) return tyErr();
            if (t->kind != Type::Kind::Str)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            bn + "() 参数必须是 str，实际为 " +
                                typeToString(t));
        }
        return tyInt(); // 写入字节数
    }
    // ---- 网络/睡眠内建（0.9.4 F/G 专项）：客户端最小闭环。句柄是不透明
    // int（1 起始表下标）；失败即 panic（与 IO 同哲学），无 Result 化。
    if (bn == "net_connect") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_connect() 恰好需要 2 个参数");
        TypePtr h = checkExpr(*c.args[0]);
        TypePtr p = checkExpr(*c.args[1]);
        if (h->kind != Type::Kind::Str && h->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "net_connect() 主机参数必须是 str，实际为 " +
                            typeToString(h));
        if (p->kind != Type::Kind::Int && p->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[1]->range,
                        "net_connect() 端口参数必须是 int，实际为 " +
                            typeToString(p));
        return tyInt();
    }
    if (bn == "net_send" || bn == "net_recv") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 2 个参数");
        TypePtr h = checkExpr(*c.args[0]);
        TypePtr x = checkExpr(*c.args[1]);
        if (h->kind != Type::Kind::Int && h->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 句柄参数必须是 int，实际为 " +
                            typeToString(h));
        const char* what = bn == "net_send" ? "数据参数" : "上限参数";
        Type::Kind want =
            bn == "net_send" ? Type::Kind::Str : Type::Kind::Int;
        if (x->kind != want && x->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[1]->range,
                        bn + "() " + what + "类型不符，实际为 " +
                            typeToString(x));
        return bn == "net_send" ? tyInt() : tyStr();
    }
    if (bn == "net_close" || bn == "sleep") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 参数必须是 int，实际为 " +
                            typeToString(t));
        return tyNone();
    }
    if (bn == "net_udp_send") {
        if (c.args.size() != 3)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_udp_send() 恰好需要 3 个参数");
        const char* whats[3] = {"主机参数", "端口参数", "数据参数"};
        Type::Kind wants[3] = {Type::Kind::Str, Type::Kind::Int,
                               Type::Kind::Str};
        for (size_t k = 0; k < 3; ++k) {
            TypePtr t = checkExpr(*c.args[k]);
            if (t->kind != wants[k] && t->kind != Type::Kind::Err)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            "net_udp_send() " + std::string(whats[k]) +
                                "类型不符，实际为 " + typeToString(t));
        }
        return tyInt();
    }
    if (bn == "net_http_get") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_http_get() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "net_http_get() URL参数必须是 str，实际为 " +
                            typeToString(t));
        return tyStr();
    }
    // TCP 服务端：bind（bind+listen 一步）→ accept（阻塞，返回连接句柄）。
    // 超时：set_timeout 同时作用于收/发（ms<=0 显式拒绝——0 语义平台分歧，
    // 拒绝即文档）。
    if (bn == "net_bind") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_bind() 恰好需要 2 个参数");
        TypePtr h = checkExpr(*c.args[0]);
        TypePtr p = checkExpr(*c.args[1]);
        if (h->kind != Type::Kind::Str && h->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "net_bind() 主机参数必须是 str，实际为 " +
                            typeToString(h));
        if (p->kind != Type::Kind::Int && p->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[1]->range,
                        "net_bind() 端口参数必须是 int，实际为 " +
                            typeToString(p));
        return tyInt();
    }
    if (bn == "net_accept") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_accept() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "net_accept() 句柄参数必须是 int，实际为 " +
                            typeToString(t));
        return tyInt();
    }
    if (bn == "net_set_timeout") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_set_timeout() 恰好需要 2 个参数");
        for (size_t k = 0; k < 2; ++k) {
            TypePtr t = checkExpr(*c.args[k]);
            if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            "net_set_timeout() 参数必须是 int，实际为 " +
                                typeToString(t));
        }
        return tyNone();
    }
    // ---- M 专项：标准库广度（统计/倒序/哈希） ----
    // mean/var/std：张量与数值列表（int/float 元素），恒返回 f64（总体方差）。
    // median：同输入域，排序取中（偶数取中间两值平均），恒返回 f64。
    if (bn == "mean" || bn == "var" || bn == "std" || bn == "median") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind == Type::Kind::Tensor) return tyFloat();
        if (t->kind == Type::Kind::List && !t->params.empty()) {
            TypePtr el = t->params[0];
            if (el->kind == Type::Kind::Int || el->kind == Type::Kind::Float)
                return tyFloat();
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 仅支持数值列表（int/float 元素），实际 List[" +
                            typeToString(el) + "]");
        }
        return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                    bn + "() 仅支持张量或数值列表，实际为 " +
                        typeToString(t));
    }
    // reversed：List[T] → List[T]（倒序新列表）；str → str（字节倒序）。
    if (bn == "reversed") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "reversed() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind == Type::Kind::List || t->kind == Type::Kind::Str)
            return t;
        return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                    "reversed() 仅支持列表或字符串，实际为 " +
                        typeToString(t));
    }
    // sha256：str → 64 位小写 hex；crc32：str → 非负 int（CRC-32/ISO-HDLC）。
    if (bn == "sha256" || bn == "crc32") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 参数必须是 str，实际为 " +
                            typeToString(t));
        return bn == "sha256" ? tyStr() : tyInt();
    }
    // json_decode：str → Json（动态 JSON 值；解析失败运行时 panic，
    // 与 BuilderRTJson.hpp 的 sg_json_decode 逐字一致）。
    if (bn == "json_decode") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "json_decode() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "json_decode() 参数必须是 str，实际为 " +
                            typeToString(t));
        return makeType(Type::Kind::Json);
    }
    // ---- HTTP 服务端内建（0.9.8）：读请求 → Json / 写响应 ----
    if (bn == "net_http_read_request") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_http_read_request() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "net_http_read_request() 句柄参数必须是 int，实际为 " +
                            typeToString(t));
        return makeType(Type::Kind::Json);
    }
    if (bn == "net_http_write_response") {
        if (c.args.size() != 4)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_http_write_response() 恰好需要 4 个参数");
        const char* const names[4] = {"句柄", "状态码", "内容类型", "响应体"};
        const bool wantInt[4] = {true, true, false, false};
        for (int k = 0; k < 4; ++k) {
            TypePtr t = checkExpr(*c.args[k]);
            bool ok = wantInt[k] ? (t->kind == Type::Kind::Int ||
                                    t->kind == Type::Kind::Err)
                                 : (t->kind == Type::Kind::Str ||
                                    t->kind == Type::Kind::Err);
            if (!ok)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            std::string("net_http_write_response() ") +
                                names[k] + "参数必须是 " +
                                (wantInt[k] ? "int" : "str") + "，实际为 " +
                                typeToString(t));
        }
        return tyInt();
    }
    // ---- 系统编程内建（0.9.4 H 专项）：args/env/时间日期/进程/信号 ----
    if (bn == "args") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "args() 不需要参数");
        return makeType(Type::Kind::List, {tyStr()});
    }
    if (bn == "env" || bn == "has_env" || bn == "unset_env") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 名称参数必须是 str，实际为 " +
                            typeToString(t));
        if (bn == "env") return tyStr();
        if (bn == "has_env") return tyBool();
        return tyNone();
    }
    if (bn == "set_env") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "set_env() 恰好需要 2 个参数");
        for (size_t k = 0; k < 2; ++k) {
            TypePtr t = checkExpr(*c.args[k]);
            if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            std::string(k == 0 ? "set_env() 名称参数"
                                               : "set_env() 值参数") +
                                "必须是 str，实际为 " + typeToString(t));
        }
        return tyBool();
    }
    if (bn == "epoch" || bn == "epoch_ms") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 不需要参数");
        return tyInt();
    }
    if (bn == "localtime" || bn == "gmtime") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 时间参数必须是 int，实际为 " +
                            typeToString(t));
        return makeType(Type::Kind::List, {tyInt()});
    }
    if (bn == "system") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "system() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "system() 命令参数必须是 str，实际为 " +
                            typeToString(t));
        return tyInt();
    }
    if (bn == "spawn_wait") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "spawn_wait() 恰好需要 2 个参数");
        TypePtr p = checkExpr(*c.args[0]);
        TypePtr l = checkExpr(*c.args[1]);
        if (p->kind != Type::Kind::Str && p->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "spawn_wait() 路径参数必须是 str，实际为 " +
                            typeToString(p));
        bool isStrList = l->kind == Type::Kind::List && !l->params.empty() &&
                         l->params[0]->kind == Type::Kind::Str;
        if (!isStrList && l->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[1]->range,
                        "spawn_wait() 参数列表必须是 List[str]，实际为 " +
                            typeToString(l));
        return tyInt();
    }
    if (bn == "signal_caught") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "signal_caught() 不需要参数");
        return tyBool();
    }
    if (bn == "signal_reset") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "signal_reset() 不需要参数");
        return tyNone();
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
