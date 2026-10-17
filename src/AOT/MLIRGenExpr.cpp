// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT MLIR 生成器 · 表达式求值
// ----------------------------------------------------------------------------
//  静态 VK 推断（inferVK）、genExpr 大分派、名字/一元/二元/三元求值、
//  int→float 提升、张量静态形状判定（staticShapeOf/staticF64Shape）。
//  类型/形状只查检查器注解表，不在生成期重算推导。内部分块文件，
//  共享模型见 MLIRGenDetail.hpp。
// ============================================================================
#include "scourge/AOT/MLIRGenDetail.hpp"

namespace scourge {
namespace aot {
namespace mlirgen {

// ============================================================================
// 表达式
// ============================================================================

/// 静态 VK 推断（三元结果类型等）。推断不出 → Bad。
/// 类型/形状推导的单一事实源是检查器（Session::exprTypes() 注解表）：
/// 此处优先查表直取，未命中（Err/Unknown 不入表）才走本地保守回退，
/// 回退逻辑与检查器推导规则同构——不在生成期重算推导。
VK Gen::inferVK(const Expr& e) const {
    if (auto it = types_.find(&e); it != types_.end()) {
        VK vk = vkOfType(it->second);
        if (vk != VK::Bad) return vk;
    }
    switch (e.kind) {
    case Expr::Kind::IntLit: return VK::Int;
    case Expr::Kind::FloatLit: return VK::Float;
    case Expr::Kind::BoolLit: return VK::Bool;
    case Expr::Kind::NoneLit: return VK::None;
    case Expr::Kind::StrLit: return VK::Str;
    case Expr::Kind::Name: {
        auto& n = static_cast<const Name&>(e);
        auto lt = locals_.find(n.name);
        if (lt != locals_.end()) return lt->second.vk;
        if (curGlobals_) {
            auto gt = curGlobals_->find(n.name);
            if (gt != curGlobals_->end()) return gt->second.vk;
        }
        return VK::Bad;
    }
    case Expr::Kind::Unary: {
        auto& u = static_cast<const Unary&>(e);
        if (u.op == "not") return VK::Bool;
        return inferVK(*u.operand);
    }
    case Expr::Kind::Binary: {
        auto& b = static_cast<const Binary&>(e);
        const std::string& op = b.op;
        if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
            op == ">=" || op == "and" || op == "or" || op == "in")
            return VK::Bool;
        if (op == "@") return VK::Tensor;
        VK l = inferVK(*b.lhs), r = inferVK(*b.rhs);
        if (op == "+" && l == VK::Str && r == VK::Str) return VK::Str;
        if (op == "*" &&
            ((l == VK::Str && r == VK::Int) || (l == VK::Int && r == VK::Str)))
            return VK::Str;
        if (l == VK::Tensor || r == VK::Tensor) return VK::Tensor;
        if (!isNumVK(l) || !isNumVK(r)) return VK::Bad;
        if (op == "/") return VK::Float;
        return (l == VK::Float || r == VK::Float) ? VK::Float : VK::Int;
    }
    case Expr::Kind::Ternary: {
        auto& t = static_cast<const Ternary&>(e);
        VK a = inferVK(*t.thenE), b = inferVK(*t.elseE);
        if (a == b) return a;
        if (isNumVK(a) && isNumVK(b))
            return (a == VK::Float || b == VK::Float) ? VK::Float : VK::Int;
        return VK::Bad;
    }
    case Expr::Kind::Call: {
        auto& c = static_cast<const Call&>(e);
        if (c.callee->kind == Expr::Kind::Name) {
            auto& n = static_cast<const Name&>(*c.callee);
            if (isBuiltinName(n.name)) {
                if (n.name == "print") return VK::None;
                if (n.name == "str") return VK::Str;
                if (n.name == "len") return VK::Int;
                if (n.name == "now") return VK::Float;
                if (n.name == "range") return VK::Range;
                if (n.name == "abs") {
                    VK a = inferVK(*c.args[0]);
                    return a == VK::Float ? VK::Float : VK::Int;
                }
                if (n.name == "sqrt" || n.name == "exp" || n.name == "log")
                    return VK::Float;
                // v0.9.3 标量数学/随机/排序
                if (n.name == "sin" || n.name == "cos" || n.name == "tan" ||
                    n.name == "asin" || n.name == "acos" || n.name == "atan" ||
                    n.name == "sinh" || n.name == "cosh" || n.name == "tanh" ||
                    n.name == "atan2" || n.name == "random")
                    return VK::Float;
                if (n.name == "floor" || n.name == "ceil" ||
                    n.name == "round" || n.name == "trunc")
                    return VK::Int;
                if (n.name == "seed") return VK::None;
                if (n.name == "sorted") return VK::List;
                if (n.name == "ok" || n.name == "err") return VK::Result;
                return VK::Bad; // sum/max/min/transpose/reshape
            }
            auto ft = s_.registry().find(curModule_);
            if (ft != s_.registry().end()) {
                auto f = ft->second->funcs.find(n.name);
                if (f != ft->second->funcs.end()) return vkOfType(f->second.ret);
            }
            return VK::Bad;
        }
        if (c.callee->kind == Expr::Kind::Attr) {
            auto& at = static_cast<const Attr&>(*c.callee);
            if (at.base->kind == Expr::Kind::Name) {
                auto& mn = static_cast<const Name&>(*at.base);
                auto mt = s_.registry().find(mn.name);
                if (mt != s_.registry().end()) {
                    auto f = mt->second->funcs.find(at.name);
                    if (f != mt->second->funcs.end())
                        return vkOfType(f->second.ret);
                }
            }
        }
        return VK::Bad;
    }
    case Expr::Kind::Await:
        return inferVK(*static_cast<const Await&>(e).operand);
    case Expr::Kind::Index: {
        auto& ix = static_cast<const Index&>(e);
        VK b = inferVK(*ix.base);
        if (b == VK::Tensor) return VK::Tensor;
        if (b == VK::Tuple) return VK::Tuple; // 元素 VK 由类型表按位查取
        return VK::Bad;
    }
    case Expr::Kind::ListLit:
        return static_cast<const ListLit&>(e).checkedAsTensor ? VK::Tensor
                                                              : VK::Bad;
    case Expr::Kind::Attr: {
        auto& at = static_cast<const Attr&>(e);
        if (at.base->kind == Expr::Kind::Name) {
            auto& mn = static_cast<const Name&>(*at.base);
            auto gt = globals_.find(mn.name);
            if (gt != globals_.end()) {
                auto g = gt->second.find(at.name);
                if (g != gt->second.end()) return g->second.vk;
            }
        }
        return VK::Bad;
    }
    default:
        return VK::Bad;
    }
}

Value Gen::genExpr(const Expr& e) {
    line_ = lineOf(e.range); // panic 行号锚点（markLine 等价）
    switch (e.kind) {
    case Expr::Kind::IntLit:
        return Value{VK::Int, intConst(static_cast<const IntLit&>(e).value)};
    case Expr::Kind::FloatLit: {
        double d = static_cast<const FloatLit&>(e).value;
        std::string ssa = fresh();
        emitEntry(ssa + " = arith.constant " + fmtF64(d) + " : f64");
        return Value{VK::Float, ssa};
    }
    case Expr::Kind::BoolLit:
        return Value{VK::Bool, boolReg(static_cast<const BoolLit&>(e).boolValue)};
    case Expr::Kind::NoneLit:
        return Value{VK::None, ""};
    case Expr::Kind::StrLit:
        return strConst(static_cast<const StrLit&>(e).strValue);
    case Expr::Kind::Name:
        return genName(static_cast<const Name&>(e));
    case Expr::Kind::Unary:
        return genUnary(static_cast<const Unary&>(e));
    case Expr::Kind::Binary: {
        auto& b = static_cast<const Binary&>(e);
        if (b.op == "and" || b.op == "or") return genLogical(b);
        Value l = genExpr(*b.lhs);
        Value r = genExpr(*b.rhs);
        return genBinary(b.op, l, r, b.range, b.rhs.get(), b.lhs.get());
    }
    case Expr::Kind::Ternary:
        return genTernary(static_cast<const Ternary&>(e));
    case Expr::Kind::Call:
        return genCall(static_cast<const Call&>(e));
    case Expr::Kind::Attr: {
        auto& at = static_cast<const Attr&>(e);
        // 模块常量访问：m.CONST / a.b.CONST → 全局加载（绑定键经
        // foldModuleKey 还原真实全限定名，0.9.6 点分链）。
        std::string qm = foldModuleKey(*at.base);
        if (!qm.empty()) {
            Value v = loadGlobal(qm, at.name);
            if (v.vk == VK::Bad)
                unsupported(e.range,
                            "模块 '" + qm + "' 无常量 '" + at.name + "'");
            return v;
        }
        unsupported(e.range, "成员访问（Attr）");
    }
    case Expr::Kind::Await:
        // await 类型透传（Task 语义 P2）；VM 内同步执行，与 BC
        // CompilerExpr.cpp Await 分支同一语义（双模一致）。
        return genExpr(*static_cast<const Await&>(e).operand);
    case Expr::Kind::Index: {
        auto& ix = static_cast<const Index&>(e);
        // 切片 s[a:b]（0.9.9）：按基型静态分派 str_nslice/list_nslice，
        // ABI 5 参 [base,a,b,hasA,hasB]——缺省端补 (0, has=0)，与 VM
        // Slice2 同一 normSlice 公式（单一事实源，注释互指）。
        if (ix.isSlice) {
            Value base = genExpr(*ix.base);
            std::string ssa = fresh();
            // 两边界值 + 两 has 位按序发射。
            Value es[2], hs[2];
            for (int k = 0; k < 2; ++k) {
                const Expr* ex = ix.indices[static_cast<size_t>(k)].get();
                if (ex) {
                    es[k] = genExpr(*ex);
                    if (es[k].vk != VK::Int)
                        unsupported(ex->range, "切片边界必须是 int");
                    hs[k] = Value{VK::Int, intConst(1)};
                } else {
                    es[k] = Value{VK::Int, intConst(0)};
                    hs[k] = Value{VK::Int, intConst(0)};
                }
            }
            if (base.vk == VK::Str) {
                emit(ssa + " = \"lang.str_nslice\"(" + base.ssa + ", " +
                     es[0].ssa + ", " + es[1].ssa + ", " + hs[0].ssa + ", " +
                     hs[1].ssa + ") : (!llvm.ptr, i64, i64, i64, i64) -> "
                     "!llvm.ptr " + opLoc());
                return Value{VK::Str, ssa};
            }
            if (base.vk == VK::List) {
                emit(ssa + " = \"lang.list_nslice\"(" + base.ssa + ", " +
                     es[0].ssa + ", " + es[1].ssa + ", " + hs[0].ssa + ", " +
                     hs[1].ssa + ") : (!llvm.ptr, i64, i64, i64, i64) -> "
                     "!llvm.ptr " + opLoc());
                return Value{VK::List, ssa, false, 0, base.lElem};
            }
            unsupported(ix.range, "仅 str 与 List 支持 s[a:b] 切片");
        }
        Value base = genExpr(*ix.base);
        if (base.vk == VK::List) {
            // List 单下标（检查器 E2004 已拒多下标）：list_get_* 内做
            // normIndex（负下标归一 + 越界 panic，消息与 VM 逐字一致）
            if (ix.indices.size() != 1)
                unsupported(ix.range, "List 只支持单下标");
            Value iv = genExpr(*ix.indices[0]);
            if (iv.vk != VK::Int)
                unsupported(ix.indices[0]->range, "列表下标必须是 int");
            std::string ssa = fresh();
            switch (static_cast<VK>(base.lElem)) {
            case VK::Int:
                emit(ssa + " = \"lang.list_get_i\"(" + base.ssa + ", " + iv.ssa +
                     ") : (!llvm.ptr, i64) -> i64 " + opLoc());
                return Value{VK::Int, ssa};
            case VK::Float:
                emit(ssa + " = \"lang.list_get_f\"(" + base.ssa + ", " + iv.ssa +
                     ") : (!llvm.ptr, i64) -> f64 " + opLoc());
                return Value{VK::Float, ssa};
            case VK::Bool:
                emit(ssa + " = \"lang.list_get_b\"(" + base.ssa + ", " + iv.ssa +
                     ") : (!llvm.ptr, i64) -> i1 " + opLoc());
                return Value{VK::Bool, ssa};
            case VK::Str:
                emit(ssa + " = \"lang.list_get_s\"(" + base.ssa + ", " + iv.ssa +
                     ") : (!llvm.ptr, i64) -> !llvm.ptr " + opLoc());
                return Value{VK::Str, ssa};
            case VK::Tuple:
                // 0.9.9：items() 元组槽——i64 列表槽存元组句柄位（指针位
                // 型），inttoptr 还原为 genTupleLit 同形态句柄值
                emit(ssa + " = \"lang.list_get_i\"(" + base.ssa + ", " + iv.ssa +
                     ") : (!llvm.ptr, i64) -> i64 " + opLoc());
                {
                    std::string h = fresh();
                    emit(h + " = llvm.inttoptr " + ssa + " : i64 to !llvm.ptr");
                    return Value{VK::Tuple, h};
                }
            default:
                unsupported(ix.base->range, "该元素类型的列表下标读取");
            }
        }
        if (base.vk == VK::Dict) {
            // Dict 单下标（检查器 E2004 已拒多下标）：键 → 64 位槽，缺键
            // panic 消息与 VM 逐字一致；返回值按 base 注解 params[1] 还原
            // （Index 节点自身注解为元素类型，Dict 类型挂在 base 上）。
            if (ix.indices.size() != 1)
                unsupported(ix.range, "dict 只支持单下标");
            Value k = genExpr(*ix.indices[0]);
            // float 键字典接受 int 键（检查器 isConvertible 放行；VM 侧
            // hashKey/valuesEqual 数值跨类型同键）——int 键插提升对齐槽位。
            auto bty = types_.find(ix.base.get());
            if (k.vk == VK::Int && bty != types_.end() &&
                bty->second->kind == Type::Kind::Dict &&
                bty->second->params.size() >= 2 &&
                bty->second->params[0]->kind == Type::Kind::Float)
                k = promoteF64(k);
            std::string kbits = dictToBits(k);
            std::string raw = fresh();
            emit(raw + " = \"lang.dict_get\"(" + base.ssa + ", " + kbits +
                 ") : (!llvm.ptr, i64) -> i64 " + opLoc());
            VK vVK = VK::Bad;
            if (bty != types_.end() && bty->second->kind == Type::Kind::Dict &&
                bty->second->params.size() >= 2)
                vVK = vkOfType(bty->second->params[1]);
            if (vVK == VK::Bad)
                unsupported(ix.range, "该值类型的字典下标读取");
            return dictFromBits(vVK, raw);
        }
        if (base.vk == VK::Str) {
            // str 单下标（0.9.7 补齐：检查器早已放行，VM 可用而 AOT 缺）。
            // 按字节索引（P1 语义），负下标归一 + 越界 panic 消息与 VM
            // normIndex 逐字一致（sg_str_chr_at 与 for 遍历共用）。
            if (ix.indices.size() != 1)
                unsupported(ix.range, "str 只支持单下标");
            Value iv = genExpr(*ix.indices[0]);
            if (iv.vk != VK::Int)
                unsupported(ix.indices[0]->range, "str 下标必须是 int");
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_chr_at\"(" + base.ssa + ", " + iv.ssa +
                 ") : (!llvm.ptr, i64) -> !llvm.ptr " + opLoc());
            return Value{VK::Str, ssa};
        }
        if (base.vk == VK::Tuple) {
            // 元组单下标（0.9.6）：检查器已强制单下标 + 编译期整数字面量 +
            // 静态越界；此处按类型表 base 注解的 params[i] 还原元素。
            auto bty = types_.find(ix.base.get());
            if (ix.indices.size() != 1 || bty == types_.end() ||
                bty->second->kind != Type::Kind::Tuple ||
                ix.indices[0]->kind != Expr::Kind::IntLit)
                unsupported(ix.range, "元组下标须为编译期整数字面量");
            const int64_t i = static_cast<const IntLit&>(*ix.indices[0]).value;
            const auto& ps = bty->second->params;
            if (i < 0 || static_cast<size_t>(i) >= ps.size())
                unsupported(ix.range, "元组下标越界");
            return genTupleElem(base, i, ps[static_cast<size_t>(i)], ix.range);
        }
        if (base.vk != VK::Tensor)
            unsupported(ix.base->range, "下标读取仅支持张量与列表");
        // 下标逐个求值（与 Compiler 的 base → idxs 求值序一致）
        std::vector<Value> idxs;
        for (const auto& ie : ix.indices) {
            Value iv = genExpr(*ie);
            if (iv.vk != VK::Int)
                unsupported(ie->range, "张量下标必须是 int");
            idxs.push_back(iv);
        }
        std::string arr = buildI64Array(idxs);
        std::string ssa = fresh();
        if (idxs.size() == base.tRank) { // 全秩 → 标量读取
            if (base.tInt)
                emit(ssa + " = \"lang.tensor_get_i\"(" + base.ssa + ", " +
                     intConst((int64_t)idxs.size()) + ", " + arr +
                     ") : (!llvm.ptr, i64, !llvm.ptr) -> i64 " + opLoc());
            else
                emit(ssa + " = \"lang.tensor_get_f\"(" + base.ssa + ", " +
                     intConst((int64_t)idxs.size()) + ", " + arr +
                     ") : (!llvm.ptr, i64, !llvm.ptr) -> f64 " + opLoc());
            return Value{base.tInt ? VK::Int : VK::Float, ssa};
        }
        // 部分索引 → 子张量切片（n > 秩时由运行时 panic，消息与 VM 一致）
        emit(ssa + " = \"lang.tensor_slice\"(" + base.ssa + ", " +
             intConst((int64_t)idxs.size()) + ", " + arr +
             ") : (!llvm.ptr, i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
        uint8_t outRank = idxs.size() < base.tRank
                              ? static_cast<uint8_t>(base.tRank - idxs.size())
                              : uint8_t{0};
        return Value{VK::Tensor, ssa, base.tInt, outRank};
    }
    case Expr::Kind::ListLit: {
        auto& ls = static_cast<const ListLit&>(e);
        if (!ls.checkedAsTensor) return genListLit(ls);
        return genTensorLit(ls);
    }
    case Expr::Kind::ListComp:
        return genListComp(static_cast<const ListComp&>(e));
    case Expr::Kind::DictLit: {
        auto& dl = static_cast<const DictLit&>(e);
        // 检查器判为空集合（Set[T] 注解上下文的空 {}）→ set_new(0)
        //（单一事实源：类型表此节点为 Set 类型，与 checkedAsTensor 同架构）。
        if (auto it = types_.find(&dl);
            it != types_.end() && it->second->kind == Type::Kind::Set)
            return genSetLitEmpty(dl, it->second);
        return genDictLit(dl);
    }
    case Expr::Kind::SetLit:
        return genSetLit(static_cast<const SetLit&>(e));
    case Expr::Kind::TupleLit:
        return genTupleLit(static_cast<const TupleLit&>(e));
    }
    unsupported(e.range, "该表达式");
}

Value Gen::genName(const Name& n) {
    // 渐进式局部槽：读取先局部后全局（与 Compiler 语义一致）。
    auto lt = locals_.find(n.name);
    if (lt != locals_.end()) return loadSlot(lt->second);
    // from-import 全局别名（0.9.6）：跨模块重定向到源模块全局。
    if (auto fg = curFromGlobals_.find(n.name); fg != curFromGlobals_.end()) {
        Value v = loadGlobal(fg->second.first, fg->second.second);
        if (v.vk == VK::Bad)
            unsupported(n.range, "模块 '" + fg->second.first + "' 无全局 '" +
                                     fg->second.second + "'");
        return v;
    }
    if (curGlobals_) {
        auto gt = curGlobals_->find(n.name);
        if (gt != curGlobals_->end()) {
            if (gt->second.vk == VK::None) return Value{VK::None, ""};
            std::string p = fresh();
            emit(p + " = llvm.mlir.addressof " + gt->second.sym + " : !llvm.ptr");
            std::string ssa = fresh();
            emit(ssa + " = llvm.load " + p + " : !llvm.ptr -> " +
                 mlirTy(gt->second.vk));
            // 容器/句柄元数据回填（与 loadGlobal 同一契约）
            const GlobalSlot& g = gt->second;
            return Value{g.vk, ssa, g.tInt, g.tRank, g.lElem};
        }
    }
    unsupported(n.range, "内建函数 / 模块名不能作为值使用");
}

Value Gen::genUnary(const Unary& u) {
    Value v = genExpr(*u.operand);
    if (u.op == "not") {
        std::string ssa = fresh();
        emit(ssa + " = arith.xori " + v.ssa + ", " + boolReg(true) + " : i1");
        return Value{VK::Bool, ssa};
    }
    if (u.op == "-") {
        if (v.vk == VK::Int) {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.negi\"(" + v.ssa + ") : (i64) -> i64 " +
                 opLoc());
            return Value{VK::Int, ssa};
        }
        if (v.vk == VK::Float) {
            std::string ssa = fresh();
            emit(ssa + " = arith.negf " + v.ssa + " : f64");
            return Value{VK::Float, ssa};
        }
        unsupported(u.range, "取负不支持该类型");
    }
    if (u.op == "+") {
        if (v.vk == VK::Int || v.vk == VK::Float) return v; // 原样
        unsupported(u.range, "一元 '+' 不支持该类型");
    }
    unsupported(u.range, "未知一元运算符 '" + u.op + "'");
}

Value Gen::promoteF64(Value v) {
    if (v.vk == VK::Float) return v;
    std::string ssa = fresh();
    emit(ssa + " = arith.sitofp " + v.ssa + " : i64 to f64");
    return Value{VK::Float, ssa};
}

std::optional<std::vector<int64_t>>
Gen::staticShapeOf(const Expr* e) const {
    if (!e) return std::nullopt;
    auto it = types_.find(e);
    if (it == types_.end() || it->second->kind != Type::Kind::Tensor)
        return std::nullopt;
    const Type& t = *it->second;
    // 仅 int/float 元素进入覆盖集（str 等其他元素张量兜底 sg_*）
    if (t.params.empty() || (t.params[0]->kind != Type::Kind::Float &&
                             t.params[0]->kind != Type::Kind::Int))
        return std::nullopt;
    for (int64_t d : t.shape)
        if (d == kDyn) return std::nullopt;
    // 0-d 张量不在覆盖集（shapeArrAttr 空形状文本非法；当前语言亦不可达，
    // 全量索引得标量、部分索引秩 ≥ 1——守卫保留为防御）
    if (t.shape.empty()) return std::nullopt;
    return t.shape;
}

std::optional<std::vector<int64_t>>
Gen::staticF64Shape(const Expr* e) const {
    auto sh = staticShapeOf(e);
    if (!sh) return std::nullopt;
    if (types_.find(e)->second->params[0]->kind != Type::Kind::Float)
        return std::nullopt;
    return sh;
}

std::string Gen::shapeArrAttr(const std::vector<int64_t>& s) {
    std::string out = "array<i64: ";
    for (size_t k = 0; k < s.size(); ++k) {
        if (k) out += ", ";
        out += std::to_string(s[k]);
    }
    out += ">";
    return out;
}

/// 元组 == 的编译期逐元素展开（0.9.6）。检查器 tupleElemsComparable 已限
/// 两侧同长且逐位可比；元素经 genTupleElem 从位槽还原后按 VM valuesEqual
/// 语义比较：int×int 位整比较，其余数值族（含 bool）提升 f64 比较（bool
/// 先 extui i64 对齐槽位），双 str 走 str_cmp，双 None 恒真，None 对异类
/// 恒假，嵌套元组递归展开。空元组恒真（长度已由检查器保证相等）。
Value Gen::genTupleEq(Value l, Value r, const TypePtr& lt, const TypePtr& rt,
                      const SourceRange& loc) {
    const auto& lps = lt->params;
    const auto& rps = rt->params;
    if (lps.empty()) return Value{VK::Bool, boolReg(true)};
    Value acc;
    for (size_t k = 0; k < lps.size(); ++k) {
        Value le = genTupleElem(l, static_cast<int64_t>(k), lps[k], loc);
        Value re = genTupleElem(r, static_cast<int64_t>(k), rps[k], loc);
        Value e;
        if (le.vk == VK::None || re.vk == VK::None) {
            e = Value{VK::Bool,
                      boolReg(le.vk == VK::None && re.vk == VK::None)};
        } else if (le.vk == VK::Str && re.vk == VK::Str) {
            std::string c = fresh();
            emit(c + " = \"lang.str_cmp\"(" + le.ssa + ", " + re.ssa +
                 ") : (!llvm.ptr, !llvm.ptr) -> i64 " + opLoc());
            std::string b = fresh();
            emit(b + " = arith.cmpi eq, " + c + ", " + zeroI64() + " : i64");
            e = Value{VK::Bool, b};
        } else if (le.vk == VK::Tuple || re.vk == VK::Tuple) {
            e = genTupleEq(le, re, lps[k], rps[k], loc);
        } else {
            // 数值族：bool 元素先 extui 对齐 i64 槽位（VM asDouble 语义）。
            if (le.vk == VK::Bool) {
                std::string z = fresh();
                emit(z + " = arith.extui " + le.ssa + " : i1 to i64");
                le = Value{VK::Int, z};
            }
            if (re.vk == VK::Bool) {
                std::string z = fresh();
                emit(z + " = arith.extui " + re.ssa + " : i1 to i64");
                re = Value{VK::Int, z};
            }
            if (le.vk == VK::Int && re.vk == VK::Int) {
                std::string b = fresh();
                emit(b + " = arith.cmpi eq, " + le.ssa + ", " + re.ssa +
                     " : i64");
                e = Value{VK::Bool, b};
            } else if (isNumVK(le.vk) && isNumVK(re.vk)) {
                Value lf = promoteF64(le), rf = promoteF64(re);
                std::string b = fresh();
                emit(b + " = arith.cmpf oeq, " + lf.ssa + ", " + rf.ssa +
                     " : f64");
                e = Value{VK::Bool, b};
            } else {
                unsupported(loc, "该元素类型的元组相等比较");
            }
        }
        if (k == 0) {
            acc = e;
        } else {
            std::string b = fresh();
            emit(b + " = arith.andi " + acc.ssa + ", " + e.ssa + " : i1");
            acc = Value{VK::Bool, b};
        }
    }
    return acc;
}

Value Gen::genLogical(const Binary& b) {
    // and/or 短路求值（与 VM 语义一致：右侧仅在需要时求值——含 panic
    // 副作用的表达式两侧差异即双模分歧，cf 块实现，模式同 genTernary）。
    bool isAnd = b.op == "and";
    Value cond = genExpr(*b.lhs);
    Slot slot = allocSlot(VK::Bool);
    int thenB = newBlock(), elseB = newBlock(), joinB = newBlock();
    terminate("cf.cond_br " + cond.ssa + ", " + blk(thenB).label + ", " +
              blk(elseB).label);
    cur_ = thenB;
    {
        if (isAnd) {
            Value v = genExpr(*b.rhs);
            storeTo(slot, v);
        } else {
            std::string t = fresh();
            emit(t + " = arith.constant 1 : i1");
            storeTo(slot, Value{VK::Bool, t});
        }
    }
    terminate("cf.br " + blk(joinB).label);
    cur_ = elseB;
    {
        if (isAnd) {
            std::string f = fresh();
            emit(f + " = arith.constant 0 : i1");
            storeTo(slot, Value{VK::Bool, f});
        } else {
            Value v = genExpr(*b.rhs);
            storeTo(slot, v);
        }
    }
    terminate("cf.br " + blk(joinB).label);
    cur_ = joinB;
    return loadSlot(slot);
}

Value Gen::genBinary(const std::string& op, Value l, Value r,
                     const SourceRange& range, const Expr* rExpr,
                     const Expr* lExpr) {
    // ---- 逻辑（两侧恒为 bool） ----
    if (op == "and") {
        std::string ssa = fresh();
        emit(ssa + " = arith.andi " + l.ssa + ", " + r.ssa + " : i1");
        return Value{VK::Bool, ssa};
    }
    if (op == "or") {
        std::string ssa = fresh();
        emit(ssa + " = arith.ori " + l.ssa + ", " + r.ssa + " : i1");
        return Value{VK::Bool, ssa};
    }

    // ---- 成员测试：str in str / k in List[T]（v0.9.3；与 VM valuesEqual
    // 一致——int 键遇 float 列表提升 f64 比较，bool/str 严格同类） ----
    if (op == "in") {
        if (l.vk == VK::Str && r.vk == VK::Str) {
            // C 运行时返回 int（i32），与 ABI 一致；调用方转回 i1 布尔。
            // 参数序与 VM 的 r.s.find(l.s) 对齐：hay = 右操作数。
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_contains\"(" + r.ssa + ", " + l.ssa +
                 ") : (!llvm.ptr, !llvm.ptr) -> i32 " + opLoc());
            std::string z = fresh();
            emit(z + " = arith.constant 0 : i32");
            std::string b = fresh();
            emit(b + " = arith.cmpi ne, " + ssa + ", " + z + " : i32");
            return Value{VK::Bool, b};
        }
        if (r.vk == VK::List) {
            // 键类型由检查器约束（isConvertible(l, elem)）；此处仅按列表
            // 元素 VK 分派，int 键遇 float 列表先提升（int→float 唯一放宽）。
            switch (static_cast<VK>(r.lElem)) {
            case VK::Int: {
                if (l.vk != VK::Int)
                    unsupported(range, "'in' 键类型与列表元素不符");
                std::string ssa = fresh();
                emit(ssa + " = \"lang.list_in_i\"(" + r.ssa + ", " + l.ssa +
                     ") : (!llvm.ptr, i64) -> i1");
                return Value{VK::Bool, ssa};
            }
            case VK::Float: {
                Value k = l.vk == VK::Int ? promoteF64(l) : l;
                std::string ssa = fresh();
                emit(ssa + " = \"lang.list_in_f\"(" + r.ssa + ", " + k.ssa +
                     ") : (!llvm.ptr, f64) -> i1");
                return Value{VK::Bool, ssa};
            }
            case VK::Bool: {
                std::string ssa = fresh();
                emit(ssa + " = \"lang.list_in_b\"(" + r.ssa + ", " + l.ssa +
                     ") : (!llvm.ptr, i1) -> i1");
                return Value{VK::Bool, ssa};
            }
            case VK::Str: {
                std::string ssa = fresh();
                emit(ssa + " = \"lang.list_in_s\"(" + r.ssa + ", " + l.ssa +
                     ") : (!llvm.ptr, !llvm.ptr) -> i1");
                return Value{VK::Bool, ssa};
            }
            default:
                unsupported(range, "该元素类型的列表成员测试");
            }
        }
        if (r.vk == VK::Dict) {
            // 键成员判定：键 → 64 位槽后 dict_in（哈希查找，与 VM find 一致）。
            // float 键字典的 int 键先提升（与下标读取同规则，VM 数值跨类型同键）。
            if (l.vk == VK::Int && rExpr) {
                if (auto it = types_.find(rExpr);
                    it != types_.end() && it->second->kind == Type::Kind::Dict &&
                    it->second->params.size() >= 2 &&
                    it->second->params[0]->kind == Type::Kind::Float)
                    l = promoteF64(l);
            }
            std::string kbits = dictToBits(l);
            std::string ssa = fresh();
            emit(ssa + " = \"lang.dict_in\"(" + r.ssa + ", " + kbits +
                 ") : (!llvm.ptr, i64) -> i1");
            return Value{VK::Bool, ssa};
        }
        if (r.vk == VK::Set) {
            // 元素成员判定：元素 → 64 位槽后 set_in（哈希查找，与 VM find
            // 一致）。float 集合的 int 元素先提升（VM 数值跨类型同键）。
            if (l.vk == VK::Int && rExpr) {
                if (auto it = types_.find(rExpr);
                    it != types_.end() && it->second->kind == Type::Kind::Set &&
                    it->second->params.size() >= 1 &&
                    it->second->params[0]->kind == Type::Kind::Float)
                    l = promoteF64(l);
            }
            std::string kbits = dictToBits(l);
            std::string ssa = fresh();
            emit(ssa + " = \"lang.set_in\"(" + r.ssa + ", " + kbits +
                 ") : (!llvm.ptr, i64) -> i1");
            return Value{VK::Bool, ssa};
        }
        unsupported(range, "'in' 仅支持 str/list/dict/Set 右操作数");
    }

    // ---- 比较（谓词查 kCmpOps 三族） ----
    if (const CmpInfo* cop = findCmpOp(op)) {
        if (l.vk == VK::Tensor || r.vk == VK::Tensor)
            unsupported(range, "张量比较运算");
        // None：常量折叠（VM valuesEqual：None==None → true，其余 false）；
        // != 为 valuesEqual 取反（VM Ne 同一实现）。
        if (l.vk == VK::None || r.vk == VK::None) {
            bool same = l.vk == VK::None && r.vk == VK::None;
            bool res = op == "!=" ? !same : same;
            return Value{VK::Bool, boolReg(res)};
        }
        // 元组 ==/!=（0.9.6）：编译期逐元素展开（检查器已限同构同长可比；
        // 嵌套元组递归）。!= 对 == 结果取反（与 VM valuesEqual 互补一致）。
        if (l.vk == VK::Tuple || r.vk == VK::Tuple) {
            auto ltIt = lExpr ? types_.find(lExpr) : types_.end();
            auto rtIt = rExpr ? types_.find(rExpr) : types_.end();
            if (ltIt == types_.end() || rtIt == types_.end() ||
                ltIt->second->kind != Type::Kind::Tuple ||
                rtIt->second->kind != Type::Kind::Tuple)
                unsupported(range, "元组比较的类型注解");
            Value eqv = genTupleEq(l, r, ltIt->second, rtIt->second, range);
            if (op == "!=") {
                std::string nb = fresh();
                emit(nb + " = arith.xori " + eqv.ssa + ", " + boolReg(true) +
                     " : i1");
                return Value{VK::Bool, nb};
            }
            return eqv;
        }
        if (l.vk == VK::Str && r.vk == VK::Str) {
            std::string s = fresh();
            emit(s + " = \"lang.str_cmp\"(" + l.ssa + ", " + r.ssa +
                 ") : (!llvm.ptr, !llvm.ptr) -> i64 " + opLoc());
            std::string b = fresh();
            emit(b + " = arith.cmpi " + cop->s + ", " + s + ", " + zeroI64() +
                 " : i64");
            return Value{VK::Bool, b};
        }
        if (l.vk == VK::Int && r.vk == VK::Int) {
            std::string b = fresh();
            emit(b + " = arith.cmpi " + cop->s + ", " + l.ssa + ", " + r.ssa +
                 " : i64");
            return Value{VK::Bool, b};
        }
        if (l.vk == VK::Bool && r.vk == VK::Bool) {
            // VM 按数值比较（asDouble）——0/1 的无符号整数比较等价。
            std::string b = fresh();
            emit(b + " = arith.cmpi " + cop->u + ", " + l.ssa + ", " + r.ssa +
                 " : i1");
            return Value{VK::Bool, b};
        }
        // 数值混合（int/float）：提升 f64；Ne 用 une（NaN != NaN 与 VM 一致）。
        Value lf = promoteF64(l), rf = promoteF64(r);
        std::string b = fresh();
        emit(b + " = arith.cmpf " + cop->f + ", " + lf.ssa + ", " + rf.ssa +
             " : f64");
        return Value{VK::Bool, b};
    }

    // ---- 字符串运算 ----
    if (op == "+" && l.vk == VK::Str && r.vk == VK::Str) {
        std::string ssa = fresh();
        emit(ssa + " = \"lang.str_concat\"(" + l.ssa + ", " + r.ssa +
             ") : (!llvm.ptr, !llvm.ptr) -> !llvm.ptr " + opLoc());
        Value v{VK::Str, ssa};
        v.owned = true; // concat 产出独占新缓冲（sg_str_alloc）
        return v;
    }
    if (op == "*" &&
        ((l.vk == VK::Str && r.vk == VK::Int) ||
         (l.vk == VK::Int && r.vk == VK::Str))) {
        Value s = l.vk == VK::Str ? l : r;
        Value n = l.vk == VK::Str ? r : l;
        std::string ssa = fresh();
        emit(ssa + " = \"lang.str_repeat\"(" + s.ssa + ", " + n.ssa +
             ") : (!llvm.ptr, i64) -> !llvm.ptr " + opLoc());
        Value v{VK::Str, ssa};
        v.owned = true; // repeat 产出独占新缓冲
        return v;
    }

    // ---- 张量运算（@ 与算术广播；语义与 VM tensorBinOp / tensorMatMul 一致） ----
    // op 编码查 kArithOps（与 C 运行时 sg_tiop/sg_tfop 一致）。
    const OpInfo* aop = findArithOp(op);
    const int tOp = aop ? aop->tCode : -1;
    if (op == "@" ||
        (tOp >= 0 && (l.vk == VK::Tensor || r.vk == VK::Tensor))) {
        if (op == "@") {
            if (l.vk != VK::Tensor || r.vk != VK::Tensor)
                unsupported(range, "'@' 需要两个张量操作数");
            // linalg 覆盖集：双 f64 + 两侧形状全静态 + 内维一致（内维不匹配
            // 保留兜底 sg_*，运行时 panic 语义与 VM 零分叉）。
            // GPU 覆盖判定属性：dtype 恒写（GpuCover covers 依赖；动态形状
            // matmul 由运行时 try 守卫校验秩/内维）；shape 属性仅双侧静态 +
            // 内维相等时写（linalg 降级门槛，GPU 路径不依赖它）。
            std::string attrs =
                " {lhs_int = " + std::to_string(l.tInt ? 1 : 0) + " : i32" +
                ", rhs_int = " + std::to_string(r.tInt ? 1 : 0) + " : i32";
            {
                auto lShape = staticF64Shape(lExpr);
                auto rShape = staticF64Shape(rExpr);
                if (lShape && rShape && (*lShape)[1] == (*rShape)[0])
                    attrs += ", lhs_shape = " + shapeArrAttr(*lShape) +
                             ", rhs_shape = " + shapeArrAttr(*rShape);
            }
            attrs += "}";
            std::string ssa = fresh();
            emit(ssa + " = \"lang.tensor_matmul\"(" + l.ssa + ", " + r.ssa +
                 ")" + attrs + " : (!llvm.ptr, !llvm.ptr) -> !llvm.ptr " +
                 opLoc());
            return Value{VK::Tensor, ssa, l.tInt && r.tInt, 2};
        }
        if (l.vk == VK::Tensor && r.vk == VK::Tensor) {
            // 张量降级覆盖集：+ - * / // % **（0-6）全 dtype 组合、形状全
            // 静态。双侧都必须静态——检查器 unifyShape 允许「静态⊕动态」
            // 统一成静态结果，只查单侧会把动态张量误当静态降级（输入桥
            // 读错内存）；int 溢出/除零 panic 由逐元素 lang 标量 op 调用
            // 保持零分叉（sg_add_i64 等 == sg_tiop 同一实现）。
            // GPU 覆盖判定属性：kind/dtype 恒写（GpuCover covers 依赖，
            // 动态形状由运行时 try 守卫校验）；shape 仅双侧静态相等时写
            // （linalg/affine 降级门槛——两 pass 合法性回调以"缺 shape
            // 属性 = 合法"兜底，GPU 路径不依赖它）。
            std::string attrs;
            if (tOp >= 0) {
                attrs = " {kind = " + std::to_string(tOp) + " : i32" +
                        ", lhs_int = " + std::to_string(l.tInt ? 1 : 0) +
                        " : i32, rhs_int = " + std::to_string(r.tInt ? 1 : 0) +
                        " : i32";
                auto lShape = staticShapeOf(lExpr);
                auto rShape = staticShapeOf(rExpr);
                if (lShape && rShape && *lShape == *rShape)
                    attrs += ", shape = " + shapeArrAttr(*lShape);
                attrs += "}";
            }
            std::string ssa = fresh();
            emit(ssa + " = \"lang.tensor_binop_tt\"(" + i32Const(tOp) + ", " +
                 l.ssa + ", " + r.ssa + ")" + attrs +
                 " : (i32, !llvm.ptr, !llvm.ptr) -> !llvm.ptr " + opLoc());
            // 结果 dtype 规则与 VM tensorBinOp 共用 DType 表；形状相等秩不变
            return Value{VK::Tensor, ssa,
                         dtypeElementwiseIsInt(tOp == 3, l.tInt, r.tInt),
                         l.tRank};
        }
        Value sc = l.vk == VK::Tensor ? r : l;
        if (!isNumVK(sc.vk))
            unsupported(range, "运算符 '" + op + "' 的标量操作数类型");
        // 标量双寄存器（vi/vf）按静态类型准备；bool 按 int 处理（VM 规则）
        std::string vi, vf;
        if (sc.vk == VK::Float) {
            vi = zeroI64();
            vf = sc.ssa;
        } else if (sc.vk == VK::Bool) {
            std::string z = fresh();
            emit(z + " = arith.extui " + sc.ssa + " : i1 to i64");
            vi = z;
            std::string f = fresh();
            emit(f + " = arith.uitofp " + sc.ssa + " : i1 to f64");
            vf = f;
        } else {
            vi = sc.ssa;
            std::string f = fresh();
            emit(f + " = arith.sitofp " + sc.ssa + " : i64 to f64");
            vf = f;
        }
        std::string si = i32Const(sc.vk != VK::Float ? 1 : 0);
        Value t = l.vk == VK::Tensor ? l : r;
        // 张量降级覆盖集：+ - * / // % **（0-6）全 dtype 组合。GPU 覆盖判
        // 定属性：kind/dtype 恒写（covers 依赖，动态形状运行时守卫）；shape
        // 仅张量侧静态时写（linalg/affine 降级门槛）。t_int/s_int 告知降
        // 级 pass 张量元素与标量槽位（vi/sf）的 dtype；float 张量 × int 标
        // 量走 vf（MLIRGen 已 sitofp），int 张量 × float 标量降级处 sitofp
        // 元素。（ts 查左表达式、st 查右表达式——张量所在侧）
        std::string attrs;
        if (tOp >= 0) {
            attrs = " {kind = " + std::to_string(tOp) + " : i32" +
                    ", t_int = " + std::to_string(t.tInt ? 1 : 0) + " : i32" +
                    ", s_int = " + std::to_string(sc.vk != VK::Float ? 1 : 0) +
                    " : i32";
            if (auto sh = staticShapeOf(l.vk == VK::Tensor ? lExpr : rExpr))
                attrs += ", shape = " + shapeArrAttr(*sh);
            attrs += "}";
        }
        std::string ssa = fresh();
        if (l.vk == VK::Tensor) { // 张量 op 标量 → ts
            emit(ssa + " = \"lang.tensor_binop_ts\"(" + i32Const(tOp) + ", " +
                 t.ssa + ", " + si + ", " + vi + ", " + vf + ")" + attrs +
                 " : (i32, !llvm.ptr, i32, i64, f64) -> !llvm.ptr " + opLoc());
        } else { // 标量 op 张量 → st（标量在左，C 侧负责方向）
            emit(ssa + " = \"lang.tensor_binop_st\"(" + i32Const(tOp) + ", " +
                 si + ", " + vi + ", " + vf + ", " + t.ssa + ")" + attrs +
                 " : (i32, i32, i64, f64, !llvm.ptr) -> !llvm.ptr " + opLoc());
        }
        return Value{VK::Tensor, ssa,
                     dtypeElementwiseIsInt(tOp == 3, t.tInt,
                                           sc.vk != VK::Float),
                     t.tRank};
    }

    // ---- 算术（kArithOps 注册表驱动） ----
    if (aop) {
        if (!isNumVK(l.vk) || !isNumVK(r.vk))
            unsupported(range, "运算符 '" + op + "' 的操作数类型");

        if (l.vk != VK::Float && r.vk != VK::Float) {
            // ---- 纯整数路径 ----
            if (op == "**") {
                // 负指数结果为 float（结果类型依赖运行时符号）——AOT 要求
                // 指数为非负整数字面量。
                if (!rExpr || rExpr->kind != Expr::Kind::IntLit ||
                    static_cast<const IntLit&>(*rExpr).value < 0)
                    unsupported(range,
                                "整数幂 '**' 需要非负整数字面量指数");
            }
            std::string ssa = fresh();
            emit(ssa + " = \"" + aop->intSym + "\"(" + l.ssa + ", " + r.ssa +
                 ") : (i64, i64) -> " + aop->intRet + " " + opLoc());
            return Value{aop->intRet[0] == 'f' ? VK::Float : VK::Int, ssa};
        }

        // ---- 浮点路径：arith 直降或 lang op（promote 后 emit） ----
        Value lf = promoteF64(l), rf = promoteF64(r);
        std::string ssa = fresh();
        if (aop->fSym)
            emit(ssa + " = \"" + aop->fSym + "\"(" + lf.ssa + ", " + rf.ssa +
                 ") : (f64, f64) -> f64 " + opLoc());
        else
            emit(ssa + " = " + aop->fArith + " " + lf.ssa + ", " + rf.ssa +
                 " : f64");
        return Value{VK::Float, ssa};
    }

    unsupported(range, "运算符 '" + op + "'");
}

Value Gen::genTernary(const Ternary& t) {
    VK vk = inferVK(t);
    if (vk == VK::Bad || vk == VK::Void || vk == VK::None)
        unsupported(t.range, "三元表达式分支类型");
    Value cond = genExpr(*t.cond);
    Slot slot = allocSlot(vk);
    uint8_t thenElem = 0, elseElem = 0;
    int thenB = newBlock(), elseB = newBlock(), joinB = newBlock();
    terminate("cf.cond_br " + cond.ssa + ", " + blk(thenB).label + ", " +
              blk(elseB).label);
    cur_ = thenB;
    {
        Value v = genExpr(*t.thenE);
        thenElem = v.lElem;
        storeTo(slot, v);
    }
    terminate("cf.br " + blk(joinB).label);
    cur_ = elseB;
    {
        Value v = genExpr(*t.elseE);
        elseElem = v.lElem;
        storeTo(slot, v);
    }
    terminate("cf.br " + blk(joinB).label);
    cur_ = joinB;
    // 静态模型一致性：两分支列表元素 VK 不一致时槽位单值记录会失真
    //（运行时 etag 与静态分派不符 → 内存误读），显式拒绝。
    if (vk == VK::List && thenElem != elseElem)
        unsupported(t.range, "三元表达式分支列表元素类型不一致");
    return loadSlot(slot);
}

} // namespace mlirgen
} // namespace aot
} // namespace scourge
