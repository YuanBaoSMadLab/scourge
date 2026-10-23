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
    isScript_ = false;
    // 隐式单态化（批 9-③）：实例注册目标与克隆体所有权，逐模块重置。
    curOut_ = &out;
    monoDefs_.clear();
    monoDepth_ = 0;

    // 脚本模式脱糖（互斥模式）：顶层可执行语句并入合成入口 main——收集/
    // 检查/双后端（VM 入口、AOT 包壳）按普通模块消费，单一事实源在 Sema，
    // VM/AOT 零特判（双模一致的实现根基）。
    desugarScript_(m);

    scope_.push(); // 模块作用域
    collectTopLevel(m, out);

    for (auto& stmt : m.body) {
        if (stmt->kind == Stmt::Kind::FuncDef)
            checkFunction(static_cast<FuncDef&>(*stmt), out);
    }

    // 单态化实例并入模块体：BC/AOT 遍历模块体按名编译实例（泛型模板
    // 已被后端跳过，调用点已被改写为实例符号）。主体检查已收尾，此时
    // 追加不干扰上文的模块体遍历。
    for (auto& d : monoDefs_) m.body.push_back(std::move(d));
    monoDefs_.clear();

    // 模块级容器占位收尾：未定型的空 List/Dict 报 E2002；精化后的类型
    // 同步进导出表（AOT 全局槽位按 out.globals 定容器位型）。
    reportUnresolvedContainers();
    for (const auto& [name, sym] : scope_.backLayer()) {
        auto g = out.globals.find(name);
        if (g != out.globals.end() && sym.type) g->second = sym.type;
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

// ============================================================================
// 脚本模式脱糖（顶层可执行语句 → 合成入口 main，互斥模式）
// ============================================================================

void TypeChecker::desugarScript_(Module& m) {
    // REPL 会话模块豁免：__repl__ 是求值包装保留名（Tool/Repl 跨层契约）。
    // 会话的顶层语句（preamble）走模块全局路径、求值入口独立（entryFunc=
    // __repl__）；若被脱糖，顶层定义会搬进永不执行的合成 main，破坏会话
    // 语义。文件模式撞名则安全退化：顶层语句走 collectTopLevel 老处方。
    for (const auto& s : m.body)
        if (s->kind == Stmt::Kind::FuncDef &&
            static_cast<const FuncDef&>(*s).name == "__repl__")
            return;
    // 顶层赋值的模式归属（判定与搬移两处消费的单一谓词）：带注解
    // （NAME: T = v）是模块全局声明——0.9.7 的既定形态，函数体须能引用
    // 模块常量（docs 02 词法作用域：SCALE 示例），搬进入口体会让全部
    // def 变成悬空引用（E2003 假错 + 依赖层误判脚本不可 import）；
    // 裸赋值（x = v）才是脚本模式语句（入口局部，Python 手感）。
    // 处方语义：函数体内要引用的顶层名必须带注解。
    auto isModuleAssign = [](const Stmt& s) {
        return s.kind == Stmt::Kind::Assign &&
               static_cast<const Assign&>(s).ann != nullptr;
    };
    // 判定：模块体含"可执行"顶层语句（非 def/import/capability，docstring
    // 除外）即脚本模式。裸赋值并入入口体——脚本顶层变量是入口局部
    // 变量（Python 手感）；模块全局路径仅服务于被 import 的声明模块，两
    // 者互斥后互不干扰。
    size_t firstExec = m.body.size();
    for (size_t i = 0; i < m.body.size(); ++i) {
        const Stmt& s = *m.body[i];
        switch (s.kind) {
        case Stmt::Kind::FuncDef:
        case Stmt::Kind::Import:
        case Stmt::Kind::Capability:
            break;
        case Stmt::Kind::ExprStmt: {
            auto& es = static_cast<const ExprStmt&>(s);
            if (i == 0 && es.expr && es.expr->kind == Expr::Kind::StrLit)
                break; // docstring 保留为模块声明（collectTopLevel 同规则）
            firstExec = i;
            break;
        }
        default:
            if (isModuleAssign(s)) break; // 注解常量：模块声明，非脚本语句
            firstExec = i;
            break;
        }
        if (firstExec != m.body.size()) break;
    }
    if (firstExec == m.body.size()) return; // 纯声明模块，入口仪式照旧
    // 互斥（定案）：脚本文件不得再写 def main——单一入口来源，杜绝
    // "顶层语句与 main 谁先执行"的不可判定语义。
    for (const auto& s : m.body) {
        if (s->kind == Stmt::Kind::FuncDef &&
            static_cast<const FuncDef&>(*s).name == "main") {
            diag_.error(Errc::E2007_DuplicateDefinition, file_, s->range,
                        "脚本模式冲突：文件已含顶层可执行语句（入口自动合成），"
                        "def main 与之互斥；删除顶层语句或删除 def main 二选一");
            return;
        }
    }
    isScript_ = true;
    SourceRange entryRange = m.body[firstExec]->range;
    auto mainDef =
        std::make_unique<FuncDef>(entryRange, "main", entryRange);
    auto retInt =
        std::make_unique<TypeExpr>(TypeExpr::Kind::Named, entryRange);
    retInt->name = "int"; // 退出码契约：类型不符由函数检查期统一报 E2001
    mainDef->retType = std::move(retInt);
    // 选择性搬移（保持原相对顺序）：可执行语句进入口体；裸 return 补 0
    // （脚本模式专属放行：return 即退出码，可省略——末尾非 return 时由
    // 收尾补 return 0）。def/import/capability/docstring 留在模块体。
    for (size_t i = 0; i < m.body.size(); ++i) {
        Stmt& s = *m.body[i];
        bool isDecl = s.kind == Stmt::Kind::FuncDef ||
                      s.kind == Stmt::Kind::Import ||
                      s.kind == Stmt::Kind::Capability ||
                      isModuleAssign(s); // 注解常量留模块层（函数可引用）
        if (!isDecl && s.kind == Stmt::Kind::ExprStmt) {
            auto& es = static_cast<const ExprStmt&>(s);
            if (i == 0 && es.expr && es.expr->kind == Expr::Kind::StrLit)
                isDecl = true; // docstring
        }
        if (isDecl) continue;
        if (s.kind == Stmt::Kind::Return) {
            auto& r = static_cast<Return&>(s);
            if (!r.value)
                r.value = std::make_unique<IntLit>(r.range, 0);
        }
        mainDef->body.push_back(std::move(m.body[i]));
    }
    // 末尾非 return → 补 return 0（退出码 0，脚本可完全省略 return）。
    if (mainDef->body.empty() ||
        mainDef->body.back()->kind != Stmt::Kind::Return)
        mainDef->body.push_back(
            std::make_unique<Return>(entryRange,
                                     std::make_unique<IntLit>(entryRange, 0)));
    // 压缩被搬空的语句槽，声明保持原相对顺序，合成 main 追加在尾部
    // （收集期先声明导入绑定，函数检查循环最后检查 main——名字解析天然
    // 有序）。
    size_t w = 0;
    for (size_t i = 0; i < m.body.size(); ++i)
        if (m.body[i]) m.body[w++] = std::move(m.body[i]);
    m.body.resize(w);
    m.body.push_back(std::move(mainDef));
}

void TypeChecker::checkFunction(FuncDef& f, ModuleSymbols& out) {
    auto it = localFuncs_.find(f.name);
    if (it == localFuncs_.end()) return; // 签名阶段已报错
    // 泛型模板（存在未注解必填参数）本体不检查——C++ 模板语义：类型事实
    // 全部由调用点实例化产生，未被调用的模板体不做类型检查。
    if (it->second.hasUnannotated) return;
    checkFunctionBody(f, it->second, out);
}

void TypeChecker::checkFunctionBody(FuncDef& f, FuncSig& sig,
                                    ModuleSymbols& out) {
    // 上下文保存（重入）：单态化实例可在调用者体检查中途嵌套实例化，
    // 返回后调用者的返回类型/循环深度/return 收集必须原样恢复。
    TypePtr savedRet = std::move(fnRet_);
    auto savedPending = std::move(pendingReturns_);
    const int savedLoop = loopDepth_;

    fnRet_ = sig.ret;
    pendingReturns_.clear();
    scope_.push();
    loopDepth_ = 0;
    const bool prevAsync = inAsyncFn_;
    inAsyncFn_ = f.isAsync;
    const bool prevInFunc = inFunc_;
    inFunc_ = true; // defer 仅函数体顶层合法（模块顶层无退出点）
    const bool prevPure = inPureFn_;
    inPureFn_ = sig.isPure; // K 专项：@pure 函数体启用纯度强制
    for (size_t i = 0; i < f.params.size(); ++i) {
        if (!scope_.declare(f.params[i].name,
                            VarSym{sig.params[i], f.params[i].range})) {
            diag_.error(Errc::E2007_DuplicateDefinition, file_,
                        f.params[i].range,
                        "参数 '" + f.params[i].name + "' 重复");
        }
    }
    // 实例克隆体的缺省值补查：exprTypes_ 按节点指针缓存类型，克隆体的
    // 缺省值是独立 AST 节点（AOT 调用点缺省填充按 sig.def 取 AST 求类型）。
    if (sig.isMonoInst) {
        for (size_t i = 0; i < f.params.size(); ++i)
            if (f.params[i].defaultValue)
                checkParamDefault(f.params[i], sig.params[i]);
    }

    // 契约定义侧：类型必须为 bool；不可静态验证子句降级告警（W3001）。
    checkContractDefs(f);

    checkStmtList(f.body);

    // 容器占位收尾：仍未定型的空 List/Dict 在此报 E2002（后续 return
    // 推断据 reported 避免对同一根因重复诊断）。
    const bool unresolvedReported = reportUnresolvedContainers();

    // 返回类型推断（Q10）：统一全部 return 的值类型并回填签名。
    // 递归自引用（Unknown）跳过；数值混返回取宽化（int+float → float，
    // 与 isConvertible 的唯一隐式转换方向一致）；不一致即 E2006。
    if (sig.retPending) {
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
        // 空容器占位返回值（return []）无法定型：要求显式注解返回类型。
        if (!conflict && hasUnknownParam(inferred)) {
            if (!unresolvedReported)
                diag_.error(Errc::E2002_CannotInfer, file_, f.nameRange,
                            "无法推断函数 '" + f.name + "' 的返回类型（返回空容器）；"
                            "请显式注解返回类型（如 -> List[int]）");
            inferred = tyErr();
        }
        if (!conflict) {
            sig.ret = inferred;
            sig.retPending = false;
            out.funcs[f.name] = sig; // 导出同步（实例按实例符号名入注册表）
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
    fnRet_ = std::move(savedRet);
    pendingReturns_ = std::move(savedPending);
    loopDepth_ = savedLoop;
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
