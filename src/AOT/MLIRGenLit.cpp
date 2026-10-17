// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT MLIR 生成器 · 字面量与容器构造
// ----------------------------------------------------------------------------
//  字符串常量（strConst）、张量字面量（全常量入模块级常量池 / 逐元素
//  alloca+store 两路径）、列表/字典/集合字面量（kvTagOf 统一 tag →
//  lang.list_new_*/lang.dict_new/lang.set_new 构造堆句柄；dict ABI 位槽
//  编解码 dictToBits/dictFromBits）、元组字面量（0.9.6：位槽 + list_new_i）。
//  内部分块文件，共享模型见 MLIRGenDetail.hpp。
// ============================================================================
#include "scourge/AOT/MLIRGenDetail.hpp"

namespace scourge {
namespace aot {
namespace mlirgen {

Value Gen::strConst(const std::string& text) {
    std::string ssa = fresh();
    emitEntry(ssa + " = llvm.mlir.addressof " + internStr(text) + " : !llvm.ptr");
    return Value{VK::Str, ssa};
}

// ============================================================================
// 张量辅助
// ============================================================================

/// 8 字节元素数组第 k 元素地址：GEP 元素类型取 i64（索引自动按 8 字节
/// 缩放）。LLVM 23 起元素类型跟随结果类型之后（"-> !llvm.ptr, i64"）。
std::string Gen::elemAddr8(const std::string& arr, const std::string& idxSsa) {
    std::string addr = fresh();
    emit(addr + " = llvm.getelementptr " + arr + "[" + idxSsa +
         "] : (!llvm.ptr, i64) -> !llvm.ptr, i64");
    return addr;
}

/// i64 值数组落盘：入口块 alloca N×i64（行主序连续），逐元素 store 到当前块。
std::string Gen::buildI64Array(const std::vector<Value>& items) {
    std::string arr = fresh();
    emitEntry(arr + " = llvm.alloca " +
              intConst(items.empty() ? 1 : (int64_t)items.size()) +
              " x i64 : (i64) -> !llvm.ptr");
    for (size_t k = 0; k < items.size(); ++k) {
        std::string addr = elemAddr8(arr, intConst((int64_t)k));
        emit("llvm.store " + items[k].ssa + ", " + addr + " : i64, !llvm.ptr");
    }
    return arr;
}

/// 元素级常量判定（Q7）：IntLit / FloatLit / 一元负号包裹的字面量 →
/// 文本形式（isF=true 时统一为 f64 文本）；非常量返回 false。
static bool constScalarText(const Expr& e, bool isF, std::string& out) {
    auto lit = [&](const Expr& x, std::string& dst) -> bool {
        if (x.kind == Expr::Kind::IntLit) {
            int64_t v = static_cast<const IntLit&>(x).value;
            dst = isF ? fmtF64(static_cast<double>(v)) : std::to_string(v);
            return true;
        }
        if (x.kind == Expr::Kind::FloatLit) {
            if (!isF) return false; // 浮点元素只允许 f64 数据
            dst = fmtF64(static_cast<const FloatLit&>(x).value);
            return true;
        }
        return false;
    };
    if (lit(e, out)) return true;
    if (e.kind == Expr::Kind::Unary) {
        auto& u = static_cast<const Unary&>(e);
        if (u.op == "-") {
            std::string inner;
            if (lit(*u.operand, inner)) {
                out = "-" + inner;
                return true;
            }
        }
    }
    return false;
}

/// 常量标量是否为浮点（穿透一元负号）：决定字面量走 f64 还是 i64 数据。
static bool floatConstExpr(const Expr& e) {
    const Expr* p = &e;
    if (p->kind == Expr::Kind::Unary) {
        auto& u = static_cast<const Unary&>(*p);
        if (u.op != "-") return false;
        p = u.operand.get();
    }
    return p && p->kind == Expr::Kind::FloatLit;
}

/// 全常量张量字面量 → 常量池符号（Q7/Q8）。数据以 dense 属性入模块级
/// llvm.global 常量，运行时由 sg_tensor_from_* 从常量区拷贝建堆句柄。
/// 同一数据（精确文本匹配）跨字面量去重复用。
std::string Gen::internTensorConst(bool isF, int64_t n,
                                   const std::string& dataText) {
    std::string key = (isF ? "f:" : "i:") + dataText;
    if (auto it = tensorConstPool_.find(key); it != tensorConstPool_.end())
        return it->second;
    std::string sym = "@sg_tc" + std::to_string(tensorConstCounter_++);
    const char* ty = isF ? "f64" : "i64";
    tensorConstDefs_.push_back(
        "llvm.mlir.global internal constant " + sym + "(dense<[" + dataText +
        "]> : tensor<" + std::to_string(n) + " x " + ty + ">) : !llvm.array<" +
        std::to_string(n) + " x " + ty + ">");
    tensorConstPool_.emplace(std::move(key), sym);
    return sym;
}

/// 张量字面量：全标量 → 一维（任一 Float → f64，与 VM tensorNew 一致）；
/// 全张量 → 行拼接升维（只比形状不比 dtype，混合 → 提升 f64）。
Value Gen::genTensorLit(const ListLit& ls) {
    // ---- Q7/Q8 快路径：全常量字面量直发模块级常量区 ------------------------
    // 在任何 genExpr 之前判定：逐元素 alloca+store 的栈上物化是大字面量
    // 溢出线程栈、N 条独立 SSA op 拖慢构建的共同根源。含变量 / bool /
    // 不规则形状时回落下方通用路径，语义不变。
    if (!ls.elems.empty()) {
        // 一维：全元素为常量标量
        bool flat1D = true, flatF = false;
        for (const auto& el : ls.elems) {
            if (!el || el->kind == Expr::Kind::ListLit) { flat1D = false; break; }
            std::string t;
            if (!constScalarText(*el, /*isF=*/true, t)) { flat1D = false; break; }
            if (floatConstExpr(*el)) flatF = true;
        }
        if (flat1D) {
            const bool isF = flatF;
            const int64_t n = (int64_t)ls.elems.size();
            std::string data;
            for (size_t k = 0; k < ls.elems.size(); ++k) {
                if (k) data += ", ";
                std::string t;
                constScalarText(*ls.elems[k], isF, t); // 已探明必成功
                data += t;
            }
            std::string sym = internTensorConst(isF, n, data);
            std::string p = fresh();
            emit(p + " = llvm.mlir.addressof " + sym + " : !llvm.ptr");
            std::string ssa = fresh();
            emit(ssa + " = \"lang.tensor_from_" + (isF ? "f64" : "i64") +
                 "\"(" + intConst(n) + ", " + p +
                 ") : (i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
            return Value{VK::Tensor, ssa, !isF, 1};
        }

        // 二维：全行为等宽全常量标量行（行内不得再嵌套 ListLit）
        bool grid2D = true, gridF = false;
        int64_t cols = -1;
        for (const auto& el : ls.elems) {
            if (!el || el->kind != Expr::Kind::ListLit) { grid2D = false; break; }
            auto& row = static_cast<const ListLit&>(*el);
            if (cols < 0) cols = (int64_t)row.elems.size();
            if (cols <= 0 || (int64_t)row.elems.size() != cols) {
                grid2D = false;
                break;
            }
            for (const auto& re : row.elems) {
                std::string t;
                if (!re || !constScalarText(*re, /*isF=*/true, t)) {
                    grid2D = false;
                    break;
                }
                if (floatConstExpr(*re)) gridF = true;
            }
            if (!grid2D) break;
        }
        if (grid2D) {
            const bool isF = gridF;
            const int64_t rows = (int64_t)ls.elems.size();
            std::string data;
            bool first = true;
            for (const auto& el : ls.elems) {
                auto& row = static_cast<const ListLit&>(*el);
                for (const auto& re : row.elems) {
                    if (!first) data += ", ";
                    first = false;
                    std::string t;
                    constScalarText(*re, isF, t); // 已探明必成功
                    data += t;
                }
            }
            std::string sym = internTensorConst(isF, rows * cols, data);
            std::string p = fresh();
            emit(p + " = llvm.mlir.addressof " + sym + " : !llvm.ptr");
            std::string flat = fresh();
            emit(flat + " = \"lang.tensor_from_" + (isF ? "f64" : "i64") +
                 "\"(" + intConst(rows * cols) + ", " + p +
                 ") : (i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
            std::string arr =
                buildI64Array({Value{VK::Int, intConst(rows), true, 0},
                               Value{VK::Int, intConst(cols), true, 0}});
            std::string ssa = fresh();
            emit(ssa + " = \"lang.tensor_reshape\"(" + flat + ", " +
                 intConst(2) + ", " + arr +
                 ") : (!llvm.ptr, i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
            return Value{VK::Tensor, ssa, !isF, 2};
        }
    }

    std::vector<Value> es;
    es.reserve(ls.elems.size());
    for (const auto& el : ls.elems) es.push_back(genExpr(*el));

    bool anyScalar = false, anyTensor = false, anyFloat = false, anyBool = false;
    for (const Value& v : es) {
        if (v.vk == VK::Tensor) {
            anyTensor = true;
        } else if (isNumVK(v.vk)) {
            anyScalar = true;
            if (v.vk == VK::Float) anyFloat = true;
            if (v.vk == VK::Bool) anyBool = true;
        }
    }
    if (anyScalar && anyTensor)
        unsupported(ls.range, "张量字面量标量与张量混排");
    // VM tensorNew 对 bool 元素 panic（"NewTensor 元素必须为数值或张量"），
    // 此处静态拒绝以保持双模失败等价。
    if (anyBool) unsupported(ls.range, "张量字面量含 bool 元素");

    if (anyTensor) {
        // 全张量行拼接升维
        std::string arr = fresh();
        emitEntry(arr + " = llvm.alloca " + intConst((int64_t)es.size()) +
                  " x !llvm.ptr : (i64) -> !llvm.ptr");
        bool allInt = true;
        uint8_t rowRank = es.empty() ? 0 : es[0].tRank;
        for (size_t k = 0; k < es.size(); ++k) {
            std::string addr = elemAddr8(arr, intConst((int64_t)k));
            emit("llvm.store " + es[k].ssa + ", " + addr +
                 " : !llvm.ptr, !llvm.ptr");
            allInt &= es[k].tInt;
        }
        std::string ssa = fresh();
        emit(ssa + " = \"lang.tensor_concat\"(" +
             intConst((int64_t)es.size()) + ", " + arr +
             ") : (i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
        return Value{VK::Tensor, ssa, allInt,
                     static_cast<uint8_t>(rowRank + 1)};
    }

    // 全标量 → 一维
    const bool isF = anyFloat;
    std::string arr = fresh();
    emitEntry(arr + " = llvm.alloca " +
              intConst(es.empty() ? 1 : (int64_t)es.size()) + " x " +
              (isF ? "f64" : "i64") + " : (i64) -> !llvm.ptr");
    for (size_t k = 0; k < es.size(); ++k) {
        Value ev = es[k];
        if (isF && ev.vk == VK::Int) ev = promoteF64(ev);
        std::string addr = elemAddr8(arr, intConst((int64_t)k));
        emit("llvm.store " + ev.ssa + ", " + addr + " : " +
             (isF ? "f64" : "i64") + ", !llvm.ptr");
    }
    std::string ssa = fresh();
    if (isF) {
        emit(ssa + " = \"lang.tensor_from_f64\"(" +
             intConst((int64_t)es.size()) + ", " + arr +
             ") : (i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
    } else {
        emit(ssa + " = \"lang.tensor_from_i64\"(" +
             intConst((int64_t)es.size()) + ", " + arr +
             ") : (i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
    }
    return Value{VK::Tensor, ssa, !isF, 1};
}

/// 指针句柄数组落盘（str 列表字面量）：入口块 alloca N×ptr，逐元素 store。
std::string Gen::buildPtrArray(const std::vector<Value>& items) {
    std::string arr = fresh();
    emitEntry(arr + " = llvm.alloca " +
              intConst(items.empty() ? 1 : (int64_t)items.size()) +
              " x !llvm.ptr : (i64) -> !llvm.ptr");
    for (size_t k = 0; k < items.size(); ++k) {
        std::string addr = elemAddr8(arr, intConst((int64_t)k));
        emit("llvm.store " + items[k].ssa + ", " + addr +
             " : !llvm.ptr, !llvm.ptr");
    }
    return arr;
}

/// f64 值数组落盘（float 列表字面量）：入口块 alloca N×f64，逐元素 store。
std::string Gen::buildF64Array(const std::vector<Value>& items) {
    std::string arr = fresh();
    emitEntry(arr + " = llvm.alloca " +
              intConst(items.empty() ? 1 : (int64_t)items.size()) +
              " x f64 : (i64) -> !llvm.ptr");
    for (size_t k = 0; k < items.size(); ++k) {
        std::string addr = elemAddr8(arr, intConst((int64_t)k));
        emit("llvm.store " + items[k].ssa + ", " + addr + " : f64, !llvm.ptr");
    }
    return arr;
}

/// 列表字面量：元素 VK 统一（同 VK 原样；int/float 混排 → f64，与检查器
/// unifyNumeric 一致）后经 lang.list_new_* 构造堆句柄（与 VM NewList 一致）。
Value Gen::genListLit(const ListLit& ls) {
    std::vector<Value> es;
    es.reserve(ls.elems.size());
    for (const auto& el : ls.elems) es.push_back(genExpr(*el));

    if (es.empty()) {
        std::string ssa = fresh();
        emit(ssa + " = \"lang.list_empty\"() : () -> !llvm.ptr");
        Value v{VK::List, ssa};
        // 空列表占位已由 Sema 精化（List[T]）时回填元素 VK：C 运行时按
        // etag 分派 list_store_*，lElem 未定型会让首写落入 unsupported。
        if (auto it = types_.find(&ls); it != types_.end() && it->second &&
                                     it->second->kind == Type::Kind::List &&
                                     !it->second->params.empty()) {
            VK ev = vkOfType(it->second->params[0]);
            if (ev != VK::Bad) v.lElem = static_cast<uint8_t>(ev);
        }
        return v;
    }

    // 元素统一：与检查器 checkListLit 一致——任一 Float → 全 f64；
    // 同为 Int 则保持 i64（bool 只能与 bool 混排，检查器 E2001 已拒异型）。
    bool anyFloat = false, allInt = true, allBool = true, allStr = true,
         allTuple = true;
    for (const Value& v : es) {
        if (v.vk == VK::Float) anyFloat = true;
        if (v.vk != VK::Int) allInt = false;
        if (v.vk != VK::Bool) allBool = false;
        if (v.vk != VK::Str) allStr = false;
        if (v.vk != VK::Tuple) allTuple = false;
    }
    if (!allInt && !allBool && !allStr && !anyFloat && !allTuple)
        unsupported(ls.range, "列表字面量含该元素类型");

    // 批 9 F-21：List[Tuple] 字面量——与 Dict.items()/for-in 同构，元素
    // 句柄 ptrtoint 存 i64 槽（sg_list_format_tuples/下标读同按指针位
    // 消费），lElem 记 Tuple 供下标与 for-in 还原句柄形态。
    if (allTuple) {
        std::vector<Value> bits;
        bits.reserve(es.size());
        for (Value& v : es) {
            std::string b = fresh();
            emit(b + " = llvm.ptrtoint " + v.ssa + " : !llvm.ptr to i64");
            bits.push_back(Value{VK::Int, b});
        }
        std::string arr = buildI64Array(bits);
        std::string h = fresh();
        emit(h + " = \"lang.list_new_i\"(" + intConst((int64_t)bits.size()) +
             ", " + arr + ") : (i64, !llvm.ptr) -> !llvm.ptr");
        return Value{VK::List, h, false, 0, static_cast<uint8_t>(VK::Tuple)};
    }

    // 逐元素统一转换（anyFloat 时 int/bool → f64；bool 元素在整型表中 zext i64）
    const bool isF = anyFloat;
    if (isF) {
        for (Value& v : es) {
            if (v.vk == VK::Int) v = promoteF64(v);
            else if (v.vk == VK::Bool) {
                std::string f = fresh();
                emit(f + " = arith.uitofp " + v.ssa + " : i1 to f64");
                v = Value{VK::Float, f};
            }
        }
        std::string arr = buildF64Array(es);
        std::string h = fresh();
        emit(h + " = \"lang.list_new_f\"(" + intConst((int64_t)es.size()) +
             ", " + arr + ") : (i64, !llvm.ptr) -> !llvm.ptr");
        return Value{VK::List, h, false, 0, static_cast<uint8_t>(VK::Float)};
    }
    if (allStr) {
        std::string arr = buildPtrArray(es);
        std::string h = fresh();
        emit(h + " = \"lang.list_new_s\"(" + intConst((int64_t)es.size()) +
             ", " + arr + ") : (i64, !llvm.ptr) -> !llvm.ptr");
        return Value{VK::List, h, false, 0, static_cast<uint8_t>(VK::Str)};
    }
    if (allBool) {
        // bool 列表：运行时按 i64 存储（etag 3），元素经 zext 提升
        std::vector<Value> wide;
        wide.reserve(es.size());
        for (const Value& v : es) {
            std::string z = fresh();
            emit(z + " = arith.extui " + v.ssa + " : i1 to i64");
            wide.push_back(Value{VK::Int, z});
        }
        std::string arr = buildI64Array(wide);
        std::string h = fresh();
        emit(h + " = \"lang.list_new_b\"(" + intConst((int64_t)es.size()) +
             ", " + arr + ") : (i64, !llvm.ptr) -> !llvm.ptr");
        return Value{VK::List, h, false, 0, static_cast<uint8_t>(VK::Bool)};
    }
    // 全 int
    std::string arr = buildI64Array(es);
    std::string h = fresh();
    emit(h + " = \"lang.list_new_i\"(" + intConst((int64_t)es.size()) + ", " +
         arr + ") : (i64, !llvm.ptr) -> !llvm.ptr");
    return Value{VK::List, h, false, 0, static_cast<uint8_t>(VK::Int)};
}

// ----------------------------------------------------------------------------
// 列表推导式（0.9.8）：空列表句柄 + 计数循环 + 条件 append。迭代源限
// range/List/str（与检查器同拒）；循环骨架与 genFor 同构（计数器槽 + 独立
// 增量块），body 换为「过滤条件 + list_append_*」。append 的列表句柄每轮
// 从槽重载（循环内 SSA 唯一性）；元素 int→float 提升与 append 成员方法同规。
// ----------------------------------------------------------------------------

Value Gen::genListComp(const ListComp& c) {
    Value src = genExpr(*c.iter);
    if (src.vk != VK::Range && src.vk != VK::Str && src.vk != VK::List)
        unsupported(c.iter->range,
                    "推导式迭代源仅支持 range、List 或 str");
    // 结果元素 VK：检查器注解表（List 参数 0）
    VK ev = VK::Bad;
    if (auto rt = types_.find(&c);
        rt != types_.end() && rt->second->kind == Type::Kind::List &&
        !rt->second->params.empty())
        ev = vkOfType(rt->second->params[0]);
    if (ev != VK::Int && ev != VK::Float && ev != VK::Bool && ev != VK::Str)
        unsupported(c.range, "推导式仅支持标量元素列表");

    Slot counter = allocSlot(VK::Int);
    Slot listSlot = allocSlot(VK::List);
    std::string lh = fresh();
    emit(lh + " = \"lang.list_empty\"() : () -> !llvm.ptr");
    storeTo(listSlot, Value{VK::List, lh, false, 0, static_cast<uint8_t>(ev)});

    Slot varSlot;
    if (src.vk == VK::Range) {
        varSlot = allocSlot(VK::Int);
        locals_[c.var] = varSlot;
        storeTo(counter, Value{VK::Int, zeroI64()});
    } else if (src.vk == VK::Str) {
        varSlot = allocSlot(VK::Str);
        locals_[c.var] = varSlot;
        storeTo(counter, Value{VK::Int, zeroI64()});
    } else {
        VK sev = static_cast<VK>(src.lElem);
        if (sev != VK::Int && sev != VK::Float && sev != VK::Bool &&
            sev != VK::Str)
            unsupported(c.iter->range, "该元素类型的列表推导式");
        varSlot = allocSlot(sev);
        locals_[c.var] = varSlot;
        storeTo(counter, Value{VK::Int, zeroI64()});
    }

    int condB = newBlock(), incrB = newBlock();
    terminate("cf.br " + blk(condB).label);
    cur_ = condB;
    std::string cnt = fresh();
    emit(cnt + " = llvm.load" + vol() + " " + counter.ptr +
         " : !llvm.ptr -> i64");
    // 轮次上限：按迭代源取长度（range=源上界 / str=list_len）
    std::string bound;
    if (src.vk == VK::Range) {
        bound = src.ssa;
    } else if (src.vk == VK::Str) {
        bound = fresh();
        emit(bound + " = \"lang.str_len\"(" + src.ssa +
             ") : (!llvm.ptr) -> i64");
    } else {
        bound = fresh();
        emit(bound + " = \"lang.list_len\"(" + src.ssa +
             ") : (!llvm.ptr) -> i64");
    }
    std::string cont = fresh();
    emit(cont + " = arith.cmpi slt, " + cnt + ", " + bound + " : i64");
    int bodyB = newBlock(), endB = newBlock();
    terminate("cf.cond_br " + cont + ", " + blk(bodyB).label + ", " +
              blk(endB).label);
    cur_ = bodyB;
    // 循环变量落槽：range=计数器原值 / str=str_chr_at / List=list_get_*
    if (src.vk == VK::Range) {
        storeTo(varSlot, Value{VK::Int, cnt});
    } else if (src.vk == VK::Str) {
        std::string ch = fresh();
        emit(ch + " = \"lang.str_chr_at\"(" + src.ssa + ", " + cnt +
             ") : (!llvm.ptr, i64) -> !llvm.ptr " + opLoc());
        storeTo(varSlot, Value{VK::Str, ch});
    } else {
        VK sev = static_cast<VK>(src.lElem);
        const char* getSym = sev == VK::Int   ? "lang.list_get_i"
                             : sev == VK::Float ? "lang.list_get_f"
                             : sev == VK::Bool  ? "lang.list_get_b"
                                                : "lang.list_get_s";
        const char* elemTy = sev == VK::Int   ? "i64"
                             : sev == VK::Float ? "f64"
                             : sev == VK::Bool  ? "i1"
                                                : "!llvm.ptr";
        std::string el = fresh();
        emit(el + " = \"" + getSym + "\"(" + src.ssa + ", " + cnt +
             ") : (!llvm.ptr, i64) -> " + elemTy);
        storeTo(varSlot, Value{sev, el});
    }
    // append 发射（cond/无 cond 两路径共用）：重载列表句柄 + 元素 +
    // list_append_*（void 调用，无 SSA 绑定）。
    auto emitAppend = [&] {
        std::string lv = fresh();
        emit(lv + " = llvm.load" + vol() + " " + listSlot.ptr +
             " : !llvm.ptr -> !llvm.ptr");
        Value v = genExpr(*c.elem);
        if (ev == VK::Float && v.vk == VK::Int) v = promoteF64(v);
        switch (ev) {
        case VK::Str:
            if (v.vk != VK::Str)
                unsupported(c.elem->range, "append() 实参类型与列表元素不符");
            emit("\"lang.list_append\"(" + lv + ", " + v.ssa +
                 ") : (!llvm.ptr, !llvm.ptr) -> () " + opLoc());
            break;
        case VK::Int:
            emit("\"lang.list_append_i\"(" + lv + ", " + v.ssa +
                 ") : (!llvm.ptr, i64) -> () " + opLoc());
            break;
        case VK::Float:
            emit("\"lang.list_append_f\"(" + lv + ", " + v.ssa +
                 ") : (!llvm.ptr, f64) -> () " + opLoc());
            break;
        case VK::Bool:
            emit("\"lang.list_append_b\"(" + lv + ", " + v.ssa +
                 ") : (!llvm.ptr, i1) -> () " + opLoc());
            break;
        default:
            unsupported(c.range, "推导式仅支持标量元素列表");
        }
    };
    // 过滤条件：false → 跳过 append（两个后继块都汇入独立增量块）
    if (c.cond) {
        Value cv = genExpr(*c.cond);
        int useB = newBlock(), skipB = newBlock();
        terminate("cf.cond_br " + cv.ssa + ", " + blk(useB).label + ", " +
                  blk(skipB).label);
        cur_ = useB;
        emitAppend();
        terminate("cf.br " + blk(incrB).label);
        cur_ = skipB;
        terminate("cf.br " + blk(incrB).label);
    } else {
        emitAppend();
        terminate("cf.br " + blk(incrB).label);
    }
    cur_ = incrB;
    std::string c2 = fresh();
    emit(c2 + " = llvm.load" + vol() + " " + counter.ptr +
         " : !llvm.ptr -> i64");
    std::string c3 = fresh();
    emit(c3 + " = arith.addi " + c2 + ", " + oneReg() + " : i64");
    storeTo(counter, Value{VK::Int, c3});
    terminate("cf.br " + blk(condB).label);
    cur_ = endB;
    return loadSlot(listSlot);
}

// ----------------------------------------------------------------------------
// 元组（0.9.6）：异构定长不可变——dict 位槽 ABI + i64 List 同构句柄。
// ----------------------------------------------------------------------------

Value Gen::genTupleLit(const TupleLit& tl) {
    // 元素逐个按位槽编码（int 直通 / float bitcast / bool zext / str 与
    // 嵌套元组句柄 ptrtoint / None 恒 0）后经 lang.list_new_i 构造堆句柄。
    // 运行时与 i64 List 同构，但仅元组专属路径（下标/相等/repr/len）按
    // 检查器类型表消费，不存在按 List 消费的通道——零新 lang op。
    // Tuple 上下文对齐（批3①）：类型表位 = 变量注解位——float 位上的
    // int 元素先 sitofp 再入槽（与 List 元素对齐同模式），杜绝 int 位型
    // 存槽、按注解 float 位读型的错位。
    auto tt = types_.find(&tl);
    bool aligned = tt != types_.end() && tt->second &&
                   tt->second->kind == Type::Kind::Tuple &&
                   tt->second->params.size() == tl.elems.size();
    std::vector<Value> bits;
    bits.reserve(tl.elems.size());
    for (size_t i = 0; i < tl.elems.size(); ++i) {
        Value ev = genExpr(*tl.elems[i]);
        if (aligned &&
            tt->second->params[i]->kind == Type::Kind::Float &&
            ev.vk == VK::Int)
            ev = promoteF64(ev);
        // str 元素进入位槽句柄（运行时不可辨识）→ 编译端补别名登记
        if (ev.vk == VK::Str) emitStrMark(ev);
        bits.push_back(Value{VK::Int, dictToBits(ev)});
    }
    std::string arr = buildI64Array(bits);
    std::string h = fresh();
    emit(h + " = \"lang.list_new_i\"(" + intConst((int64_t)bits.size()) + ", " +
         arr + ") : (i64, !llvm.ptr) -> !llvm.ptr");
    return Value{VK::Tuple, h};
}

Value Gen::genTupleElem(Value tuple, int64_t i, const TypePtr& elemTy,
                        const SourceRange& r) {
    // 元素位槽读取：下标为编译期常量且检查器已静态越界（E2004），运行时
    // list_get_i 的越界 panic 分支不可达——不挂 opLoc。
    if (vkOfType(elemTy) == VK::Bad)
        unsupported(r, "该元素类型的元组下标读取");
    std::string raw = fresh();
    emit(raw + " = \"lang.list_get_i\"(" + tuple.ssa + ", " + intConst(i) +
         ") : (!llvm.ptr, i64) -> i64");
    return dictFromBits(vkOfType(elemTy), raw);
}

// ----------------------------------------------------------------------------
// 字典（0.9.3）：与 VM NewDict 语义一致——键/值 64 位槽 ABI、插入序。
// ----------------------------------------------------------------------------

std::string Gen::dictToBits(const Value& v) {
    switch (v.vk) {
    case VK::Int:
    case VK::Range:
        return v.ssa; // range 不作键（检查器限标量键）；int 直通
    case VK::Float: {
        std::string s = fresh();
        emit(s + " = llvm.bitcast " + v.ssa + " : f64 to i64");
        return s;
    }
    case VK::Bool: {
        std::string s = fresh();
        emit(s + " = arith.extui " + v.ssa + " : i1 to i64");
        return s;
    }
    case VK::None:
        return intConst(0); // None 值恒存 0（无 SSA 表示）
    default: // Str / List / Dict / Tensor / Result → 指针位
    {
        std::string s = fresh();
        emit(s + " = llvm.ptrtoint " + v.ssa + " : !llvm.ptr to i64");
        return s;
    }
    }
}

Value Gen::dictFromBits(VK vk, const std::string& bits) {
    switch (vk) {
    case VK::Int:
        return Value{VK::Int, bits};
    case VK::Float: {
        std::string s = fresh();
        emit(s + " = llvm.bitcast " + bits + " : i64 to f64");
        return Value{VK::Float, s};
    }
    case VK::Bool: {
        std::string s = fresh();
        emit(s + " = arith.trunci " + bits + " : i64 to i1");
        return Value{VK::Bool, s};
    }
    case VK::None:
        return Value{VK::None, ""};
    case VK::Str:
    case VK::List:
    case VK::Dict:
    case VK::Tensor:
    case VK::Result:
    case VK::Tuple: { // 0.9.6 嵌套元组：元素槽存句柄（指针位），同容器族
        std::string s = fresh();
        emit(s + " = llvm.inttoptr " + bits + " : i64 to !llvm.ptr");
        return Value{vk, s};
    }
    default:
        return Value{VK::Bad, ""};
    }
}

/// 字典/集合槽位 tag 判定（与检查器 checkDictLit/checkSetLit 同构）：
/// 任一 float → 1（int∘float 统一）；全 int → 0；全 str → 2；全 bool → 3；
/// 字典值另支持容器句柄 → 4（isKey=false 时）。不可统一 → -1。
static int kvTagOf(const std::vector<Value>& xs, bool isKey) {
    bool anyF = false, allI = true, allB = true, allS = true, allH = true,
         allNum = true;
    for (const Value& v : xs) {
        if (v.vk == VK::Float) anyF = true;
        if (v.vk != VK::Int && v.vk != VK::Float) allNum = false;
        if (v.vk != VK::Int) allI = false;
        if (v.vk != VK::Bool) allB = false;
        if (v.vk != VK::Str) allS = false;
        if (v.vk != VK::List && v.vk != VK::Dict && v.vk != VK::Tensor &&
            v.vk != VK::Result)
            allH = false;
    }
    if (anyF) return allNum ? 1 : -1; // int∘float → float（unifyNumeric 同规则）
    if (allI) return 0;
    if (allB) return 3;
    if (allS) return 2;
    if (!isKey && allH) return 4;
    return -1;
}

Value Gen::genDictLit(const DictLit& dl) {
    std::vector<Value> ks, vs;
    ks.reserve(dl.pairs.size());
    vs.reserve(dl.pairs.size());
    for (const auto& p : dl.pairs) {
        ks.push_back(genExpr(*p.first));
        vs.push_back(genExpr(*p.second));
    }
    int ktag = kvTagOf(ks, true);
    if (ktag < 0) unsupported(dl.range, "该键类型的字典字面量");
    int vtag = kvTagOf(vs, false);
    if (vtag < 0) unsupported(dl.range, "该值类型的字典字面量");
    // 注解优先（unifyNumeric 之外的漂移源）：注解 Dict{K,V} 的槽位 tag 按
    // 注解种类定——`f: Dict[float, float] = {1.5: 10}` 的值按生成种类是
    // int 槽，但读取侧按注解 float 位还原，必须存储时就按注解提升。
    if (auto it = types_.find(&dl);
        it != types_.end() && it->second->kind == Type::Kind::Dict &&
        it->second->params.size() >= 2) {
        if (int a = annTagOf(it->second->params[0]); a >= 0) ktag = a;
        if (int a = annTagOf(it->second->params[1]); a >= 0) vtag = a;
    }

    // 统一转 64 位槽：float 槽的 int 元素先提升（Q3，与 VM promoteF64_ 对齐）
    auto toSlots = [&](std::vector<Value>& xs, int tag) {
        std::vector<Value> bits;
        bits.reserve(xs.size());
        for (Value& v : xs) {
            if (tag == 1 && v.vk == VK::Int) v = promoteF64(v);
            bits.push_back(Value{VK::Int, dictToBits(v)});
        }
        return bits;
    };
    std::string karr = buildI64Array(toSlots(ks, ktag));
    std::string varr = buildI64Array(toSlots(vs, vtag));
    std::string h = fresh();
    emit(h + " = \"lang.dict_new\"(" + i32Const(ktag) + ", " + i32Const(vtag) +
         ", " + intConst((int64_t)dl.pairs.size()) + ", " + karr + ", " +
         varr + ") : (i32, i32, i64, !llvm.ptr, !llvm.ptr) -> !llvm.ptr " +
         opLoc());
    return Value{VK::Dict, h};
}

Value Gen::genSetLit(const SetLit& sl) {
    std::vector<Value> es;
    es.reserve(sl.elems.size());
    for (const auto& el : sl.elems) es.push_back(genExpr(*el));
    // 元素统一（与检查器 checkSetLit 同构）：任一 float → 1；全 int → 0；
    // 全 str → 2；全 bool → 3（元素语义 = Dict 键，无容器句柄槽）。
    int tag = kvTagOf(es, true);
    if (tag < 0) unsupported(sl.range, "该元素类型的集合字面量");
    // 注解优先：Set[T] 上下文（含空 {} 判定的 DictLit）按注解槽位定 tag。
    if (auto it = types_.find(&sl);
        it != types_.end() && it->second->kind == Type::Kind::Set &&
        it->second->params.size() >= 1) {
        if (int a = annTagOf(it->second->params[0]); a >= 0) tag = a;
    }
    std::vector<Value> bits;
    bits.reserve(es.size());
    for (Value& v : es) {
        if (tag == 1 && v.vk == VK::Int) v = promoteF64(v); // Q3 对齐 VM
        bits.push_back(Value{VK::Int, dictToBits(v)});
    }
    std::string arr = buildI64Array(bits);
    std::string h = fresh();
    emit(h + " = \"lang.set_new\"(" + i32Const(tag) + ", " +
         intConst((int64_t)sl.elems.size()) + ", " + arr +
         ") : (i32, i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
    return Value{VK::Set, h};
}

Value Gen::genSetLitEmpty(const DictLit& dl, const TypePtr& ty) {
    // 空 {} 判定为空集合：tag 按注解元素种类（检查器已对齐，此路径仅
    // 由 Set 类型表项触发）。
    (void)dl;
    int tag = 0;
    if (ty->params.size() >= 1)
        if (int a = annTagOf(ty->params[0]); a >= 0) tag = a;
    std::string arr = buildI64Array({});
    std::string h = fresh();
    emit(h + " = \"lang.set_new\"(" + i32Const(tag) + ", " + intConst(0) +
         ", " + arr + ") : (i32, i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
    return Value{VK::Set, h};
}

} // namespace mlirgen
} // namespace aot
} // namespace scourge
