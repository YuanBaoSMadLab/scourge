// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT MLIR 生成器 · 语句与控制流
// ----------------------------------------------------------------------------
//  语句分派（genStmtList/genStmt）、赋值/复合赋值、if/elif/else、while、
//  for（range/list/dict/set 迭代）、assert、try/except（sg_try_push 蹦床 +
//  生成函数自帧 setjmp）。内部分块文件，共享模型见 MLIRGenDetail.hpp。
// ============================================================================
#include "MLIRGenDetail.hpp"

namespace scourge {
namespace aot {
namespace mlirgen {

// ============================================================================
// 语句
// ============================================================================

void Gen::genStmtList(const StmtList& list) {
    for (const auto& s : list) genStmt(*s);
}

void Gen::emitDefers() {
    // LIFO 内联展开词法上位于该 return 之前的 defer 块。panic 传播走
    // longjmp 蹦床，不经过 return 点，故不触发 defer（契约见 AST Defer
    // 注释；与 VM emitDefers 双模同构）。
    for (size_t k = deferStack_.size(); k > 0; --k)
        genStmtList(*deferStack_[k - 1]);
}

void Gen::genStmt(const Stmt& s) {
    line_ = lineOf(s.range);
    switch (s.kind) {
    case Stmt::Kind::Assign:
        genAssign(static_cast<const Assign&>(s));
        break;
    case Stmt::Kind::AugAssign:
        genAugAssign(static_cast<const AugAssign&>(s));
        break;
    case Stmt::Kind::ExprStmt:
        genExpr(*static_cast<const ExprStmt&>(s).expr);
        break;
    case Stmt::Kind::If: {
        auto& st = static_cast<const If&>(s);
        genIf(*st.cond, st.thenBody,
              st.elifs.empty() ? nullptr : &st.elifs,
              st.elseBody.empty() ? nullptr : &st.elseBody);
        break;
    }
    case Stmt::Kind::While:
        genWhile(static_cast<const While&>(s));
        break;
    case Stmt::Kind::For:
        genFor(static_cast<const For&>(s));
        break;
    case Stmt::Kind::Return: {
        auto& st = static_cast<const Return&>(s);
        if (curIsTop_) unsupported(s.range, "顶层 return");
        if (!st.value) {
            emitDefers(); // defer 先生成（可能含 try 自帧），再清 try 簿记
            popTriesAbove(0); // return 穿越全部激活 try（对齐 VM doReturn 清理）
            terminate("func.return");
            break;
        }
        Value v = genExpr(*st.value);
        if (curRetVK_ == VK::Float && v.vk == VK::Int) {
            std::string ssa = fresh();
            emit(ssa + " = arith.sitofp " + v.ssa + " : i64 to f64");
            v = Value{VK::Float, ssa};
        }
        emitDefers(); // 返回值 SSA 已定，defer 块不重定义（SSA 唯一性）
        popTriesAbove(0); // return 穿越全部激活 try（对齐 VM doReturn 清理）
        terminate("func.return " + v.ssa + " : " + mlirTy(curRetVK_));
        break;
    }
    case Stmt::Kind::Break: {
        if (loopStack_.empty()) unsupported(s.range, "break 不在循环内");
        popTriesAbove(loopStack_.back().tryDepth); // 显式 ExitTry 穿越（VM 同构）
        terminate("cf.br " + loopStack_.back().brk);
        break;
    }
    case Stmt::Kind::Continue: {
        if (loopStack_.empty()) unsupported(s.range, "continue 不在循环内");
        popTriesAbove(loopStack_.back().tryDepth);
        terminate("cf.br " + loopStack_.back().cont);
        break;
    }
    case Stmt::Kind::Pass:
        break;
    case Stmt::Kind::Defer:
        // 仅登记不发射：展开发生在 return 点（emitDefers，LIFO；与
        // VM Compiler deferStack_ 同构）。
        deferStack_.push_back(&static_cast<const Defer&>(s).body);
        break;
    case Stmt::Kind::Assert:
        genAssert(static_cast<const Assert&>(s));
        break;
    case Stmt::Kind::Try:
        genTry(static_cast<const Try&>(s));
        break;
    case Stmt::Kind::Import:
    case Stmt::Kind::FuncDef:
        break; // 顶层处理；嵌套定义与字节码后端一致地忽略
    }
}

void Gen::genAssign(const Assign& a) {
    // 张量/字典下标写：base → 下标… → value（与 Compiler 的 StoreIndex 求值序一致）
    if (a.target->kind == Expr::Kind::Index) {
        auto& ix = static_cast<const Index&>(*a.target);
        Value base = genExpr(*ix.base);
        if (base.vk == VK::Dict) {
            // 字典键赋值：键/值 → 64 位槽；新键插入或覆盖（插入序，NaN 键
            // panic 与 VM 逐字一致）。键/值按 base 注解对齐槽位——float 槽
            // 的 int 先提升，与读取侧按注解位还原一致（观察行为双模等价）。
            if (ix.indices.size() != 1)
                unsupported(ix.range, "dict 只支持单下标");
            auto bty = types_.find(ix.base.get());
            Value k = genExpr(*ix.indices[0]);
            if (k.vk == VK::Int && bty != types_.end() &&
                bty->second->kind == Type::Kind::Dict &&
                bty->second->params.size() >= 2 &&
                bty->second->params[0]->kind == Type::Kind::Float)
                k = promoteF64(k);
            std::string kbits = dictToBits(k);
            Value v = genExpr(*a.value);
            if (v.vk == VK::Int && bty != types_.end() &&
                bty->second->kind == Type::Kind::Dict &&
                bty->second->params.size() >= 2 &&
                bty->second->params[1]->kind == Type::Kind::Float)
                v = promoteF64(v);
            std::string vbits = dictToBits(v);
            emit("\"lang.dict_set\"(" + base.ssa + ", " + kbits + ", " +
                 vbits + ") : (!llvm.ptr, i64, i64) -> () " + opLoc());
            return;
        }
        if (base.vk != VK::Tensor)
            unsupported(ix.base->range, "下标赋值仅支持张量与字典");
        std::vector<Value> idxs;
        for (const auto& ie : ix.indices) {
            Value iv = genExpr(*ie);
            if (iv.vk != VK::Int)
                unsupported(ie->range, "张量下标必须是 int");
            idxs.push_back(iv);
        }
        Value v = genExpr(*a.value);
        std::string arr = buildI64Array(idxs);
        if (base.tInt) {
            if (v.vk == VK::Float)
                unsupported(a.value->range, "浮点值无法赋给整型张量元素");
            if (v.vk == VK::Bool) {
                std::string z = fresh();
                emit(z + " = arith.extui " + v.ssa + " : i1 to i64");
                v = Value{VK::Int, z};
            }
            emit("\"lang.tensor_set_i\"(" + base.ssa + ", " +
                 intConst((int64_t)idxs.size()) + ", " + arr + ", " + v.ssa +
                 ") : (!llvm.ptr, i64, !llvm.ptr, i64) -> () " + opLoc());
        } else {
            if (v.vk == VK::Int) {
                std::string f = fresh();
                emit(f + " = arith.sitofp " + v.ssa + " : i64 to f64");
                v = Value{VK::Float, f};
            } else if (v.vk == VK::Bool) {
                std::string f = fresh();
                emit(f + " = arith.uitofp " + v.ssa + " : i1 to f64");
                v = Value{VK::Float, f};
            }
            emit("\"lang.tensor_set_f\"(" + base.ssa + ", " +
                 intConst((int64_t)idxs.size()) + ", " + arr + ", " + v.ssa +
                 ") : (!llvm.ptr, i64, !llvm.ptr, f64) -> () " + opLoc());
        }
        return;
    }
    if (a.target->kind != Expr::Kind::Name)
        unsupported(a.target->range, "该赋值目标");
    auto& n = static_cast<const Name&>(*a.target);

    // str 自拼接特化 s = s + b → lang.str_concat_grow（局部目标专属）。
    // 无闭包保证 callee 改不到局部，RHS 含调用也安全；全局目标不特化——
    // RHS 内调用可能经全局别名原地改掉先读入的快照，与 VM 值快照语义分叉
    // （AugAssign 无此问题：其求值序是先 RHS 后读左值）。求值序与通用
    // 路径一致：先读左值，后求 RHS。
    if (a.value->kind == Expr::Kind::Binary) {
        auto& bin = static_cast<const Binary&>(*a.value);
        if (bin.op == "+" && bin.lhs->kind == Expr::Kind::Name &&
            static_cast<const Name&>(*bin.lhs).name == n.name) {
            auto lt = locals_.find(n.name);
            if (lt != locals_.end() && lt->second.vk == VK::Str) {
                Value l = loadSlot(lt->second);
                Value r = genExpr(*bin.rhs);
                std::string ssa = fresh();
                emit(ssa + " = \"lang.str_concat_grow\"(" +
                     intConst(strAccCounter_++) + ", " + l.ssa + ", " + r.ssa +
                     ") : (i64, !llvm.ptr, !llvm.ptr) -> !llvm.ptr");
                Value v{VK::Str, ssa};
                v.owned = true; // grow 产出独占 → storeTo 免 mark
                storeTo(lt->second, v);
                return;
            }
        }
    }

    Value v = genExpr(*a.value);

    if (curIsTop_) {
        auto gt = curGlobals_->find(n.name);
        if (gt == curGlobals_->end())
            unsupported(a.target->range, "全局变量 '" + n.name + "' 未登记");
        storeGlobal(gt->second, v);
        return;
    }

    // 渐进式局部槽：新建时按注解类型（其次值类型）确定 VK。
    auto lt = locals_.find(n.name);
    if (lt == locals_.end()) {
        if (a.ann && a.ann->kind == TypeExpr::Kind::Tensor) {
            // 张量注解：dtype/rank 从注解回填（shape 维度数即秩；
            // 缺省形状时秩为 0，由首次 store 修正）
            Slot slot = allocSlot(VK::Tensor);
            if (!a.ann->params.empty() &&
                a.ann->params[0]->kind == TypeExpr::Kind::Named)
                slot.tInt = a.ann->params[0]->name != "float";
            slot.tRank = static_cast<uint8_t>(a.ann->shape.size());
            lt = locals_.emplace(n.name, slot).first;
            storeTo(lt->second, v);
            return;
        }
        VK vk;
        if (a.ann) {
            if (a.ann->kind == TypeExpr::Kind::Tuple) {
                // 元组注解：句柄槽（元素类型由检查器类型表按位消费）
                vk = VK::Tuple;
            } else {
                vk = a.ann->kind == TypeExpr::Kind::Named
                         ? vkOfAnnName(a.ann->name)
                         : VK::Bad;
                if (vk == VK::Bad)
                    unsupported(a.ann->range,
                                "变量注解类型 '" + a.ann->name + "'");
            }
        } else {
            vk = v.vk;
        }
        if (vk == VK::Bad || vk == VK::Void)
            unsupported(a.value->range, "变量 '" + n.name + "' 的类型");
        Slot slot = allocSlot(vk);
        if (vk == VK::List && a.ann && a.ann->params.size() == 1 &&
            a.ann->params[0]->kind == TypeExpr::Kind::Named) {
            // List[T] 注解：元素 VK 从注解参数回填（storeTo 亦会修正）
            VK ev = vkOfAnnName(a.ann->params[0]->name);
            if (ev != VK::Bad) slot.lElem = static_cast<uint8_t>(ev);
        }
        lt = locals_.emplace(n.name, slot).first;
    }
    storeTo(lt->second, v);
}

void Gen::genAugAssign(const AugAssign& a) {
    if (a.target->kind != Expr::Kind::Name)
        unsupported(a.target->range, "下标复合赋值");
    auto& n = static_cast<const Name&>(*a.target);
    std::string op = a.op;
    if (!op.empty() && op.back() == '=') op.pop_back(); // "+=" → "+"

    Value r = genExpr(*a.value);

    // 渐进式语义：先局部后全局；都不在 → 检查器已报 E2003（防御性报错）。
    Slot slot;
    bool isLocal = false;
    GlobalSlot g;
    auto lt = locals_.find(n.name);
    if (lt != locals_.end()) {
        slot = lt->second;
        isLocal = true;
    } else {
        auto gt = curGlobals_->find(n.name);
        if (gt == curGlobals_->end())
            unsupported(a.target->range, "未声明的标识符 '" + n.name + "'");
        g = gt->second;
    }
    VK slotVK = isLocal ? slot.vk : g.vk;

    // 复合赋值结果类型必须与槽一致（VM 槽动态类型允许变型，AOT 静态模型
    // 显式拒绝；与"张量静默降级禁止"同一纪律）。
    if (slotVK == VK::Str) {
        if (!(op == "+" && r.vk == VK::Str))
            unsupported(a.range, "字符串复合赋值仅支持 '+='");
        // 自拼接原地扩容特化：acc 按发射点编号登记，运行时对独占缓冲原地
        // 追加（perf 契约见 kRuntimeC sg_str_mark 注释）。求值序与 VM 复合
        // 赋值一致：r 先于左值读入——r 内调用可能原地改缓冲，但左值在调用
        // 之后才读，不存在快照过期；内容恒等由登记表+mark 保证。
        Value l = isLocal ? loadSlot(slot) : loadGlobal(curModule_, n.name);
        std::string ssa = fresh();
        emit(ssa + " = \"lang.str_concat_grow\"(" + intConst(strAccCounter_++) +
             ", " + l.ssa + ", " + r.ssa +
             ") : (i64, !llvm.ptr, !llvm.ptr) -> !llvm.ptr");
        Value v{VK::Str, ssa};
        v.owned = true; // grow 产出独占 → storeTo/storeGlobal 免 mark
        if (isLocal) storeTo(slot, v);
        else storeGlobal(g, v);
        return;
    }
    if (slotVK == VK::Tensor) {
        Value l = isLocal ? loadSlot(slot) : loadGlobal(curModule_, n.name);
        // 目标标识符表达式供 staticF64Shape 查 ExprTypeMap（lExpr 参数）
        Value v = genBinary(op, l, r, a.range, nullptr, a.target.get());
        if (isLocal) storeTo(slot, v);
        else storeGlobal(g, v);
        return;
    }
    if (!isNumVK(slotVK)) unsupported(a.range, "复合赋值该变量类型");
    if (op == "/" || r.vk == VK::Float) {
        if (slotVK != VK::Float)
            unsupported(a.range, "复合赋值 '" + a.op + "' 会改变 '" + n.name +
                                     "' 的静态类型（结果为 float）");
    }
    Value l = isLocal ? loadSlot(slot) : loadGlobal(curModule_, n.name);
    Value v = genBinary(op, l, r, a.range, nullptr);
    if (isLocal) storeTo(slot, v);
    else storeGlobal(g, v);
}

void Gen::genIf(const Expr& cond, const StmtList& thenBody,
                const std::vector<If::Elif>* elifs, const StmtList* elseBody) {
    Value c = genExpr(cond);
    int thenB = newBlock();
    if (!elifs && !elseBody) {
        int endB = newBlock();
        terminate("cf.cond_br " + c.ssa + ", " + blk(thenB).label + ", " +
                  blk(endB).label);
        cur_ = thenB;
        genStmtList(thenBody);
        terminate("cf.br " + blk(endB).label);
        cur_ = endB;
        return;
    }
    int elseB = newBlock(), endB = newBlock();
    terminate("cf.cond_br " + c.ssa + ", " + blk(thenB).label + ", " +
              blk(elseB).label);
    cur_ = thenB;
    genStmtList(thenBody);
    terminate("cf.br " + blk(endB).label);
    cur_ = elseB;
    if (elifs) {
        // elif 链：迭代展开（尾部 else 归入最后一环）。
        for (size_t k = 0; k < elifs->size(); ++k) {
            const If::Elif& e = (*elifs)[k];
            Value ec = genExpr(*e.cond);
            int eBody = newBlock(), nextB = newBlock();
            terminate("cf.cond_br " + ec.ssa + ", " + blk(eBody).label + ", " +
                      blk(nextB).label);
            cur_ = eBody;
            genStmtList(e.body);
            terminate("cf.br " + blk(endB).label);
            cur_ = nextB;
        }
        if (elseBody) genStmtList(*elseBody);
        terminate("cf.br " + blk(endB).label);
    } else {
        genStmtList(*elseBody);
        terminate("cf.br " + blk(endB).label);
    }
    cur_ = endB;
}

void Gen::genWhile(const While& w) {
    int condB = newBlock();
    terminate("cf.br " + blk(condB).label);
    cur_ = condB;
    Value c = genExpr(*w.cond);
    int bodyB = newBlock(), endB = newBlock();
    terminate("cf.cond_br " + c.ssa + ", " + blk(bodyB).label + ", " +
              blk(endB).label);
    cur_ = bodyB;
    loopStack_.push_back(LoopCtx{blk(endB).label, blk(condB).label, tryStack_.size()});
    genStmtList(w.body);
    loopStack_.pop_back();
    terminate("cf.br " + blk(condB).label); // 回边重新求值条件
    cur_ = endB;
}

void Gen::genFor(const For& f) {
    // range 迭代 + List 迭代（List 值：字面量 / 函数返回，均已有静态元素 VK）
    Value src = genExpr(*f.iter);
    Slot counter = allocSlot(VK::Int);
    Slot varSlot;
    if (src.vk == VK::Range) {
        varSlot = allocSlot(VK::Int);
        locals_[f.var] = varSlot;
        storeTo(counter, Value{VK::Int, zeroI64()});

        int condB = newBlock(), incrB = newBlock();
        terminate("cf.br " + blk(condB).label);
        cur_ = condB;
        std::string c = fresh();
        emit(c + " = llvm.load" + vol() + " " + counter.ptr +
             " : !llvm.ptr -> i64");
        std::string cont = fresh();
        emit(cont + " = arith.cmpi slt, " + c + ", " + src.ssa + " : i64");
        int bodyB = newBlock(), endB = newBlock();
        terminate("cf.cond_br " + cont + ", " + blk(bodyB).label + ", " +
                  blk(endB).label);
        cur_ = bodyB;
        storeTo(varSlot, Value{VK::Int, c});
        // continue 目标 = 独立增量块（增量绝不能放 body 尾单一路径——
        // continue 跳过 body 尾直落条件块会让计数器永不递增，死循环）。
        loopStack_.push_back(
            LoopCtx{blk(endB).label, blk(incrB).label, tryStack_.size()});
        genStmtList(f.body);
        loopStack_.pop_back();
        terminate("cf.br " + blk(incrB).label);
        cur_ = incrB;
        // 计数器 +1（body 以 return 终止时，后续 emit 自动落入死代码丢弃）。
        std::string c2 = fresh();
        emit(c2 + " = llvm.load" + vol() + " " + counter.ptr +
             " : !llvm.ptr -> i64");
        std::string c3 = fresh();
        emit(c3 + " = arith.addi " + c2 + ", " + oneReg() + " : i64");
        storeTo(counter, Value{VK::Int, c3});
        terminate("cf.br " + blk(condB).label);
        cur_ = endB;
        return;
    }
    if (src.vk == VK::Dict) {
        // Dict 迭代：0..dict_len-1 计数 + 逐轮 dict_key_at（插入序键遍历；
        // 迭代中新增键同样访问——与 VM ForNext Dict 分支语义一致）。
        VK kv = VK::Bad;
        if (auto it = types_.find(f.iter.get());
            it != types_.end() && it->second->kind == Type::Kind::Dict &&
            it->second->params.size() >= 1)
            kv = vkOfType(it->second->params[0]);
        if (kv == VK::Bad || kv == VK::Void || kv == VK::None)
            unsupported(f.iter->range, "该键类型的字典迭代");
        varSlot = allocSlot(kv);
        locals_[f.var] = varSlot;
        storeTo(counter, Value{VK::Int, zeroI64()});

        int condB = newBlock(), incrB = newBlock();
        terminate("cf.br " + blk(condB).label);
        cur_ = condB;
        std::string c = fresh();
        emit(c + " = llvm.load" + vol() + " " + counter.ptr +
             " : !llvm.ptr -> i64");
        std::string len = fresh();
        emit(len + " = \"lang.dict_len\"(" + src.ssa + ") : (!llvm.ptr) -> i64");
        std::string cont = fresh();
        emit(cont + " = arith.cmpi slt, " + c + ", " + len + " : i64");
        int bodyB = newBlock(), endB = newBlock();
        terminate("cf.cond_br " + cont + ", " + blk(bodyB).label + ", " +
                  blk(endB).label);
        cur_ = bodyB;
        std::string raw = fresh();
        emit(raw + " = \"lang.dict_key_at\"(" + src.ssa + ", " + c +
             ") : (!llvm.ptr, i64) -> i64 " + opLoc());
        storeTo(varSlot, dictFromBits(kv, raw));
        // continue 目标 = 独立增量块（与 range/List 分支一致）。
        loopStack_.push_back(
            LoopCtx{blk(endB).label, blk(incrB).label, tryStack_.size()});
        genStmtList(f.body);
        loopStack_.pop_back();
        terminate("cf.br " + blk(incrB).label);
        cur_ = incrB;
        std::string c2 = fresh();
        emit(c2 + " = llvm.load" + vol() + " " + counter.ptr +
             " : !llvm.ptr -> i64");
        std::string c3 = fresh();
        emit(c3 + " = arith.addi " + c2 + ", " + oneReg() + " : i64");
        storeTo(counter, Value{VK::Int, c3});
        terminate("cf.br " + blk(condB).label);
        cur_ = endB;
        return;
    }
    if (src.vk == VK::Set) {
        // Set 迭代：0..set_len-1 计数 + 逐轮 set_elem_at（首插序元素遍历；
        // 迭代中新增元素同样访问——与 VM ForNext Set 分支语义一致）。
        VK ev = VK::Bad;
        if (auto it = types_.find(f.iter.get());
            it != types_.end() && it->second->kind == Type::Kind::Set &&
            it->second->params.size() >= 1)
            ev = vkOfType(it->second->params[0]);
        if (ev == VK::Bad || ev == VK::Void || ev == VK::None)
            unsupported(f.iter->range, "该元素类型的集合迭代");
        varSlot = allocSlot(ev);
        locals_[f.var] = varSlot;
        storeTo(counter, Value{VK::Int, zeroI64()});

        int condB = newBlock(), incrB = newBlock();
        terminate("cf.br " + blk(condB).label);
        cur_ = condB;
        std::string c = fresh();
        emit(c + " = llvm.load" + vol() + " " + counter.ptr +
             " : !llvm.ptr -> i64");
        std::string len = fresh();
        emit(len + " = \"lang.set_len\"(" + src.ssa + ") : (!llvm.ptr) -> i64");
        std::string cont = fresh();
        emit(cont + " = arith.cmpi slt, " + c + ", " + len + " : i64");
        int bodyB = newBlock(), endB = newBlock();
        terminate("cf.cond_br " + cont + ", " + blk(bodyB).label + ", " +
                  blk(endB).label);
        cur_ = bodyB;
        std::string raw = fresh();
        emit(raw + " = \"lang.set_elem_at\"(" + src.ssa + ", " + c +
             ") : (!llvm.ptr, i64) -> i64 " + opLoc());
        storeTo(varSlot, dictFromBits(ev, raw));
        // continue 目标 = 独立增量块（与 Dict/List 分支一致）。
        loopStack_.push_back(
            LoopCtx{blk(endB).label, blk(incrB).label, tryStack_.size()});
        genStmtList(f.body);
        loopStack_.pop_back();
        terminate("cf.br " + blk(incrB).label);
        cur_ = incrB;
        std::string c2 = fresh();
        emit(c2 + " = llvm.load" + vol() + " " + counter.ptr +
             " : !llvm.ptr -> i64");
        std::string c3 = fresh();
        emit(c3 + " = arith.addi " + c2 + ", " + oneReg() + " : i64");
        storeTo(counter, Value{VK::Int, c3});
        terminate("cf.br " + blk(condB).label);
        cur_ = endB;
        return;
    }
    if (src.vk == VK::Str) {
        // str 迭代（0.9.7）：0..strlen-1 计数 + 逐轮 str_chr_at 单字节取字符
        // （字节遍历，非码点感知——与 VM ForNext Str 分支语义一致；空串零次循环）。
        varSlot = allocSlot(VK::Str);
        locals_[f.var] = varSlot;
        storeTo(counter, Value{VK::Int, zeroI64()});

        int condB = newBlock(), incrB = newBlock();
        terminate("cf.br " + blk(condB).label);
        cur_ = condB;
        std::string c = fresh();
        emit(c + " = llvm.load" + vol() + " " + counter.ptr +
             " : !llvm.ptr -> i64");
        std::string len = fresh();
        emit(len + " = \"lang.str_len\"(" + src.ssa + ") : (!llvm.ptr) -> i64");
        std::string cont = fresh();
        emit(cont + " = arith.cmpi slt, " + c + ", " + len + " : i64");
        int bodyB = newBlock(), endB = newBlock();
        terminate("cf.cond_br " + cont + ", " + blk(bodyB).label + ", " +
                  blk(endB).label);
        cur_ = bodyB;
        std::string ch = fresh();
        emit(ch + " = \"lang.str_chr_at\"(" + src.ssa + ", " + c +
             ") : (!llvm.ptr, i64) -> !llvm.ptr " + opLoc());
        storeTo(varSlot, Value{VK::Str, ch});
        // continue 目标 = 独立增量块（与 range/List/Dict 分支一致）。
        loopStack_.push_back(
            LoopCtx{blk(endB).label, blk(incrB).label, tryStack_.size()});
        genStmtList(f.body);
        loopStack_.pop_back();
        terminate("cf.br " + blk(incrB).label);
        cur_ = incrB;
        std::string c2 = fresh();
        emit(c2 + " = llvm.load" + vol() + " " + counter.ptr +
             " : !llvm.ptr -> i64");
        std::string c3 = fresh();
        emit(c3 + " = arith.addi " + c2 + ", " + oneReg() + " : i64");
        storeTo(counter, Value{VK::Int, c3});
        terminate("cf.br " + blk(condB).label);
        cur_ = endB;
        return;
    }
    if (src.vk != VK::List)
        unsupported(f.iter->range,
                    "for 迭代源仅支持 range、list、dict、Set 与 str");
    // List 迭代：0..len-1 计数 + 逐轮 list_get（越界不可达；与 VM ForNext
    // 的「元素 + true / 仅 false」两值语义等价——循环变量即元素）
    VK ev = static_cast<VK>(src.lElem);
    if (ev != VK::Int && ev != VK::Float && ev != VK::Bool && ev != VK::Str)
        unsupported(f.iter->range, "该元素类型的列表 for 迭代");
    varSlot = allocSlot(ev);
    locals_[f.var] = varSlot;
    storeTo(counter, Value{VK::Int, zeroI64()});

    int condB = newBlock(), incrB = newBlock();
    terminate("cf.br " + blk(condB).label);
    cur_ = condB;
    std::string c = fresh();
    emit(c + " = llvm.load" + vol() + " " + counter.ptr +
         " : !llvm.ptr -> i64");
    std::string len = fresh();
    emit(len + " = \"lang.list_len\"(" + src.ssa + ") : (!llvm.ptr) -> i64");
    std::string cont = fresh();
    emit(cont + " = arith.cmpi slt, " + c + ", " + len + " : i64");
    int bodyB = newBlock(), endB = newBlock();
    terminate("cf.cond_br " + cont + ", " + blk(bodyB).label + ", " +
              blk(endB).label);
    cur_ = bodyB;
    const char* getSym = ev == VK::Int   ? "lang.list_get_i"
                         : ev == VK::Float ? "lang.list_get_f"
                         : ev == VK::Bool  ? "lang.list_get_b"
                                           : "lang.list_get_s";
    const char* elemTy = ev == VK::Int   ? "i64"
                         : ev == VK::Float ? "f64"
                         : ev == VK::Bool  ? "i1"
                                           : "!llvm.ptr";
    std::string el = fresh();
    emit(el + " = \"" + getSym + "\"(" + src.ssa + ", " + c + ") : (!llvm.ptr, i64) -> " +
         elemTy);
    storeTo(varSlot, Value{ev, el});
    // continue 目标 = 独立增量块（同 range 分支——continue 必须经增量再回条件）。
    loopStack_.push_back(
        LoopCtx{blk(endB).label, blk(incrB).label, tryStack_.size()});
    genStmtList(f.body);
    loopStack_.pop_back();
    terminate("cf.br " + blk(incrB).label);
    cur_ = incrB;
    std::string c2 = fresh();
    emit(c2 + " = llvm.load" + vol() + " " + counter.ptr +
         " : !llvm.ptr -> i64");
    std::string c3 = fresh();
    emit(c3 + " = arith.addi " + c2 + ", " + oneReg() + " : i64");
    storeTo(counter, Value{VK::Int, c3});
    terminate("cf.br " + blk(condB).label);
    cur_ = endB;
}

void Gen::genAssert(const Assert& a) {
    // 求值顺序与 VM 一致：cond → msg（msg 不论断言成败都求值）→ 判定。
    Value cond = genExpr(*a.cond);
    Value msg;
    bool hasMsg = false;
    if (a.msg) {
        msg = genExpr(*a.msg);
        hasMsg = true;
    }
    int okB = newBlock(), panicB = newBlock();
    terminate("cf.cond_br " + cond.ssa + ", " + blk(okB).label + ", " +
              blk(panicB).label);
    cur_ = panicB;
    if (hasMsg) {
        Value prefix = strConst("断言失败：");
        std::string m = fresh();
        emit(m + " = \"lang.str_concat\"(" + prefix.ssa + ", " + msg.ssa +
             ") : (!llvm.ptr, !llvm.ptr) -> !llvm.ptr " + opLoc());
        emit("\"lang.panic\"(" + m + ") : (!llvm.ptr) -> () " + opLoc());
    } else {
        Value m = strConst("断言失败");
        emit("\"lang.panic\"(" + m.ssa + ") : (!llvm.ptr) -> () " + opLoc());
    }
    terminate("llvm.unreachable"); // 不可达兜底（sg_panic 不返回；无值 return
                                   // 在非 void 函数中不合法——llvm.unreachable
                                   // 语义精确且对任意返回类型成立）
    cur_ = okB;
}

// ============================================================================
// try / except（panic 恢复）
// ============================================================================

void Gen::popTriesAbove(size_t depth) {
    for (size_t k = tryStack_.size(); k-- > depth;)
        emit("func.call @sg_try_pop(" + tryStack_[k] + ") : (!llvm.ptr) -> ()");
    tryStack_.resize(depth);
}

void Gen::genTry(const Try& t) {
    // 蹦床：sg_try_push 取 handler 句柄 → 直调 CRT @_setjmp 双返回（0=直落
    // body，非 0=panic longjmp 到 handler）。setjmp 必须在生成函数自身活跃
    // 帧调用——包装函数方案下 longjmp 恢复的包装帧会被 try body 内同调用
    // 深度的兄弟调用覆盖，恢复到死帧。SgTry.jb 为首成员，句柄即 jmp_buf*。
    // x64 上 clang 前端对 setjmp 有 intrinsic 特判：@_setjmp 实为双参
    // (jmp_buf, frameaddress(0))，这里复刻该 ABI（@llvm.frameaddress.p0 经
    // 声明调用，LLVM 后端按 intrinsic 展开）。处理器栈簿记与 VM catches_
    // 同构：正常路径 body 尾 sg_try_pop；panic 路径由 longjmp 落 handler
    // 块，tryStack_ 以入口快照恢复（body 内 break/return 可能已弹走部分层）。
    line_ = lineOf(t.range);
    tryDeclsEmitted_ = true; // assemble 时补插蹦床声明（幂等，仅一次）
    frameHasSetjmp_ = true;  // 本函数槽位读写自此全部 volatile（跨 longjmp 存活）
    std::string h = fresh();
    emit(h + " = func.call @sg_try_push() : () -> !llvm.ptr");
    std::vector<std::string> entryStack = tryStack_;
    tryStack_.push_back(h);
    std::string r = fresh();
    std::string fa = fresh();
    emit(fa + " = func.call @llvm.frameaddress.p0(" + i32Const(0) +
         ") : (i32) -> !llvm.ptr");
    emit(r + " = func.call @_setjmp(" + h + ", " + fa +
         ") : (!llvm.ptr, !llvm.ptr) -> i32");
    std::string nz = fresh();
    emit(nz + " = arith.cmpi ne, " + r + ", " + i32Const(0) + " : i32");
    int bodyB = newBlock(), catchB = newBlock(), joinB = newBlock();
    terminate("cf.cond_br " + nz + ", " + blk(catchB).label + ", " +
              blk(bodyB).label);
    cur_ = bodyB;
    genStmtList(t.body);
    if (!tryStack_.empty() && tryStack_.back() == h) {
        // 正常路径 ExitTry（body 以 return/break 终止时 emit 已死、簿记已弹）
        emit("func.call @sg_try_pop(" + h + ") : (!llvm.ptr) -> ()");
        tryStack_.pop_back();
    }
    terminate("cf.br " + blk(joinB).label);
    cur_ = catchB;
    // longjmp 消费了 h（sg_panic 不弹栈，故 handler 首行补 sg_try_pop）；
    // 簿记恢复 = try 入口状态（本身即不含 h，无需再 pop）。
    tryStack_ = entryStack;
    emit("func.call @sg_try_pop(" + h + ") : (!llvm.ptr) -> ()");
    genStmtList(t.handlerBody);
    terminate("cf.br " + blk(joinB).label);
    cur_ = joinB;
}

} // namespace mlirgen
} // namespace aot
} // namespace scourge
