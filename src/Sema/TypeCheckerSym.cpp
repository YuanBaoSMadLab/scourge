// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 类型检查器 · 模块顶层符号收集
// ----------------------------------------------------------------------------
//  职责边界：collectTopLevel——函数/全局/导入/capability 顶层符号预收集与重复定义检查（check 主流程的前置装配阶段）。
//  其余阶段见同目录 TypeChecker*.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/Sema/TypeChecker.hpp"

#include <cmath>
#include <functional>
#include <set>

namespace scourge {

// ============================================================================
// 模块级：收集顶层符号
// ============================================================================

// 模块顶层非法语句的处方消息（两处 switch 臂共用，单一事实源）：附 main()
// 骨架示例——F-3 入口仪式是 AI 起手最高频翻车点（顶层 print 必错），
// 示例直接可抄，报错即自解释。
static const char kTopLevelOnlyMsg[] =
    "模块顶层仅允许 def / import / 赋值 / capability"
    "（保证 .sgi 可静态生成；docstring 除外）；程序入口需"
    " def main() -> int，示例:\n"
    "  def main() -> int:\n"
    "      print(\"hi\")\n"
    "      return 0";

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
                    // 链接别名（@extern("dll", symbol="C名")）：查符号用
                    // 别名、Scourge 侧保留惯用名；双模后端与 VM 解析同源。
                    // 其余关键字参数一律拒绝（白名单外无隐式默认）。
                    bool sawSym = false;
                    for (const auto& kv : d.kwargs) {
                        if (kv.first != "symbol" ||
                            kv.second->kind != Expr::Kind::StrLit) {
                            diag_.error(
                                Errc::E1007_UnsupportedFeature, file_,
                                d.range,
                                "@extern 仅支持关键字参数 symbol=\"符号名\""
                                "（字符串，缺省 = 函数名）");
                            continue;
                        }
                        if (sawSym) {
                            diag_.error(Errc::E1007_UnsupportedFeature, file_,
                                        d.range,
                                        "@extern 的 symbol= 只能出现一次");
                            continue;
                        }
                        sawSym = true;
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
            // @extern 需在参数循环前裁决：无注解参数的处方消息与其余函数
            // 不同（FFI 调用约定要求完整签名，不走隐式单态化）。
            bool isExtern = false;
            for (const auto& d : f.decorators) isExtern |= (d.name == "extern");
            bool anyUnannotated = false;
            bool sawDefault = false;
            for (const auto& p : f.params) {
                bool unAnn = false;
                if (!p.type) {
                    if (isExtern) {
                        diag_.error(Errc::E2002_CannotInfer, file_, p.range,
                                    "@extern 函数参数 '" + p.name +
                                        "' 类型必须显式注解（FFI 调用约定"
                                        "需要完整签名）");
                        sig.params.push_back(tyErr());
                    } else {
                        // 批 9-③：参数注解可选——Unknown 占位成泛型模板，
                        // 调用点按实参类型隐式单态化（模块内调用）。
                        sig.params.push_back(tyUnknown());
                        unAnn = true;
                    }
                } else {
                    sig.params.push_back(resolveTypeExpr(*p.type));
                }
                // 缺省值边界 int→float 提升（Q3：与函数体注解赋值同规则）。
                if (p.defaultValue) {
                    // 缺省值即注解：未注解参数由缺省值定型（Python 式），
                    // 此后该参数不再是泛型位。
                    TypePtr dv = checkParamDefault(p, sig.params.back());
                    if (unAnn) sig.params.back() = dv;
                    sawDefault = true;
                } else if (sawDefault) {
                    diag_.error(Errc::E2001_TypeMismatch, file_, p.range,
                                "无默认值参数不能跟在有默认值参数之后");
                } else {
                    ++sig.requiredArgs;
                    anyUnannotated |= unAnn;
                }
            }
            sig.hasUnannotated = anyUnannotated;
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
            if (isExtern) {
                sig.isExtern = true;
                sig.isVariadic = f.isVariadic;
                // FFI 门禁记账（0.9.7）：@extern 即外部代码边界，恒需 ffi。
                capUses_.push_back({f.name, Cap::Ffi, f.nameRange});
                // getenv 处方化（F-2）：env 已是内建且缺失键返回 ""，行为
                // 与 C getenv 的 NULL 兜底一致——跨库声明反而引入双份依赖。
                if (f.name == "getenv")
                    diag_.error(Errc::E1007_UnsupportedFeature, file_,
                                f.nameRange,
                                "env 已是内建，无需 @extern——直接调用 "
                                "env(\"名称\")（缺失键返回 \"\"）");
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
                if (!scope_.declare(bind, VarSym{mod, s.range})) {
                    // F-19：同包多模块 import 合并命名空间——同名绑定已存在
                    // 且同包（首段相同）时宽容跳过（import std.autodiff 后再
                    // import std.nn 不再 E2007；兄弟模块成员经包根回退解析，
                    // 见 checkAttr/qualifiedModuleName/foldModuleKey）。跨包
                    // 同名仍拒绝（真冲突）。
                    size_t dot = qual.find('.');
                    std::string pkg =
                        dot == std::string::npos ? qual : qual.substr(0, dot);
                    const VarSym* ex = scope_.find(bind);
                    bool samePkg =
                        ex && ex->type &&
                        ex->type->kind == Type::Kind::Module &&
                        ex->type->name.substr(
                            0, ex->type->name.find('.')) == pkg;
                    if (!samePkg)
                        diag_.error(Errc::E2007_DuplicateDefinition, file_,
                                    s.range,
                                    "顶层名 '" + bind +
                                        "' 重复定义（import 绑定与既有名冲突；"
                                        "可用 as 别名或全限定形式）");
                }
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
                            if (cp.hasUnannotated) {
                                // 批 9-③ 边界：隐式单态化仅限定义模块内部
                                // ——跨模块走 .sgi 接口面，签名必须完整。
                                diag_.error(Errc::E2001_TypeMismatch, file_,
                                            s.range,
                                            "导入函数 '" + item.name +
                                                "' 存在未注解参数——隐式"
                                                "单态化仅限定义模块内部调用；"
                                                "请在其定义处显式标注参数类型");
                                continue;
                            }
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
            // Dict/Set 上下文偏好（单发旗标，同 asTensorLit_）：注解为
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
            // Tuple 上下文偏好（0.9.10）：注解为 Tuple 时字面量逐位对齐
            // （与语句级同构，防 AOT 位型错位）。
            TypePtr tupleAnn;
            if (a.value->kind == Expr::Kind::TupleLit && a.ann) {
                tupleAnn = resolveTypeExpr(*a.ann);
                if (tupleAnn->kind == Type::Kind::Tuple)
                    tupleTarget_ = tupleAnn.get();
            }
            // Tensor 上下文偏好（单发旗标，F-18 A 案，同语句级 asTensorLit_）：
            // 注解为 Tensor 时一维数值列表字面量按 Tensor 推断（默认已改为
            // List[T]；checkListLit 消费，检查完清空；模块级无既有变量
            // 重赋值路径）。
            if (a.value->kind == Expr::Kind::ListLit && a.ann &&
                a.ann->kind == TypeExpr::Kind::Tensor)
                asTensorLit_ = a.value.get();
            TypePtr vt = checkExpr(*a.value);
            asTensorLit_ = nullptr;
            dictTarget_ = nullptr;
            setTarget_ = nullptr;
            tupleTarget_ = nullptr;
            TypePtr declared;
            if (a.ann) {
                declared = resolveTypeExpr(*a.ann);
                if (vt->kind == Type::Kind::Int &&
                    declared->kind == Type::Kind::Float)
                    promoteF64_.insert(a.value.get()); // Q3 边界提升
                if (!isConvertible(vt, declared) &&
                    !optAssignable(vt, declared)) {
                    // 三分支与语句级一致（Type.hpp 单一事实源）。
                    std::string mm = tupleMismatch(vt, declared);
                    if (!mm.empty()) {
                        fail(Errc::E2001_TypeMismatch, a.value->range, mm);
                    } else if (declared->kind == Type::Kind::Option) {
                        fail(Errc::E2001_TypeMismatch, a.value->range,
                             "赋值类型不匹配：" + typeToString(vt) +
                                 " 无法赋给 " + typeToString(declared) +
                                 "；Option 目标接受 None 或 " +
                                 typeToString(declared->params[0]) +
                                 " 载荷（其余请用 ok() 显式构造）");
                    } else {
                        fail(Errc::E2001_TypeMismatch, a.value->range,
                             "赋值类型不匹配：" + typeToString(vt) +
                                 " 无法赋给 " + typeToString(declared));
                    }
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
                        kTopLevelOnlyMsg);
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
                        kTopLevelOnlyMsg);
        }
    }
}

// ============================================================================
// 缺省值检查（收集期与单态化实例补查共用）
// ============================================================================

TypePtr TypeChecker::checkParamDefault(const Param& p, const TypePtr& t) {
    // Dict/Set 上下文偏好：缺省值容器字面量对齐参数注解。
    if (p.defaultValue->kind == Expr::Kind::DictLit && t &&
        t->kind == Type::Kind::Dict && t->params.size() >= 2)
        dictTarget_ = t.get();
    if (p.defaultValue->kind == Expr::Kind::DictLit && t &&
        t->kind == Type::Kind::Set && t->params.size() >= 1)
        setTarget_ = t.get();
    if (p.defaultValue->kind == Expr::Kind::SetLit && t &&
        t->kind == Type::Kind::Set && t->params.size() >= 1)
        setTarget_ = t.get();
    TypePtr dv = checkExpr(*p.defaultValue);
    dictTarget_ = nullptr;
    setTarget_ = nullptr;
    // 边界 int→float 提升（Q3：与函数体注解赋值同规则）。
    if (t && dv->kind == Type::Kind::Int && t->kind == Type::Kind::Float)
        promoteF64_.insert(p.defaultValue.get());
    return dv;
}

} // namespace scourge
