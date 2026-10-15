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
#include "MLIRGenDetail.hpp"

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
        return Value{VK::List, ssa};
    }

    // 元素统一：与检查器 checkListLit 一致——任一 Float → 全 f64；
    // 同为 Int 则保持 i64（bool 只能与 bool 混排，检查器 E2001 已拒异型）。
    bool anyFloat = false, allInt = true, allBool = true, allStr = true;
    for (const Value& v : es) {
        if (v.vk == VK::Float) anyFloat = true;
        if (v.vk != VK::Int) allInt = false;
        if (v.vk != VK::Bool) allBool = false;
        if (v.vk != VK::Str) allStr = false;
    }
    if (!allInt && !allBool && !allStr && !anyFloat)
        unsupported(ls.range, "列表字面量含该元素类型");

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
// 元组（0.9.6）：异构定长不可变——dict 位槽 ABI + i64 List 同构句柄。
// ----------------------------------------------------------------------------

Value Gen::genTupleLit(const TupleLit& tl) {
    // 元素逐个按位槽编码（int 直通 / float bitcast / bool zext / str 与
    // 嵌套元组句柄 ptrtoint / None 恒 0）后经 lang.list_new_i 构造堆句柄。
    // 运行时与 i64 List 同构，但仅元组专属路径（下标/相等/repr/len）按
    // 检查器类型表消费，不存在按 List 消费的通道——零新 lang op。
    std::vector<Value> bits;
    bits.reserve(tl.elems.size());
    for (const auto& el : tl.elems)
        bits.push_back(Value{VK::Int, dictToBits(genExpr(*el))});
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

/// 注解类型 → 槽位 tag（unifyNumeric 之外的漂移源：读取侧按注解位还原，
/// 存储必须按注解槽位提升）。不可作槽位种类 → -1。
static int annTagOf(const TypePtr& t) {
    switch (t->kind) {
    case Type::Kind::Int: return 0;
    case Type::Kind::Float: return 1;
    case Type::Kind::Str: return 2;
    case Type::Kind::Bool: return 3;
    case Type::Kind::List:
    case Type::Kind::Dict:
    case Type::Kind::Set:
    case Type::Kind::Tensor:
    case Type::Kind::Result: return 4;
    default: return -1;
    }
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
