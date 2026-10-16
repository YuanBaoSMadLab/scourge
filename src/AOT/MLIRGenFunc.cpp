// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT MLIR 生成器 · 函数生命周期
// ----------------------------------------------------------------------------
//  函数骨架（startFunction 入口块/参数 → finishFunction 收尾 → render 汇出 →
//  pruneUnreachable 剪枝）、局部槽/全局槽存取（allocSlot/loadSlot/storeTo/
//  loadGlobal/storeGlobal）与 func.call 发射（emitCallRet：int→float 提升、
//  张量/List 返回值元数据回填）。内部分块文件，共享模型见 MLIRGenDetail.hpp。
// ============================================================================
#include "MLIRGenDetail.hpp"

namespace scourge {
namespace aot {
namespace mlirgen {

// ============================================================================
// 函数骨架
// ============================================================================

void Gen::startFunction(const std::string& symLine, const std::string& dispName,
                        VK retVK, bool isTop,
                        const std::vector<std::pair<std::string, VK>>& params) {
    curSymLine_ = symLine;
    curDispName_ = dispName;
    curRetVK_ = retVK;
    curIsTop_ = isTop;
    blocks_.clear();
    blocks_.emplace_back(); // 入口块（label 空）
    cur_ = 0;
    tmp_ = 0;
    blockCounter_ = 0;
    oneSsa_.clear();
    zeroI64Ssa_.clear();
    zeroF64Ssa_.clear();
    btrueSsa_.clear();
    bfalseSsa_.clear();
    intConsts_.clear();
    i32Consts_.clear();
    locals_.clear();
    loopStack_.clear();
    tryStack_.clear();
    frameHasSetjmp_ = false;

    // 参数槽 + store（None 参数无 SSA，跳过 store）
    for (size_t k = 0; k < params.size(); ++k) {
        Slot slot = allocSlot(params[k].second);
        locals_.emplace(params[k].first, slot);
        if (params[k].second != VK::None) {
            std::string arg = "%a" + std::to_string(k);
            const char* ty = mlirTy(params[k].second);
            // 一律 volatile：try 可能出现在函数体任何位置（allocSlot 先于
            // genTry 发生），入口块一次性 store 的 volatile 开销可忽略。
            emit("llvm.store volatile " + arg + ", " + slot.ptr + " : " + ty +
                 ", !llvm.ptr");
            // 形参绑定产生第二引用 → 别名登记（调用方累积缓冲据此禁原地扩容）
            if (params[k].second == VK::Str)
                emit("\"lang.str_mark\"(" + arg + ") : (!llvm.ptr) -> ()");
        }
    }

    cur_ = newBlock(); // body 块自 ^bb1 起
}

void Gen::finishFunction() {
    // 入口块补跳转（必须先于剪枝：入口终结符是可达性 BFS 的起点边）。
    Block& entry = blocks_[0];
    if (!entry.terminated) {
        entry.lines.push_back("cf.br ^bb1");
        entry.terminated = true;
    }
    // 可达性剪枝：方言转换（--convert-to-llvm）会把不可达块从函数中
    // 孤立出来，其 func.return 将失去 func.func 父节点而违反合法性。
    pruneUnreachable();
    // 其余可达块补终止（死代码块也必须合法终止；入口块已带终止符）。
    for (size_t i = 1; i < blocks_.size(); ++i) {
        if (blocks_[i].terminated || !blocks_[i].reachable) continue;
        int saved = cur_;
        cur_ = static_cast<int>(i);
        switch (curRetVK_) {
        case VK::Int:
        case VK::Range:
            emit("func.return " + zeroI64() + " : i64");
            break;
        case VK::Float:
            emit("func.return " + zeroF64() + " : f64");
            break;
        case VK::Bool:
            emit("func.return " + boolReg(false) + " : i1");
            break;
        case VK::Str:
            emit("func.return " + strConst("").ssa + " : !llvm.ptr");
            break;
        case VK::Tensor: {
            std::string p = fresh();
            emit(p + " = \"lang.tensor_empty\"() : () -> !llvm.ptr");
            emit("func.return " + p + " : !llvm.ptr");
            break;
        }
        case VK::Result: {
            // 零值返回 = Err("")（与 VM 槽位零值语义一致）
            std::string p = fresh();
            emit(p + " = llvm.mlir.addressof " + internStr("") + " : !llvm.ptr");
            std::string h = fresh();
            emit(h + " = \"lang.result_err\"(" + p + ") : (!llvm.ptr) -> !llvm.ptr");
            emit("func.return " + h + " : !llvm.ptr");
            break;
        }
        case VK::List:
        case VK::Tuple: {
            std::string p = fresh();
            emit(p + " = \"lang.list_empty\"() : () -> !llvm.ptr");
            emit("func.return " + p + " : !llvm.ptr");
            break;
        }
        default:
            emit("func.return");
            break;
        }
        cur_ = saved;
    }
    funcDefs_.push_back(renderFunction());
}

std::string Gen::renderFunction() const {
    std::string out = "  " + curSymLine_ + " {\n";
    for (const auto& b : blocks_) {
        if (!b.reachable) continue; // 不可达块整块不渲染
        if (!b.label.empty()) out += "  " + b.label + ":\n";
        for (const auto& l : b.lines) out += "    " + l + "\n";
    }
    out += "  }\n";
    return out;
}

void Gen::pruneUnreachable() {
    // 从入口块沿终结符目标（cf.br / cf.cond_br 的 ^bbN 操作数）BFS。
    std::vector<char> reach(blocks_.size(), 0);
    std::vector<int> work{0};
    reach[0] = 1;
    while (!work.empty()) {
        int b = work.back();
        work.pop_back();
        const Block& blk = blocks_[static_cast<size_t>(b)];
        if (!blk.terminated || blk.lines.empty()) continue;
        const std::string& term = blk.lines.back();
        for (size_t p = term.find("^bb"); p != std::string::npos;
             p = term.find("^bb", p + 1)) {
            size_t q = p + 3;
            int id = 0;
            bool any = false;
            while (q < term.size() && term[q] >= '0' && term[q] <= '9') {
                id = id * 10 + (term[q] - '0');
                ++q;
                any = true;
            }
            if (any && static_cast<size_t>(id) < reach.size() &&
                !reach[static_cast<size_t>(id)]) {
                reach[static_cast<size_t>(id)] = 1;
                work.push_back(id);
            }
        }
    }
    for (size_t i = 0; i < blocks_.size(); ++i)
        blocks_[i].reachable = reach[i] != 0;
}

// ============================================================================
// 存取
// ============================================================================

Slot Gen::allocSlot(VK vk) {
    if (vk == VK::None) return Slot{VK::None, ""};
    Slot slot;
    slot.vk = vk;
    slot.ptr = fresh();
    const char* ty = mlirTy(vk);
    emitEntry(slot.ptr + " = llvm.alloca " + oneReg() + " x " + ty +
              " : (i64) -> !llvm.ptr");
    // 零值初始化（语义上先写后读；与 VM 槽位零值对齐，防野值）。
    // 一律 volatile：try 可能在槽分配之后才出现，而含 setjmp 的函数中
    // 非 volatile 槽跨 longjmp 的值 indeterminate（clang -O2 会据此折叠
    // catch 块 load / 删 handler store）；入口块一次性开销可忽略。
    switch (vk) {
    case VK::Int:
    case VK::Range:
        emitEntry("llvm.store volatile " + zeroI64() + ", " + slot.ptr +
                  " : i64, !llvm.ptr");
        break;
    case VK::Float:
        emitEntry("llvm.store volatile " + zeroF64() + ", " + slot.ptr +
                  " : f64, !llvm.ptr");
        break;
    case VK::Bool:
        emitEntry("llvm.store volatile " + boolReg(false) + ", " + slot.ptr +
                  " : i1, !llvm.ptr");
        break;
    case VK::Str: {
        std::string p = fresh();
        emitEntry(p + " = llvm.mlir.addressof " + internStr("") + " : !llvm.ptr");
        emitEntry("llvm.store volatile " + p + ", " + slot.ptr +
                  " : !llvm.ptr, !llvm.ptr");
        break;
    }
    case VK::Tensor: {
        // 零值槽存空张量句柄（防野值；与 VM 槽位零值纪律一致）。
        std::string p = fresh();
        emitEntry(p + " = \"lang.tensor_empty\"() : () -> !llvm.ptr");
        emitEntry("llvm.store volatile " + p + ", " + slot.ptr +
                  " : !llvm.ptr, !llvm.ptr");
        break;
    }
    case VK::Result: {
        // 零值槽存 Err("") 句柄（与 VM 槽位零值纪律一致）
        std::string p = fresh();
        emitEntry(p + " = llvm.mlir.addressof " + internStr("") + " : !llvm.ptr");
        std::string h = fresh();
        emitEntry(h + " = \"lang.result_err\"(" + p +
                  ") : (!llvm.ptr) -> !llvm.ptr");
        emitEntry("llvm.store volatile " + h + ", " + slot.ptr +
                  " : !llvm.ptr, !llvm.ptr");
        break;
    }
    case VK::List:
    case VK::Tuple: {
        // 元组句柄与 List 同构：零值槽 = 空句柄（与 VM 槽位零值纪律一致）
        std::string p = fresh();
        emitEntry(p + " = \"lang.list_empty\"() : () -> !llvm.ptr");
        emitEntry("llvm.store volatile " + p + ", " + slot.ptr +
                  " : !llvm.ptr, !llvm.ptr");
        break;
    }
    default:
        break;
    }
    return slot;
}

Value Gen::loadSlot(const Slot& slot) {
    if (slot.vk == VK::None) return Value{VK::None, ""};
    std::string ssa = fresh();
    emit(ssa + " = llvm.load" + vol() + " " + slot.ptr + " : !llvm.ptr -> " +
         mlirTy(slot.vk));
    Value v{slot.vk, ssa, slot.tInt, slot.tRank};
    v.lElem = slot.lElem;
    return v;
}

/// 存储槽位：唯一隐式转换 int → float（与 isConvertible 一致）；
/// Tensor 槽同步更新元素 dtype / 秩记录；List 槽同步更新元素 VK 记录。
void Gen::storeTo(Slot& slot, Value v) {
    if (slot.vk == VK::None) return;
    if (slot.vk == VK::Tensor) {
        slot.tInt = v.tInt;
        slot.tRank = v.tRank;
    }
    if (slot.vk == VK::List) slot.lElem = v.lElem;
    if (slot.vk == VK::Float && v.vk == VK::Int) {
        std::string ssa = fresh();
        emit(ssa + " = arith.sitofp " + v.ssa + " : i64 to f64");
        v = Value{VK::Float, ssa};
    }
    // str 非独占值存槽 → 别名登记（独占新缓冲免登记；owned 契约见 Detail）
    if (slot.vk == VK::Str && !v.owned) emitStrMark(v);
    emit(std::string("llvm.store") + vol() + " " + v.ssa + ", " + slot.ptr +
         " : " + mlirTy(slot.vk) + ", !llvm.ptr");
}

Value Gen::loadGlobal(const std::string& module, const std::string& name) {
    auto it = globals_.find(module);
    if (it == globals_.end()) return Value{VK::Bad, ""};
    auto gt = it->second.find(name);
    if (gt == it->second.end()) return Value{VK::Bad, ""};
    const GlobalSlot& g = gt->second;
    if (g.vk == VK::None) return Value{VK::None, ""};
    std::string p = fresh();
    emit(p + " = llvm.mlir.addressof " + g.sym + " : !llvm.ptr");
    std::string ssa = fresh();
    emit(ssa + " = llvm.load " + p + " : !llvm.ptr -> " + mlirTy(g.vk));
    return Value{g.vk, ssa};
}

void Gen::storeGlobal(const GlobalSlot& g, Value v) {
    if (g.vk == VK::None) return;
    std::string p = fresh();
    emit(p + " = llvm.mlir.addressof " + g.sym + " : !llvm.ptr");
    if (g.vk == VK::Float && v.vk == VK::Int) {
        std::string ssa = fresh();
        emit(ssa + " = arith.sitofp " + v.ssa + " : i64 to f64");
        v = Value{VK::Float, ssa};
    }
    // str 非独占值存全局槽 → 别名登记（与 storeTo 同一契约）
    if (g.vk == VK::Str && !v.owned) emitStrMark(v);
    emit("llvm.store " + v.ssa + ", " + p + " : " + mlirTy(g.vk) +
         ", !llvm.ptr");
}

// ============================================================================
// 调用发射
// ============================================================================

Value Gen::emitCallRet(const std::string& sym, const std::vector<Value>& args,
                       const std::vector<VK>& paramVKs, VK retVK,
                       const TypePtr& retType) {
    std::vector<Value> a = args;
    std::string tys, vals;
    size_t n = 0;
    for (size_t k = 0; k < a.size(); ++k) {
        if (paramVKs[k] == VK::None) continue; // None 形参无 SSA
        if (paramVKs[k] == VK::Float && a[k].vk == VK::Int) {
            std::string ssa = fresh();
            emit(ssa + " = arith.sitofp " + a[k].ssa + " : i64 to f64");
            a[k] = Value{VK::Float, ssa};
        }
        if (n++) {
            vals += ", ";
            tys += ", ";
        }
        vals += a[k].ssa;
        tys += mlirTy(paramVKs[k]);
    }
    std::string callTail = (vals.empty() ? "()" : "(" + vals + ")") + " : (" +
                           tys + ") -> " +
                           (retVK == VK::None ? "()" : std::string(mlirTy(retVK)));
    if (retVK == VK::None) {
        emit("func.call " + sym + callTail);
        return Value{VK::None, ""};
    }
    std::string ssa = fresh();
    emit(ssa + " = func.call " + sym + callTail);
    if (retVK == VK::Tensor && retType &&
        retType->kind == Type::Kind::Tensor) {
        bool tInt = !retType->params.empty() &&
                    retType->params[0]->kind != Type::Kind::Float;
        return Value{retVK, ssa, tInt,
                     static_cast<uint8_t>(retType->shape.size())};
    }
    if (retVK == VK::List && retType && retType->kind == Type::Kind::List &&
        !retType->params.empty()) {
        VK ev = vkOfType(retType->params[0]);
        return Value{retVK, ssa, false, 0, static_cast<uint8_t>(ev)};
    }
    return Value{retVK, ssa};
}

} // namespace mlirgen
} // namespace aot
} // namespace scourge
