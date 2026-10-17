// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT MLIR 生成器 · 内建调用
// ----------------------------------------------------------------------------
//  职责边界：genBuiltinCall（print/str/len/数学/张量/容器方法等全表分派）
//  与 genTupleRepr（元组 repr，与 VM toString(Tuple, repr=true) 逐字一致）。
//  从 MLIRGenCall.cpp 按功能域拆出；共享模型见 MLIRGenDetail.hpp，
//  调用分派与 @extern 见 MLIRGenCall.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/AOT/MLIRGenDetail.hpp"

namespace scourge {
namespace aot {
namespace mlirgen {

// ----------------------------------------------------------------------------
// 元组 repr（0.9.6）：与 VM toString(Tuple, repr=true) 逐字一致
// ----------------------------------------------------------------------------

Value Gen::genTupleRepr(Value tuple, const TypePtr& ty, const SourceRange& r) {
    // "(" + 各元素 repr（", " 连接）+ 单元素尾逗号 + ")"。元素按类型表
    // params 静态 VK 分派：str 走 lang.str_escape（带引号转义，repr 模式），
    // None 恒 "None"，嵌套元组递归；元素经 genTupleElem 位槽读取。
    const auto& ps = ty->params;
    auto cat = [&](const Value& a, const Value& b) -> Value {
        std::string ssa = fresh();
        emit(ssa + " = \"lang.str_concat\"(" + a.ssa + ", " + b.ssa +
             ") : (!llvm.ptr, !llvm.ptr) -> !llvm.ptr");
        return Value{VK::Str, ssa};
    };
    auto reprOf = [&](const Value& e, const TypePtr& et) -> Value {
        std::string ssa;
        switch (e.vk) {
        case VK::Int:
            ssa = fresh();
            emit(ssa + " = \"lang.str_from_i64\"(" + e.ssa +
                 ") : (i64) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        case VK::Float:
            ssa = fresh();
            emit(ssa + " = \"lang.str_from_f64\"(" + e.ssa +
                 ") : (f64) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        case VK::Bool:
            ssa = fresh();
            emit(ssa + " = \"lang.str_from_bool\"(" + e.ssa +
                 ") : (i1) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        case VK::Str:
            ssa = fresh();
            emit(ssa + " = \"lang.str_escape\"(" + e.ssa +
                 ") : (!llvm.ptr) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        case VK::None:
            return strConst("None");
        case VK::Tuple:
            return genTupleRepr(e, et, r); // 嵌套元组递归
        default:
            unsupported(r, "该元素类型的元组 repr");
        }
    };
    Value acc = strConst("(");
    for (size_t k = 0; k < ps.size(); ++k) {
        if (k) acc = cat(acc, strConst(", "));
        acc = cat(acc, reprOf(genTupleElem(tuple, (int64_t)k, ps[k], r), ps[k]));
    }
    if (ps.size() == 1) acc = cat(acc, strConst(","));
    return cat(acc, strConst(")"));
}

Value Gen::genBuiltinCall(const std::string& name, const Call& c) {
    auto need = [&](size_t want) {
        if (c.args.size() != want) unsupported(c.range, "内建 " + name + "() 参数个数");
    };
    // F-22：List[Tuple[k,v]] → repr 字符串（与 VM stringify 逐字一致）。
    // 元组槽为句柄位，k/v 位型由静态元素类型经 annTagOf 还原（tag 编码同
    // etag）；容器槽位（tag 4/-1）编译期拒绝（与容器值字典同先例）。
    auto formatTuples = [&](Value v, const Expr& arg) -> Value {
        auto it = types_.find(&arg);
        if (it == types_.end() || it->second->params.empty() ||
            it->second->params[0]->kind != Type::Kind::Tuple ||
            it->second->params[0]->params.size() < 2)
            unsupported(arg.range, "元组元素列表缺少静态元素类型");
        int kt = annTagOf(it->second->params[0]->params[0]);
        int vt = annTagOf(it->second->params[0]->params[1]);
        if (kt < 0 || kt > 3 || vt < 0 || vt > 3)
            unsupported(arg.range, "元组元素列表（该元素槽位类型）");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.list_format_tuples\"(" + v.ssa + ", " +
             intConst(kt) + ", " + intConst(vt) +
             ") : (!llvm.ptr, i64, i64) -> !llvm.ptr");
        return Value{VK::Str, ssa};
    };

    // IO/网络/系统域分块（MLIRGenBuiltinSys.cpp）：命中即返回；未命中
    // 落回主链。两链名字不相交，分派顺序不影响语义。
    if (auto v = genBuiltinSysCall(name, c)) return *v;

    if (name == "print") {
        std::vector<Value> args;
        for (const auto& a : c.args) args.push_back(genExpr(*a));
        for (size_t k = 0; k < args.size(); ++k) {
            if (k) emit("\"lang.print_space\"() : () -> ()");
            const Value& v = args[k];
            switch (v.vk) {
            case VK::Int:
                emit("\"lang.print_i64\"(" + v.ssa + ") : (i64) -> ()");
                break;
            case VK::Float:
                emit("\"lang.print_f64\"(" + v.ssa + ") : (f64) -> ()");
                break;
            case VK::Bool:
                emit("\"lang.print_bool\"(" + v.ssa + ") : (i1) -> ()");
                break;
            case VK::Str:
                emit("\"lang.print_str\"(" + v.ssa + ") : (!llvm.ptr) -> ()");
                break;
            case VK::None:
                emit("\"lang.print_none\"() : () -> ()");
                break;
            case VK::Tensor:
                emit("\"lang.print_tensor\"(" + v.ssa + ") : (!llvm.ptr) -> ()");
                break;
            case VK::List:
                // 0.9.4：repr 格式与 VM stringify 逐字一致（sg_list_format）；
                // 元组元素走 lang.list_format_tuples（F-22，Dict.items()
                // 打印闭环）。
                if (v.lElem == static_cast<uint8_t>(VK::Tuple)) {
                    Value s = formatTuples(v, *c.args[k]);
                    emit("\"lang.print_str\"(" + s.ssa +
                         ") : (!llvm.ptr) -> ()");
                    break;
                }
                emit("\"lang.print_list\"(" + v.ssa + ") : (!llvm.ptr) -> ()");
                break;
            case VK::Dict: {
                // 0.9.7：print(Dict) 与 VM repr 逐字一致（sg_print_dict）。
                // 容器值槽（tag 4，64 位句柄无类型标签）运行时无法 repr，
                // 编译期显式拒绝；标量值槽（int/float/str/bool）全覆盖。
                if (auto it = types_.find(c.args[k].get());
                    it != types_.end() && it->second->kind == Type::Kind::Dict &&
                    it->second->params.size() >= 2) {
                    switch (it->second->params[1]->kind) {
                    case Type::Kind::List:
                    case Type::Kind::Dict:
                    case Type::Kind::Set:
                    case Type::Kind::Tensor:
                    case Type::Kind::Result:
                        unsupported(c.args[k]->range,
                                    "print 容器值字典（AOT 句柄槽无类型标签）");
                    default:
                        break;
                    }
                }
                emit("\"lang.print_dict\"(" + v.ssa + ") : (!llvm.ptr) -> ()");
                break;
            }
            case VK::Json:
                // 0.9.4 M 专项：紧凑 JSON 文本，与 VM stringify(Json) 逐字一致
                emit("\"lang.print_json\"(" + v.ssa + ") : (!llvm.ptr) -> ()");
                break;
            case VK::Tuple: {
                // 0.9.6：repr 格式与 VM toString(Tuple, repr) 逐字一致
                Value s = genTupleRepr(
                    v, types_.find(c.args[k].get())->second, c.args[k]->range);
                emit("\"lang.print_str\"(" + s.ssa + ") : (!llvm.ptr) -> ()");
                break;
            }
            default:
                unsupported(c.range, "print 该参数类型");
            }
        }
        emit("\"lang.print_nl\"() : () -> ()");
        return Value{VK::None, ""};
    }
    if (name == "str") {
        need(1);
        Value v = genExpr(*c.args[0]);
        switch (v.vk) {
        case VK::Int: {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_from_i64\"(" + v.ssa +
                 ") : (i64) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        }
        case VK::Float: {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_from_f64\"(" + v.ssa +
                 ") : (f64) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        }
        case VK::Bool: {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_from_bool\"(" + v.ssa +
                 ") : (i1) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        }
        case VK::Str:
            return v; // VM stringify(Str) 原样返回
        case VK::Tensor: {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_from_tensor\"(" + v.ssa +
                 ") : (!llvm.ptr) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        }
        case VK::Tuple:
            // 0.9.6：VM str(Tuple) 与 print 同为 repr 形态（元素一律 repr）
            return genTupleRepr(v, types_.find(c.args[0].get())->second,
                                c.args[0]->range);
        case VK::List:
            // 0.9.9：容器化（repr 与 VM stringify 逐字一致，sg_list_format）；
            // 元组元素走 lang.list_format_tuples（F-22，同 print）。
            if (v.lElem == static_cast<uint8_t>(VK::Tuple))
                return formatTuples(v, *c.args[0]);
            {
                std::string ssa = fresh();
                emit(ssa + " = \"lang.list_format\"(" + v.ssa +
                     ") : (!llvm.ptr) -> !llvm.ptr");
                return Value{VK::Str, ssa};
            }
        case VK::Dict: {
            // 容器值槽编译期拒绝（与 print(Dict) 同防御：句柄槽无类型标签）
            if (auto it = types_.find(c.args[0].get());
                it != types_.end() && it->second->kind == Type::Kind::Dict &&
                it->second->params.size() >= 2) {
                switch (it->second->params[1]->kind) {
                case Type::Kind::List:
                case Type::Kind::Dict:
                case Type::Kind::Set:
                case Type::Kind::Tensor:
                case Type::Kind::Result:
                    unsupported(c.args[0]->range,
                                "str 容器值字典（AOT 句柄槽无类型标签）；"
                                "请逐键 str() 后重组，或改存标量值");
                default:
                    break;
                }
            }
            {
                std::string ssa = fresh();
                emit(ssa + " = \"lang.dict_format\"(" + v.ssa +
                     ") : (!llvm.ptr) -> !llvm.ptr");
                return Value{VK::Str, ssa};
            }
        }
        case VK::Set:
            // 0.9.9：repr 与 VM stringify(Set) 逐字一致（sg_set_format）
            {
                std::string ssa = fresh();
                emit(ssa + " = \"lang.set_format\"(" + v.ssa +
                     ") : (!llvm.ptr) -> !llvm.ptr");
                return Value{VK::Str, ssa};
            }
        default:
            unsupported(c.args[0]->range, "str() 该参数类型");
        }
    }
    // fmt(expr, "spec")（0.9.9）：f-string :spec 降糖目标。spec 合法性由
    // Sema validateFmtSpec 静态保证；按值 vk 静态分派四层，RT 实现与 VM
    // fmtI64/F64/Str 逐字同构（snprintf + 码点计宽）。
    if (name == "fmt") {
        need(2);
        Value v = genExpr(*c.args[0]);
        Value sp = strConst(
            static_cast<const StrLit&>(*c.args[1]).strValue);
        std::string ssa = fresh();
        switch (v.vk) {
        case VK::Int:
            emit(ssa + " = \"lang.fmt_i64\"(" + v.ssa + ", " + sp.ssa +
                 ") : (i64, !llvm.ptr) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        case VK::Float:
            emit(ssa + " = \"lang.fmt_f64\"(" + v.ssa + ", " + sp.ssa +
                 ") : (f64, !llvm.ptr) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        case VK::Bool:
            emit(ssa + " = \"lang.fmt_bool\"(" + v.ssa + ", " + sp.ssa +
                 ") : (i1, !llvm.ptr) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        case VK::Str:
            emit(ssa + " = \"lang.fmt_str\"(" + v.ssa + ", " + sp.ssa +
                 ") : (!llvm.ptr, !llvm.ptr) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        default:
            unsupported(c.args[0]->range, "fmt() 该参数类型");
        }
    }
    // 0.9.9 类型转换/码点（panic 消息与 VM doBuiltin / sg_* 运行时逐字一致）
    if (name == "int") {
        need(1);
        Value v = genExpr(*c.args[0]);
        switch (v.vk) {
        case VK::Int:
            return v; // 恒等
        case VK::Float: {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.int_from_f64\"(" + v.ssa +
                 ") : (f64) -> i64 " + opLoc());
            return Value{VK::Int, ssa};
        }
        case VK::Bool: {
            std::string ssa = fresh();
            emit(ssa + " = arith.extui " + v.ssa + " : i1 to i64");
            return Value{VK::Int, ssa};
        }
        case VK::Str: {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_to_i64\"(" + v.ssa +
                 ") : (!llvm.ptr) -> i64 " + opLoc());
            return Value{VK::Int, ssa};
        }
        default:
            unsupported(c.args[0]->range, "int() 该参数类型");
        }
    }
    if (name == "float") {
        need(1);
        Value v = genExpr(*c.args[0]);
        switch (v.vk) {
        case VK::Float:
            return v; // 恒等
        case VK::Int: {
            std::string ssa = fresh();
            emit(ssa + " = arith.sitofp " + v.ssa + " : i64 to f64");
            return Value{VK::Float, ssa};
        }
        case VK::Bool: {
            std::string ssa = fresh();
            emit(ssa + " = arith.uitofp " + v.ssa + " : i1 to f64");
            return Value{VK::Float, ssa};
        }
        case VK::Str: {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_to_f64\"(" + v.ssa +
                 ") : (!llvm.ptr) -> f64 " + opLoc());
            return Value{VK::Float, ssa};
        }
        default:
            unsupported(c.args[0]->range, "float() 该参数类型");
        }
    }
    if (name == "ord") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, "ord() 参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.ord\"(" + v.ssa + ") : (!llvm.ptr) -> i64 " +
             opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "chr") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Int)
            unsupported(c.args[0]->range, "chr() 参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.chr\"(" + v.ssa + ") : (i64) -> !llvm.ptr " +
             opLoc());
        return Value{VK::Str, ssa};
    }
    if (name == "len") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk == VK::Str) {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_len\"(" + v.ssa + ") : (!llvm.ptr) -> i64");
            return Value{VK::Int, ssa};
        }
        if (v.vk == VK::List) {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.list_len\"(" + v.ssa + ") : (!llvm.ptr) -> i64");
            return Value{VK::Int, ssa};
        }
        if (v.vk == VK::Dict) {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.dict_len\"(" + v.ssa + ") : (!llvm.ptr) -> i64");
            return Value{VK::Int, ssa};
        }
        if (v.vk == VK::Set) {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.set_len\"(" + v.ssa + ") : (!llvm.ptr) -> i64");
            return Value{VK::Int, ssa};
        }
        if (v.vk == VK::Tuple) {
            // 0.9.6：元组句柄与 i64 List 同构，长度经 list_len 读取
            std::string ssa = fresh();
            emit(ssa + " = \"lang.list_len\"(" + v.ssa + ") : (!llvm.ptr) -> i64");
            return Value{VK::Int, ssa};
        }
        unsupported(c.args[0]->range, "len() 仅支持 str 与列表");
    }
    if (name == "now") {
        std::string ssa = fresh();
        // 能力行：挂 opLoc——sg_cap_check 拒绝时 panic 上下文与 VM 一致
        emit(ssa + " = \"lang.now\"() : () -> f64 " + opLoc());
        return Value{VK::Float, ssa};
    }
    if (name == "range") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Int) unsupported(c.args[0]->range, "range() 参数必须是 int");
        return Value{VK::Range, v.ssa}; // 负数原样存（首轮即结束）
    }
    // 一元数学内建（kUnOps 注册表驱动）
    if (const UnInfo* uop = findUnOp(name)) {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk == VK::Tensor) {
            // 逐元素：fnid 查表；fnid 0 = abs 保 dtype，其余恒 f64
            std::string ssa = fresh();
            emit(ssa + " = \"lang.tensor_un\"(" + i32Const(uop->fnid) + ", " +
                 v.ssa + ") : (i32, !llvm.ptr) -> !llvm.ptr " + opLoc());
            return Value{VK::Tensor, ssa, uop->fnid == 0 && v.tInt, v.tRank};
        }
        if (v.vk != VK::Int && v.vk != VK::Float)
            unsupported(c.args[0]->range, name + "() 仅支持数值");
        if (uop->iSym) { // abs：双路径保 dtype，不提升不挂 opLoc
            std::string ssa = fresh();
            if (v.vk == VK::Int) {
                emit(ssa + " = \"" + uop->iSym + "\"(" + v.ssa +
                     ") : (i64) -> i64");
                return Value{VK::Int, ssa};
            }
            emit(ssa + " = \"" + uop->uSym + "\"(" + v.ssa + ") : (f64) -> f64");
            return Value{VK::Float, ssa};
        }
        // sqrt/exp/log：提升 f64
        Value vf = promoteF64(v);
        std::string ssa = fresh();
        emit(ssa + " = \"" + uop->uSym + "\"(" + vf.ssa + ") : (f64) -> f64 " +
             opLoc());
        return Value{VK::Float, ssa};
    }
    // ---- v0.9.3 标量数学/随机/排序（仅标量与列表，不触碰张量路径） ----
    // 三角/双曲/atan2：int 提升 f64，恒返回 float（依 IEEE 754，不 panic）。
    if (name == "sin" || name == "cos" || name == "tan" || name == "asin" ||
        name == "acos" || name == "atan" || name == "sinh" || name == "cosh" ||
        name == "tanh") {
        need(1);
        Value vf = promoteF64(genExpr(*c.args[0]));
        std::string ssa = fresh();
        emit(ssa + " = \"lang." + name + "\"(" + vf.ssa + ") : (f64) -> f64 " +
             opLoc());
        return Value{VK::Float, ssa};
    }
    if (name == "atan2") {
        need(2);
        Value vy = promoteF64(genExpr(*c.args[0]));
        Value vx = promoteF64(genExpr(*c.args[1]));
        std::string ssa = fresh();
        emit(ssa + " = \"lang.atan2\"(" + vy.ssa + ", " + vx.ssa +
             ") : (f64, f64) -> f64 " + opLoc());
        return Value{VK::Float, ssa};
    }
    // 取整族：int 输入恒等返回（大整数经 f64 会失真，禁止绕道）；
    // float 输入 → i64，越界/NaN 由运行时 panic（挂上下文）。
    if (name == "floor" || name == "ceil" || name == "round" ||
        name == "trunc") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk == VK::Int) return v;
        if (v.vk != VK::Float)
            unsupported(c.args[0]->range, name + "() 仅支持标量数值");
        std::string ssa = fresh();
        emit(ssa + " = \"lang." + name + "\"(" + v.ssa + ") : (f64) -> i64 " +
             opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "random") {
        std::string ssa = fresh();
        emit(ssa + " = \"lang.random\"() : () -> f64");
        return Value{VK::Float, ssa};
    }
    if (name == "seed") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Int)
            unsupported(c.args[0]->range, "seed() 参数必须是 int");
        emit("\"lang.seed\"(" + v.ssa + ") : (i64) -> ()");
        return Value{VK::None, ""};
    }
    // sorted：AOT 列表恒同质（SgList etag），运行时按 etag 泛型稳定升序。
    if (name == "sorted") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::List)
            unsupported(c.args[0]->range, "sorted() 仅支持列表");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.sorted\"(" + v.ssa + ") : (!llvm.ptr) -> !llvm.ptr " +
             opLoc());
        // 元素类型透传（lElem 驱动后续 list_get_* 分派）
        return Value{VK::List, ssa, false, 0, v.lElem};
    }
    // ---- M 专项：标准库广度（统计/倒序/哈希）。统计接受张量/列表
    // （运行时结构不同各一套入口），恒返回 f64；空序列 panic 双模一致。 ----
    if (name == "mean" || name == "var" || name == "std" ||
        name == "median") {
        need(1);
        Value v = genExpr(*c.args[0]);
        std::string ssa = fresh();
        if (v.vk == VK::Tensor) {
            emit(ssa + " = \"lang.tensor_" + name + "\"(" + v.ssa +
                 ") : (!llvm.ptr) -> f64 " + opLoc());
            return Value{VK::Float, ssa};
        }
        if (v.vk == VK::List) {
            emit(ssa + " = \"lang.list_" + name + "\"(" + v.ssa +
                 ") : (!llvm.ptr) -> f64 " + opLoc());
            return Value{VK::Float, ssa};
        }
        unsupported(c.args[0]->range,
                    name + "() 仅支持张量或数值列表");
    }
    // reversed：List → 倒序新列表（元素类型透传）；str → 字节倒序。
    if (name == "reversed") {
        need(1);
        Value v = genExpr(*c.args[0]);
        std::string ssa = fresh();
        if (v.vk == VK::List) {
            emit(ssa + " = \"lang.list_reversed\"(" + v.ssa +
                 ") : (!llvm.ptr) -> !llvm.ptr");
            return Value{VK::List, ssa, false, 0, v.lElem};
        }
        if (v.vk == VK::Str) {
            emit(ssa + " = \"lang.str_reversed\"(" + v.ssa +
                 ") : (!llvm.ptr) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        }
        unsupported(c.args[0]->range, "reversed() 仅支持列表或字符串");
    }
    // sha256：str → 64 位小写 hex（FIPS 180-4）。
    if (name == "sha256") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, "sha256() 参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.sha256\"(" + v.ssa +
             ") : (!llvm.ptr) -> !llvm.ptr");
        return Value{VK::Str, ssa};
    }
    // crc32：str → 非负 int（CRC-32/ISO-HDLC，zlib 同款参数）。
    if (name == "crc32") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, "crc32() 参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.crc32\"(" + v.ssa + ") : (!llvm.ptr) -> i64");
        return Value{VK::Int, ssa};
    }
    // json_decode：str → Json 动态值（解析失败 panic 消息与 VM 逐字一致）。
    if (name == "json_decode") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, "json_decode() 参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.json_decode\"(" + v.ssa +
             ") : (!llvm.ptr) -> !llvm.ptr " + opLoc());
        return Value{VK::Json, ssa};
    }
    if (name == "sum") {
        need(1);
        Value v = genExpr(*c.args[0]);
        // List 归约（0.9.9）：AOT 列表恒同质，按注解元素类型分流 i64/f64
        // 路径（bool 走 i64，槽值 0/1 与 VM 提升一致）；op 编码 0=sum。
        if (v.vk == VK::List) {
            bool isF = false;
            if (auto lt = types_.find(c.args[0].get()); lt != types_.end() &&
                lt->second->kind == Type::Kind::List &&
                !lt->second->params.empty() &&
                lt->second->params[0]->kind == Type::Kind::Float)
                isF = true;
            Value nm = strConst(name);
            std::string ssa = fresh();
            if (isF) {
                emit(ssa + " = \"lang.list_reduce_f\"(" + i32Const(0) + ", " +
                     v.ssa + ", " + nm.ssa +
                     ") : (i32, !llvm.ptr, !llvm.ptr) -> f64 " + opLoc());
                return Value{VK::Float, ssa};
            }
            emit(ssa + " = \"lang.list_reduce_i\"(" + i32Const(0) + ", " +
                 v.ssa + ", " + nm.ssa +
                 ") : (i32, !llvm.ptr, !llvm.ptr) -> i64 " + opLoc());
            return Value{VK::Int, ssa};
        }
        if (v.vk != VK::Tensor)
            unsupported(c.args[0]->range, "sum() 仅支持张量或数值列表");
        std::string ssa = fresh();
        if (v.tInt) {
            emit(ssa + " = \"lang.tensor_sum_i\"(" + v.ssa +
                 ") : (!llvm.ptr) -> i64 " + opLoc());
            return Value{VK::Int, ssa};
        }
        // linalg 覆盖集：f64 + 形状静态 → 写形状属性（否则 LangToLLVM 兜底）
        std::string attrs;
        if (auto sh = staticF64Shape(c.args[0].get()))
            attrs = " {shape = " + shapeArrAttr(*sh) + "}";
        emit(ssa + " = \"lang.tensor_sum_f\"(" + v.ssa + ")" + attrs +
             " : (!llvm.ptr) -> f64 " + opLoc());
        return Value{VK::Float, ssa};
    }
    if (name == "max" || name == "min") {
        need(1);
        Value v = genExpr(*c.args[0]);
        // List 归约（0.9.9）：op 编码 1=min 2=max（与 sum=0 同一运行时
        // 双入口）；空序列 panic 由 sg_list_reduce_* 逐字提供。
        if (v.vk == VK::List) {
            bool isF = false;
            if (auto lt = types_.find(c.args[0].get()); lt != types_.end() &&
                lt->second->kind == Type::Kind::List &&
                !lt->second->params.empty() &&
                lt->second->params[0]->kind == Type::Kind::Float)
                isF = true;
            Value nm = strConst(name);
            std::string ssa = fresh();
            if (isF) {
                emit(ssa + " = \"lang.list_reduce_f\"(" +
                     i32Const(name == "max" ? 2 : 1) + ", " + v.ssa + ", " +
                     nm.ssa + ") : (i32, !llvm.ptr, !llvm.ptr) -> f64 " +
                     opLoc());
                return Value{VK::Float, ssa};
            }
            emit(ssa + " = \"lang.list_reduce_i\"(" +
                 i32Const(name == "max" ? 2 : 1) + ", " + v.ssa + ", " +
                 nm.ssa + ") : (i32, !llvm.ptr, !llvm.ptr) -> i64 " + opLoc());
            return Value{VK::Int, ssa};
        }
        if (v.vk != VK::Tensor)
            unsupported(c.args[0]->range,
                        name + "() 仅支持张量或 List 归约；其他类型请"
                               "逐元素循环比较");
        Value nm = strConst(name);
        std::string ssa = fresh();
        if (v.tInt) {
            emit(ssa + " = \"lang.tensor_reduce_i\"(" +
                 i32Const(name == "max" ? 1 : 0) + ", " + v.ssa + ", " + nm.ssa +
                 ") : (i32, !llvm.ptr, !llvm.ptr) -> i64 " + opLoc());
            return Value{VK::Int, ssa};
        }
        emit(ssa + " = \"lang.tensor_reduce_f\"(" +
             i32Const(name == "max" ? 1 : 0) + ", " + v.ssa + ", " + nm.ssa +
             ") : (i32, !llvm.ptr, !llvm.ptr) -> f64 " + opLoc());
        return Value{VK::Float, ssa};
    }
    if (name == "transpose") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Tensor)
            unsupported(c.args[0]->range, "transpose() 仅支持张量");
        std::string ssa = fresh();
        // 张量降级覆盖集：2D int/float + 形状静态 → 写形状属性（纯数据
        // 搬移与 dtype 无关；int 经 t_int 属性分派 i64 视图与句柄）
        std::string attrs;
        if (v.tRank == 2) {
            if (auto sh = staticShapeOf(c.args[0].get()))
                attrs = " {shape = " + shapeArrAttr(*sh) +
                        ", t_int = " + std::to_string(v.tInt ? 1 : 0) +
                        " : i32}";
        }
        emit(ssa + " = \"lang.tensor_transpose\"(" + v.ssa + ")" + attrs +
             " : (!llvm.ptr) -> !llvm.ptr " + opLoc());
        return Value{VK::Tensor, ssa, v.tInt, 2};
    }
    if (name == "reshape") {
        // 变参维度：reshape(t, d1, d2, ...)（与 VM doBuiltin 一致）
        if (c.args.size() < 2)
            unsupported(c.range, "reshape() 需要张量 + 至少 1 个维度");
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Tensor)
            unsupported(c.args[0]->range, "reshape() 首参必须是张量");
        std::vector<Value> dims;
        for (size_t k = 1; k < c.args.size(); ++k) {
            Value d = genExpr(*c.args[k]);
            if (d.vk != VK::Int)
                unsupported(c.args[k]->range, "reshape() 维度必须是 int");
            dims.push_back(d);
        }
        std::string arr = buildI64Array(dims);
        std::string ssa = fresh();
        emit(ssa + " = \"lang.tensor_reshape\"(" + v.ssa + ", " +
             intConst((int64_t)dims.size()) + ", " + arr +
             ") : (!llvm.ptr, i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
        return Value{VK::Tensor, ssa, v.tInt,
                     static_cast<uint8_t>(dims.size())};
    }
    // data_ptr（0.9.6 O 节 FFI 宽类型域）：取连续原生缓冲首地址（int 承载）。
    // str：全局常量指针就地 ptrtoint（进程期存活，与 VM intern 池同契约——
    // 地址只读透传）；张量：lang.data_ptr → sg_data_ptr（设备驻留先回拷，
    // 空张量 panic 消息与 VM doBuiltin 逐字一致）。List 不连续 tagged 数组，
    // 检查器已拦；此处白名单再拦属防御。
    if (name == "data_ptr") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk == VK::Str) {
            std::string ssa = fresh();
            emit(ssa + " = llvm.ptrtoint " + v.ssa +
                 " : !llvm.ptr to i64 " + opLoc());
            return Value{VK::Int, ssa};
        }
        if (v.vk == VK::Tensor) {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.data_ptr\"(" + v.ssa +
                 ") : (!llvm.ptr) -> i64 " + opLoc());
            return Value{VK::Int, ssa};
        }
        unsupported(c.args[0]->range,
                    "data_ptr() 仅支持 Tensor[int]/Tensor[float]/str");
    }
    // ok / err（Result 构造；检查器已强制恰好 1 参）
    if (name == "ok") {
        need(1);
        Value v = genExpr(*c.args[0]);
        std::string ssa = fresh();
        switch (v.vk) {
        case VK::Int:
            emit(ssa + " = \"lang.result_ok_i\"(" + v.ssa +
                 ") : (i64) -> !llvm.ptr");
            break;
        case VK::Float:
            emit(ssa + " = \"lang.result_ok_f\"(" + v.ssa +
                 ") : (f64) -> !llvm.ptr");
            break;
        case VK::Bool:
            emit(ssa + " = \"lang.result_ok_b\"(" + v.ssa +
                 ") : (i1) -> !llvm.ptr");
            break;
        case VK::Str:
            emit(ssa + " = \"lang.result_ok_s\"(" + v.ssa +
                 ") : (!llvm.ptr) -> !llvm.ptr");
            break;
        case VK::None:
            emit(ssa + " = \"lang.result_ok_n\"() : () -> !llvm.ptr");
            break;
        default:
            unsupported(c.args[0]->range, "ok() 该载荷类型");
        }
        return Value{VK::Result, ssa, false, 0, static_cast<uint8_t>(v.vk)};
    }
    if (name == "err") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, "err() 参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.result_err\"(" + v.ssa +
             ") : (!llvm.ptr) -> !llvm.ptr");
        return Value{VK::Result, ssa, false, 0, static_cast<uint8_t>(VK::Str)};
    }
    // F-20：panic(msg) 可调用终止——lang.panic 单一 op（kLowerings →
    // sg_panic，try/except 可捕）。恒不返回：unreachable 终结当前块后开
    // 新死块承接后续语句（同 genAssert 兜底理由，函数尾由隐式 return 收口）。
    if (name == "panic") {
        need(1);
        Value m = genExpr(*c.args[0]);
        if (m.vk != VK::Str)
            unsupported(c.args[0]->range, "panic() 参数必须是 str");
        emit("\"lang.panic\"(" + m.ssa + ") : (!llvm.ptr) -> () " + opLoc());
        terminate("llvm.unreachable");
        cur_ = newBlock();
        return Value{VK::None, ""};
    }
    unsupported(c.range, "内建 " + name + "()（该内建）");
}

} // namespace mlirgen
} // namespace aot
} // namespace scourge
