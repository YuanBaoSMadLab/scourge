// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 类型检查器 · 语句检查
// ----------------------------------------------------------------------------
//  职责边界：checkStmtList / checkNested / checkStmt（赋值/分支/循环/断言/try）与 requireBool（无隐式真值转换门禁）。
//  其余阶段见同目录 TypeChecker*.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/Sema/TypeChecker.hpp"

#include <cmath>
#include <functional>
#include <set>

namespace scourge {

// optAssignable / tupleMismatch 自 0.9.10 起上收至 Sema/Type.hpp——
// 语句级（本文件）与模块级（TypeCheckerSym.cpp）检查共用，单一事实源。


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
        // Tensor 上下文偏好（单发旗标，F-18 A 案）：注解为 Tensor 或重赋值
        // 既有 Tensor 变量时，一维数值列表字面量按 Tensor 推断（默认已改为
        // List[T]；checkListLit 消费旗标，检查完立即清空）。
        if (a.value->kind == Expr::Kind::ListLit &&
            a.target->kind == Expr::Kind::Name) {
            auto& n0 = static_cast<const Name&>(*a.target);
            bool tensorCtx = a.ann && a.ann->kind == TypeExpr::Kind::Tensor;
            if (!tensorCtx) {
                if (const VarSym* pre = scope_.find(n0.name))
                    tensorCtx = pre->type->kind == Type::Kind::Tensor;
            }
            if (tensorCtx) asTensorLit_ = a.value.get();
        }
        // Dict/Set 上下文偏好（单发旗标，同 asTensorLit_）：注解为
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
        TypePtr tupleAnn;
        if (a.value->kind == Expr::Kind::TupleLit) {
            // Tuple 上下文偏好（0.9.9 批3①）：目标为元组注解（或既有元组
            // 变量）时 checkTupleLit 逐位对齐——int 元素遇 float 位打提升
            // 标记并改写推断位，AOT genTupleLit 按位消费（防位型错位）。
            if (a.ann) {
                tupleAnn = resolveTypeExpr(*a.ann);
                if (tupleAnn->kind == Type::Kind::Tuple)
                    tupleTarget_ = tupleAnn.get();
            } else if (a.target->kind == Expr::Kind::Name) {
                if (const VarSym* pre = scope_.find(
                        static_cast<const Name&>(*a.target).name))
                    if (pre->type->kind == Type::Kind::Tuple)
                        tupleTarget_ = pre->type.get();
            }
        }
        TypePtr vt = checkExpr(*a.value);
        asTensorLit_ = nullptr;
        dictTarget_ = nullptr;
        setTarget_ = nullptr;
        tupleTarget_ = nullptr;

        if (a.target->kind == Expr::Kind::Name) {
            auto& n = static_cast<const Name&>(*a.target);
            const VarSym* existing = scope_.find(n.name);
            if (a.ann) {
                TypePtr declared = resolveTypeExpr(*a.ann);
                if (vt->kind == Type::Kind::Int &&
                    declared->kind == Type::Kind::Float)
                    promoteF64_.insert(a.value.get()); // Q3 边界提升
                if (!isConvertible(vt, declared) &&
                    !optAssignable(vt, declared)) {
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
                if (!isConvertible(vt, existing->type) &&
                    !optAssignable(vt, existing->type)) {
                    std::string mm = tupleMismatch(vt, existing->type);
                    if (!mm.empty()) {
                        fail(Errc::E2001_TypeMismatch, s.range, mm);
                    } else {
                        fail(Errc::E2001_TypeMismatch, s.range,
                             "变量 '" + n.name + "' 已声明为 " +
                                 typeToString(existing->type) + "，不能赋 " +
                                 typeToString(vt));
                    }
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
        // Option 同为显式出口（0.9.9 批3）：空选项也是信息，丢弃多半是
        // 忘了消费；消息与 Result 对齐（is_some 对应 is_ok）。
        if (t->kind == Type::Kind::Option) {
            diag_.error(Errc::E2005_ResultNotConsumed, file_, es.expr->range,
                        "Option 值被丢弃；必须显式消费：绑定变量、用 "
                        "is_some() 判断，或 unwrap()/unwrap_or() 取值");
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
        TypePtr elem = iterElemOf(it);
        if (elem->kind == Type::Kind::Err && it->kind != Type::Kind::Err)
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
            if (!isConvertible(vt, fnRet_) && !optAssignable(vt, fnRet_)) {
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

} // namespace scourge
