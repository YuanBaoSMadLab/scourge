// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT MLIR 生成器 · 调用与 @extern
// ----------------------------------------------------------------------------
//  职责边界：genCall（内建/本模块/跨模块三分派）、genCrossCall/foldModuleKey
//  （跨模块点分路径折叠）与 genExternCall（@extern 变参 ABI 落地）。
//  内建全表分派见 MLIRGenBuiltin.cpp；共享模型见 MLIRGenDetail.hpp
//  （拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/AOT/MLIRGenDetail.hpp"

namespace scourge {
namespace aot {
namespace mlirgen {

// ============================================================================
// 调用
// ============================================================================

Value Gen::genCrossCall(const std::string& mod, const std::string& fname,
                        const Call& c) {
    auto mt = s_.registry().find(mod);
    if (mt == s_.registry().end())
        unsupported(c.range, "模块 '" + mod + "' 不在符号表中");
    auto f = mt->second->funcs.find(fname);
    if (f == mt->second->funcs.end())
        unsupported(c.range,
                    "模块 '" + mod + "' 无函数 '" + fname + "'");
    std::vector<Value> args;
    std::vector<VK> paramVKs;
    for (const auto& t : f->second.params) paramVKs.push_back(vkOfType(t));
    // 缺省实参（Q2）：跨模块同规则（FuncSig.def 指向定义方 AST）。
    // 变参（O 节）：实参超出定参形参即落入 ...，检查器已拦 20 上限。
    if (c.args.size() < f->second.requiredArgs ||
        (!f->second.isVariadic && c.args.size() > paramVKs.size()))
        unsupported(c.range,
                    "函数 '" + mod + "." + fname + "' 实参数量不符");
    for (const auto& a : c.args) args.push_back(genExpr(*a));
    for (size_t k = c.args.size(); k < paramVKs.size(); ++k) {
        const Param* p = nullptr;
        if (f->second.def && k < f->second.def->params.size())
            p = &f->second.def->params[k];
        if (!p || !p->defaultValue)
            unsupported(c.range,
                        "函数 '" + mod + "." + fname + "' 缺少实参与缺省值");
        args.push_back(genExpr(*p->defaultValue));
    }
    // @extern（H 专项）：跨模块同规则（函数体不发射）。
    if (f->second.isExtern) return genExternCall(f->second, args);
    return emitCallRet(funcSym(mod, fname), args, paramVKs,
                       vkOfType(f->second.ret), f->second.ret);
}

std::string Gen::foldModuleKey(const Expr& base) const {
    std::vector<std::string> trail; // Attr 链成员段（逆序：外层先收）
    const Expr* leaf = &base;
    while (leaf->kind == Expr::Kind::Attr) {
        trail.push_back(static_cast<const Attr&>(*leaf).name);
        leaf = static_cast<const Attr&>(*leaf).base.get();
    }
    if (leaf->kind != Expr::Kind::Name) return {};
    auto first = modAlias_.find(static_cast<const Name&>(*leaf).name);
    if (first == modAlias_.end()) return {};
    std::string cur = first->second;
    // 与检查器 qualifiedModuleName 同构：重写段（与真名尾段重合，源自
    // 宽容首段绑定）跳过，其余段下钻——每段全名须为已加载模块
    // （registry 与检查器 imports_ 同源），任一段缺失即交回方法分派。
    for (size_t k = trail.size(); k-- > 0;) {
        const std::string& seg = trail[k];
        std::string suffix = '.' + seg;
        if (cur.size() > suffix.size() &&
            cur.compare(cur.size() - suffix.size(), suffix.size(), suffix) ==
                0)
            continue;
        std::string prev = cur;
        cur += suffix;
        if (!s_.registry().count(cur)) {
            // F-19：同包多模块合并命名空间——下钻段缺失时回退包根（首段）
            // 重试（与检查器 qualifiedModuleName 同构同序）。
            size_t dot = prev.find('.');
            if (dot == std::string::npos) return {};
            std::string alt = prev.substr(0, dot) + suffix;
            if (!s_.registry().count(alt)) return {};
            cur = alt;
        }
    }
    return cur;
}

Value Gen::genCall(const Call& c) {
    // callee: Name（内建 / 本模块函数）或 Attr（mod.f 跨模块）。
    if (c.callee->kind == Expr::Kind::Name) {
        auto& n = static_cast<const Name&>(*c.callee);
        // 内建优先（与检查器/字节码后端同序；变量遮蔽场景检查器已报错）。
        if (isBuiltinName(n.name)) return genBuiltinCall(n.name, c);

        auto ft = s_.registry().find(curModule_);
        if (ft != s_.registry().end()) {
            auto f = ft->second->funcs.find(n.name);
            if (f != ft->second->funcs.end()) {
                std::vector<Value> args;
                std::vector<VK> paramVKs;
                for (const auto& t : f->second.params)
                    paramVKs.push_back(vkOfType(t));
                // 缺省实参（Q2）：实参个数落在 [requiredArgs, nParams]，
                // 缺失尾部实参在调用点对缺省值表达式求值（emitCallRet
                // 统一做 int→float 边界提升，与 VM constOf 一致）。
                // 变参（O 节）：实参超出定参形参即落入 ...，检查器已拦 20 上限。
                if (c.args.size() < f->second.requiredArgs ||
                    (!f->second.isVariadic &&
                     c.args.size() > paramVKs.size()))
                    unsupported(c.range, "函数 '" + n.name + "' 实参数量不符");
                for (const auto& a : c.args) args.push_back(genExpr(*a));
                for (size_t k = c.args.size(); k < paramVKs.size(); ++k) {
                    const Param* p = nullptr;
                    if (f->second.def && k < f->second.def->params.size())
                        p = &f->second.def->params[k];
                    if (!p || !p->defaultValue)
                        unsupported(c.range,
                                    "函数 '" + n.name + "' 缺少实参与缺省值");
                    args.push_back(genExpr(*p->defaultValue));
                }
                // @extern（H 专项）：函数体不发射，直接落动态库调用。
                if (f->second.isExtern) return genExternCall(f->second, args);
                return emitCallRet(funcSym(curModule_, n.name), args, paramVKs,
                                   vkOfType(f->second.ret), f->second.ret);
            }
        }
        // from-import 函数别名（0.9.6）：还原 (源模块, 源名) 后统一发射。
        if (auto ff = curFromFuncs_.find(n.name); ff != curFromFuncs_.end())
            return genCrossCall(ff->second.first, ff->second.second, c);
        unsupported(c.range, "调用未解析的函数 '" + n.name + "'");
    }
    if (c.callee->kind == Expr::Kind::Attr) {
        auto& at = static_cast<const Attr&>(*c.callee);
        // Result/Option 消费方法（is_ok|is_some / unwrap / unwrap_or）：
        // Option ≡ Result 同构（批3），载荷 VK 由检查器类型表（ExprTypeMap）
        // 反查，self 先 args 后（与 VM CallMethod 一致）。
        if (auto bt = types_.find(at.base.get());
            bt != types_.end() && bt->second &&
            (bt->second->kind == Type::Kind::Result ||
             bt->second->kind == Type::Kind::Option)) {
            const TypePtr& rt = bt->second;
            VK pv = rt->params.empty() ? VK::Bad : vkOfType(rt->params[0]);
            if (at.name == "is_ok" || at.name == "is_some") {
                if (!c.args.empty())
                    unsupported(c.range, "is_ok()/is_some() 不需要参数");
                Value self = genExpr(*at.base);
                std::string ssa = fresh();
                emit(ssa + " = \"lang.result_is_ok\"(" + self.ssa +
                     ") : (!llvm.ptr) -> i1");
                return Value{VK::Bool, ssa};
            }
            if (at.name == "is_err" || at.name == "is_none") {
                if (!c.args.empty())
                    unsupported(c.range, "is_err()/is_none() 不需要参数");
                Value self = genExpr(*at.base);
                std::string ok = fresh();
                emit(ok + " = \"lang.result_is_ok\"(" + self.ssa +
                     ") : (!llvm.ptr) -> i1");
                std::string ssa = fresh();
                emit(ssa + " = arith.xori " + ok + ", " + boolReg(true) +
                     " : i1");
                return Value{VK::Bool, ssa};
            }
            if (at.name == "unwrap" || at.name == "unwrap_or") {
                if (c.args.size() != 1)
                    unsupported(c.range,
                                at.name + "() 需要 1 个缺省值参数");
                Value self = genExpr(*at.base);
                if (pv == VK::None)
                    return Value{VK::None, ""}; // None 载荷无 SSA；ok/err 均取 None
                if (pv != VK::Int && pv != VK::Float && pv != VK::Bool &&
                    pv != VK::Str)
                    unsupported(c.range, "该载荷类型的 " + at.name + "()");
                Value def = genExpr(*c.args[0]);
                // 缺省值 int → float 载荷（与 emitCallRet 隐式转换一致）
                if (pv == VK::Float && def.vk == VK::Int) {
                    std::string f = fresh();
                    emit(f + " = arith.sitofp " + def.ssa + " : i64 to f64");
                    def = Value{VK::Float, f};
                }
                const char* sym = pv == VK::Int   ? "lang.result_unwrap_i"
                                  : pv == VK::Float ? "lang.result_unwrap_f"
                                  : pv == VK::Bool  ? "lang.result_unwrap_b"
                                                    : "lang.result_unwrap_s";
                const char* retTy =
                    pv == VK::Int ? "i64"
                    : pv == VK::Float ? "f64"
                    : pv == VK::Bool  ? "i1"
                                      : "!llvm.ptr";
                std::string ssa = fresh();
                emit(ssa + " = \"" + sym + "\"(" + self.ssa + ", " + def.ssa +
                     ") : (!llvm.ptr, " + std::string(mlirTy(pv)) + ") -> " +
                     retTy);
                return Value{pv, ssa};
            }
        }
        // str 成员方法（0.9.4）：纯函数，经 lang.str_* 降级（kLowerings）；
        // panic 消息由运行时 sg_str_* 提供（与 VM callValueMethod 逐字一致）。
        if (auto bt = types_.find(at.base.get());
            bt != types_.end() && bt->second &&
            bt->second->kind == Type::Kind::Str) {
            Value self = genExpr(*at.base);
            const std::string& nm = at.name;
            auto needArgs = [&](size_t n) {
                if (c.args.size() != n) unsupported(c.range, nm + "() 参数个数");
            };
            auto strArg = [&](size_t k) {
                Value v = genExpr(*c.args[k]);
                if (v.vk != VK::Str)
                    unsupported(c.args[k]->range, nm + "() 参数必须是 str");
                return v;
            };
            if (nm == "upper" || nm == "lower" || nm == "trim" ||
                nm == "trim_start" || nm == "trim_end") {
                const char* op = nm == "upper"   ? "lang.str_upper"
                                 : nm == "lower" ? "lang.str_lower"
                                 : nm == "trim"  ? "lang.str_trim"
                                 : nm == "trim_start" ? "lang.str_trim_start"
                                                      : "lang.str_trim_end";
                std::string ssa = fresh();
                emit(ssa + " = \"" + op + "\"(" + self.ssa +
                     ") : (!llvm.ptr) -> !llvm.ptr");
                return Value{VK::Str, ssa};
            }
            if (nm == "contains" || nm == "startswith" || nm == "endswith") {
                needArgs(1);
                Value sub = strArg(0);
                const char* op = nm == "contains"     ? "lang.str_contains"
                                 : nm == "startswith" ? "lang.str_startswith"
                                                      : "lang.str_endswith";
                std::string ssa = fresh();
                emit(ssa + " = \"" + op + "\"(" + self.ssa + ", " + sub.ssa +
                     ") : (!llvm.ptr, !llvm.ptr) -> i32 " + opLoc());
                std::string z = fresh();
                emit(z + " = arith.constant 0 : i32");
                std::string b = fresh();
                emit(b + " = arith.cmpi ne, " + ssa + ", " + z + " : i32");
                return Value{VK::Bool, b};
            }
            if (nm == "find" || nm == "index") {
                needArgs(1);
                Value sub = strArg(0);
                std::string ssa = fresh();
                emit(ssa + " = \"" +
                     (nm == "find" ? "lang.str_find" : "lang.str_index") +
                     "\"(" + self.ssa + ", " + sub.ssa +
                     ") : (!llvm.ptr, !llvm.ptr) -> i64 " + opLoc());
                return Value{VK::Int, ssa};
            }
            if (nm == "split") {
                needArgs(1);
                Value sep = strArg(0);
                std::string ssa = fresh();
                emit(ssa + " = \"lang.str_split\"(" + self.ssa + ", " +
                     sep.ssa + ") : (!llvm.ptr, !llvm.ptr) -> !llvm.ptr " +
                     opLoc());
                // 元素 VK 静态记 Str（后续 list_get_s / print 分派）
                return Value{VK::List, ssa, false, 0,
                             static_cast<uint8_t>(VK::Str)};
            }
            if (nm == "replace") {
                needArgs(2);
                Value o = strArg(0);
                Value nw = strArg(1);
                std::string ssa = fresh();
                emit(ssa + " = \"lang.str_replace\"(" + self.ssa + ", " +
                     o.ssa + ", " + nw.ssa +
                     ") : (!llvm.ptr, !llvm.ptr, !llvm.ptr) -> !llvm.ptr " +
                     opLoc());
                return Value{VK::Str, ssa};
            }
            if (nm == "join") {
                needArgs(1);
                Value parts = genExpr(*c.args[0]);
                if (parts.vk != VK::List)
                    unsupported(c.args[0]->range,
                                "join() 参数必须是 List[str]");
                std::string ssa = fresh();
                emit(ssa + " = \"lang.str_join\"(" + self.ssa + ", " +
                     parts.ssa + ") : (!llvm.ptr, !llvm.ptr) -> !llvm.ptr " +
                     opLoc());
                return Value{VK::Str, ssa};
            }
            if (nm == "slice") {
                needArgs(2);
                Value a = genExpr(*c.args[0]);
                Value b = genExpr(*c.args[1]);
                if (a.vk != VK::Int || b.vk != VK::Int)
                    unsupported(c.args[0]->range,
                                "slice() 参数必须是 int");
                std::string ssa = fresh();
                emit(ssa + " = \"lang.str_slice\"(" + self.ssa + ", " +
                     a.ssa + ", " + b.ssa +
                     ") : (!llvm.ptr, i64, i64) -> !llvm.ptr " + opLoc());
                return Value{VK::Str, ssa};
            }
            if (nm == "count") {
                // 0.9.9：子串非重叠计数（空子串 = len+1，与 VM/Python 一致）；
                // 纯函数无 panic（kLowerings 无上下文条目）。
                needArgs(1);
                Value sub = strArg(0);
                std::string ssa = fresh();
                emit(ssa + " = \"lang.str_count\"(" + self.ssa + ", " +
                     sub.ssa + ") : (!llvm.ptr, !llvm.ptr) -> i64");
                return Value{VK::Int, ssa};
            }
        }
        // List 成员方法（0.9.4）：contains 复用 list_in_*（与 'in' 同降级）；
        // index/count 走 64 位槽 tag ABI（dict 键编码一致，运行时按 etag 匹配）。
        if (auto bt = types_.find(at.base.get());
            bt != types_.end() && bt->second &&
            bt->second->kind == Type::Kind::List &&
            !bt->second->params.empty()) {
            VK evk = vkOfType(bt->second->params[0]);
            Value self = genExpr(*at.base);
            if (at.name == "contains") {
                if (c.args.size() != 1)
                    unsupported(c.range, "contains() 参数个数");
                Value key = genExpr(*c.args[0]);
                switch (evk) {
                case VK::Int: {
                    if (key.vk != VK::Int)
                        unsupported(c.args[0]->range,
                                    "contains() 键类型与列表元素不符");
                    std::string ssa = fresh();
                    emit(ssa + " = \"lang.list_in_i\"(" + self.ssa + ", " +
                         key.ssa + ") : (!llvm.ptr, i64) -> i1");
                    return Value{VK::Bool, ssa};
                }
                case VK::Float: {
                    if (key.vk == VK::Int) key = promoteF64(key);
                    else if (key.vk != VK::Float)
                        unsupported(c.args[0]->range,
                                    "contains() 键类型与列表元素不符");
                    std::string ssa = fresh();
                    emit(ssa + " = \"lang.list_in_f\"(" + self.ssa + ", " +
                         key.ssa + ") : (!llvm.ptr, f64) -> i1");
                    return Value{VK::Bool, ssa};
                }
                case VK::Bool: {
                    if (key.vk != VK::Bool)
                        unsupported(c.args[0]->range,
                                    "contains() 键类型与列表元素不符");
                    std::string ssa = fresh();
                    emit(ssa + " = \"lang.list_in_b\"(" + self.ssa + ", " +
                         key.ssa + ") : (!llvm.ptr, i1) -> i1");
                    return Value{VK::Bool, ssa};
                }
                case VK::Str: {
                    if (key.vk != VK::Str)
                        unsupported(c.args[0]->range,
                                    "contains() 键类型与列表元素不符");
                    std::string ssa = fresh();
                    emit(ssa + " = \"lang.list_in_s\"(" + self.ssa + ", " +
                         key.ssa + ") : (!llvm.ptr, !llvm.ptr) -> i1");
                    return Value{VK::Bool, ssa};
                }
                default:
                    unsupported(c.range, "该元素类型的 contains()");
                }
            }
            if (at.name == "append") {
                // List.append(v)：原地尾插，返回 None（非纯）。元素域
                // str/int/float/bool（0.9.8 标量全域）——C 运行时按 etag
                // 分四入口；float 列表收 int 实参先提升（与 'in'/contains
                // 同一唯一放宽）；空列表首插定型 etag（运行时内部簿记）。
                if (c.args.size() != 1)
                    unsupported(c.range, "append() 参数个数");
                Value v = genExpr(*c.args[0]);
                std::string ssa = fresh();
                switch (evk) {
                case VK::Str:
                    if (v.vk != VK::Str)
                        unsupported(c.args[0]->range,
                                    "append() 实参类型与列表元素不符");
                    emit("\"lang.list_append\"(" + self.ssa + ", " + v.ssa +
                         ") : (!llvm.ptr, !llvm.ptr) -> () " + opLoc());
                    break;
                case VK::Int:
                    if (v.vk != VK::Int)
                        unsupported(c.args[0]->range,
                                    "append() 实参类型与列表元素不符");
                    emit("\"lang.list_append_i\"(" + self.ssa + ", " + v.ssa +
                         ") : (!llvm.ptr, i64) -> () " + opLoc());
                    break;
                case VK::Float:
                    if (v.vk == VK::Int) v = promoteF64(v);
                    else if (v.vk != VK::Float)
                        unsupported(c.args[0]->range,
                                    "append() 实参类型与列表元素不符");
                    emit("\"lang.list_append_f\"(" + self.ssa + ", " + v.ssa +
                         ") : (!llvm.ptr, f64) -> () " + opLoc());
                    break;
                case VK::Bool:
                    if (v.vk != VK::Bool)
                        unsupported(c.args[0]->range,
                                    "append() 实参类型与列表元素不符");
                    emit("\"lang.list_append_b\"(" + self.ssa + ", " + v.ssa +
                         ") : (!llvm.ptr, i1) -> () " + opLoc());
                    break;
                case VK::Tuple:
                    // 批 9 F-21：元组句柄槽——ptrtoint 存 i64（与 items()/
                    // List[Tuple] 字面量同构，下标/for-in 按 lElem 还原）
                    if (v.vk != VK::Tuple)
                        unsupported(c.args[0]->range,
                                    "append() 实参类型与列表元素不符");
                    {
                        std::string b = fresh();
                        emit(b + " = llvm.ptrtoint " + v.ssa +
                             " : !llvm.ptr to i64");
                        emit("\"lang.list_append_i\"(" + self.ssa + ", " + b +
                             ") : (!llvm.ptr, i64) -> () " + opLoc());
                    }
                    break;
                default:
                    unsupported(c.range, "append() 仅支持标量元素列表");
                }
                return Value{VK::None, ""};
            }
            if (at.name == "index" || at.name == "count") {
                if (c.args.size() != 1)
                    unsupported(c.range, at.name + "() 参数个数");
                Value key = genExpr(*c.args[0]);
                int tag = 0;
                switch (evk) {
                case VK::Int:
                    if (key.vk != VK::Int)
                        unsupported(c.args[0]->range,
                                    at.name + "() 键类型与列表元素不符");
                    tag = 0;
                    break;
                case VK::Float:
                    // int 键先提升（与 'in' 同唯一放宽），位型入 64 位槽
                    if (key.vk == VK::Int) key = promoteF64(key);
                    else if (key.vk != VK::Float)
                        unsupported(c.args[0]->range,
                                    at.name + "() 键类型与列表元素不符");
                    tag = 1;
                    break;
                case VK::Str:
                    if (key.vk != VK::Str)
                        unsupported(c.args[0]->range,
                                    at.name + "() 键类型与列表元素不符");
                    tag = 2;
                    break;
                case VK::Bool:
                    if (key.vk != VK::Bool)
                        unsupported(c.args[0]->range,
                                    at.name + "() 键类型与列表元素不符");
                    tag = 3;
                    break;
                default:
                    unsupported(c.range, "该元素类型的 " + at.name + "()");
                }
                std::string bits = dictToBits(key);
                std::string ssa = fresh();
                emit(ssa + " = \"" +
                     (at.name == "index" ? "lang.list_index"
                                         : "lang.list_count") +
                     "\"(" + self.ssa + ", " + i32Const(tag) + ", " + bits +
                     ") : (!llvm.ptr, i32, i64) -> i64 " + opLoc());
                return Value{VK::Int, ssa};
            }
        }
        // Dict 成员方法（0.9.9）：keys/values 只读视图 + items 元组化 +
        // get 带缺省，经 lang.dict_* 降级（kLowerings）；panic 消息由运行时
        // sg_dict_* 提供（与 VM callValueMethod 逐字一致）。键/值标量域
        // （检查器限域），64 位槽 ABI 与 d[k]=v 一致（int 对 float 域先提升）。
        if (auto bt = types_.find(at.base.get());
            bt != types_.end() && bt->second &&
            bt->second->kind == Type::Kind::Dict &&
            bt->second->params.size() >= 2) {
            Value self = genExpr(*at.base);
            const std::string& nm = at.name;
            VK kvk = vkOfType(bt->second->params[0]);
            VK vvk = vkOfType(bt->second->params[1]);
            if (nm == "keys" || nm == "values") {
                if (c.args.size() != 0)
                    unsupported(c.range, nm + "() 参数个数");
                std::string ssa = fresh();
                emit(ssa + " = \"lang.dict_" +
                     (nm == "keys" ? std::string("keys") : std::string("values")) +
                     "\"(" + self.ssa + ") : (!llvm.ptr) -> !llvm.ptr " +
                     opLoc());
                return Value{VK::List, ssa, false, 0,
                             static_cast<uint8_t>(nm == "keys" ? kvk : vvk)};
            }
            if (nm == "items") {
                if (c.args.size() != 0)
                    unsupported(c.range, "items() 参数个数");
                std::string ssa = fresh();
                emit(ssa + " = \"lang.dict_items\"(" + self.ssa +
                     ") : (!llvm.ptr) -> !llvm.ptr " + opLoc());
                // 元素 VK 静态记 Tuple：2 元组句柄位存 i64 列表槽，
                // 下标/for-in 侧 inttoptr 还原（与 genTupleLit 同形态）
                return Value{VK::List, ssa, false, 0,
                             static_cast<uint8_t>(VK::Tuple)};
            }
            if (nm == "contains") {
                if (c.args.size() != 1)
                    unsupported(c.range, "contains() 参数个数");
                Value k = genExpr(*c.args[0]);
                if (kvk == VK::Float && k.vk == VK::Int) k = promoteF64(k);
                else if (k.vk != kvk)
                    unsupported(c.args[0]->range,
                                "contains() 键类型与字典不符");
                std::string ssa = fresh();
                emit(ssa + " = \"lang.dict_in\"(" + self.ssa + ", " +
                     dictToBits(k) + ") : (!llvm.ptr, i64) -> i1 " + opLoc());
                return Value{VK::Bool, ssa};
            }
            if (nm == "get") {
                // 缺键取缺省值不 panic（VM DictGet 同哲学）；键/值按
                // 注解域提升位型，返回值按值域还原。
                if (c.args.size() != 2)
                    unsupported(c.range, "get() 参数个数");
                Value k = genExpr(*c.args[0]);
                if (kvk == VK::Float && k.vk == VK::Int) k = promoteF64(k);
                else if (k.vk != kvk)
                    unsupported(c.args[0]->range, "get() 键类型与字典不符");
                Value dv = genExpr(*c.args[1]);
                if (vvk == VK::Float && dv.vk == VK::Int)
                    dv = promoteF64(dv);
                else if (dv.vk != vvk)
                    unsupported(c.args[1]->range,
                                "get() 缺省值类型与字典值不符");
                std::string ssa = fresh();
                emit(ssa + " = \"lang.dict_get_or\"(" + self.ssa + ", " +
                     dictToBits(k) + ", " + dictToBits(dv) +
                     ") : (!llvm.ptr, i64, i64) -> i64 " + opLoc());
                return dictFromBits(vvk, ssa);
            }
        }
        // Set 成员方法（0.9.9）：add 原地插入返回 None（与 VM SetAdd 一致；
        // NaN panic 消息由运行时 sg_set_add 逐字提供）。
        if (auto bt = types_.find(at.base.get());
            bt != types_.end() && bt->second &&
            bt->second->kind == Type::Kind::Set) {
            Value self = genExpr(*at.base);
            if (at.name == "contains") {
                if (c.args.size() != 1)
                    unsupported(c.range, "contains() 参数个数");
                VK evk = bt->second->params.empty()
                             ? VK::Bad
                             : vkOfType(bt->second->params[0]);
                Value v = genExpr(*c.args[0]);
                if (evk == VK::Float && v.vk == VK::Int) v = promoteF64(v);
                else if (v.vk != evk)
                    unsupported(c.args[0]->range,
                                "contains() 元素类型与集合不符");
                std::string ssa = fresh();
                emit(ssa + " = \"lang.set_in\"(" + self.ssa + ", " +
                     dictToBits(v) + ") : (!llvm.ptr, i64) -> i1 " + opLoc());
                return Value{VK::Bool, ssa};
            }
            if (at.name == "add") {
                if (c.args.size() != 1)
                    unsupported(c.range, "add() 参数个数");
                Value v = genExpr(*c.args[0]);
                emit("\"lang.set_add\"(" + self.ssa + ", " + dictToBits(v) +
                     ") : (!llvm.ptr, i64) -> () " + opLoc());
                return Value{VK::None, ""};
            }
        }
        // Json 成员方法（0.9.4 M 专项）：动态 JSON 值只读面，经 lang.json_*
        // 降级（kLowerings）；panic 消息由运行时 sg_json_* 提供（与 VM
        // jsonMethod 逐字一致）。形态 tag 序：0=null 1=bool 2=int 3=float
        // 4=str 5=arr 6=obj（与 SgJson/JsonData::JK 同序）。
        if (auto bt = types_.find(at.base.get());
            bt != types_.end() && bt->second &&
            bt->second->kind == Type::Kind::Json) {
            Value self = genExpr(*at.base);
            const std::string& nm = at.name;
            auto needJ = [&](size_t n) {
                if (c.args.size() != n) unsupported(c.range, nm + "() 参数个数");
            };
            if (nm == "is_null" || nm == "is_bool" || nm == "is_int" ||
                nm == "is_float" || nm == "is_str" || nm == "is_list" ||
                nm == "is_obj") {
                needJ(0);
                int tag = nm == "is_null" ? 0
                          : nm == "is_bool" ? 1
                          : nm == "is_int" ? 2
                          : nm == "is_float" ? 3
                          : nm == "is_str" ? 4
                          : nm == "is_list" ? 5
                                            : 6;
                std::string ssa = fresh();
                emit(ssa + " = \"lang.json_is\"(" + self.ssa + ", " +
                     i32Const(tag) + ") : (!llvm.ptr, i32) -> i1");
                return Value{VK::Bool, ssa};
            }
            if (nm == "as_int" || nm == "as_float" || nm == "as_bool" ||
                nm == "as_str") {
                needJ(0);
                const char* op = nm == "as_int"   ? "lang.json_as_int"
                                 : nm == "as_float" ? "lang.json_as_float"
                                 : nm == "as_bool"  ? "lang.json_as_bool"
                                                    : "lang.json_as_str";
                const char* ret = nm == "as_int"   ? "i64"
                                  : nm == "as_float" ? "f64"
                                  : nm == "as_bool"  ? "i1"
                                                     : "!llvm.ptr";
                std::string ssa = fresh();
                emit(ssa + " = \"" + op + "\"(" + self.ssa + ") : (!llvm.ptr) -> " +
                     ret + " " + opLoc());
                VK rvk = nm == "as_int"   ? VK::Int
                         : nm == "as_float" ? VK::Float
                         : nm == "as_bool"  ? VK::Bool
                                            : VK::Str;
                return Value{rvk, ssa};
            }
            if (nm == "at") {
                needJ(1);
                Value v = genExpr(*c.args[0]);
                if (v.vk != VK::Int)
                    unsupported(c.args[0]->range, "at() 数组下标必须是 int");
                std::string ssa = fresh();
                emit(ssa + " = \"lang.json_get_idx\"(" + self.ssa + ", " +
                     v.ssa + ") : (!llvm.ptr, i64) -> !llvm.ptr " + opLoc());
                return Value{VK::Json, ssa};
            }
            if (nm == "get") {
                needJ(1);
                Value v = genExpr(*c.args[0]);
                if (v.vk != VK::Str)
                    unsupported(c.args[0]->range, "get() 对象键必须是 str");
                std::string ssa = fresh();
                emit(ssa + " = \"lang.json_get_key\"(" + self.ssa + ", " +
                     v.ssa + ") : (!llvm.ptr, !llvm.ptr) -> !llvm.ptr");
                return Value{VK::Json, ssa};
            }
            if (nm == "len") {
                needJ(0);
                std::string ssa = fresh();
                emit(ssa + " = \"lang.json_len\"(" + self.ssa +
                     ") : (!llvm.ptr) -> i64 " + opLoc());
                return Value{VK::Int, ssa};
            }
            if (nm == "keys") {
                needJ(0);
                std::string ssa = fresh();
                emit(ssa + " = \"lang.json_keys\"(" + self.ssa +
                     ") : (!llvm.ptr) -> !llvm.ptr " + opLoc());
                return Value{VK::List, ssa, false, 0,
                             static_cast<uint8_t>(VK::Str)};
            }
            if (nm == "dump") {
                needJ(0);
                std::string ssa = fresh();
                emit(ssa + " = \"lang.json_dump\"(" + self.ssa +
                     ") : (!llvm.ptr) -> !llvm.ptr");
                return Value{VK::Str, ssa};
            }
        }
        // 跨模块调用（0.9.6）：m.f() / a.b.f() / 别名.f() / from 子模块——
        // 属性链折叠为规范模块名后统一发射；未命中交回成员方法报错路径。
        std::string qm = foldModuleKey(*at.base);
        if (!qm.empty()) return genCrossCall(qm, at.name, c);
        unsupported(c.range, "Result 等方法调用（is_ok / unwrap / unwrap_or）");
    }
    unsupported(c.range, "该调用形式");
}

// ============================================================================
// @extern（H 专项）：lang.extern_call 落地
// ----------------------------------------------------------------------------
// 函数体不发射（genFunction 跳过）；调用点直接落变参 op：dll/sym 以模块级
// 字符串常量锚定（strConst），实参按 MLIR 静态类型直传（int/float/bool/str
// → i64/f64/i1/ptr，MS x64 ABI 分配由 LLVM 后端处理）。str 返回的深拷贝在
// 降级层（LangToLLVM sg_extern_wrap_str）；panic 上下文挂调用点位置。
// ============================================================================

Value Gen::genExternCall(const FuncSig& sig, const std::vector<Value>& args) {
    // dll 名取自 @extern 装饰器首参数（检查器已校验恰一 StrLit）；
    // sym = symbol= 关键字参数（链接别名），缺省 = 函数名（历史约定）。
    // 与 BC/VMExtern.cpp 解析同源：三处（查符号/缓存键/panic 消息）同值。
    std::string dll;
    std::string sym = sig.name;
    if (sig.def)
        for (const auto& d : sig.def->decorators)
            if (d.name == "extern") {
                if (!d.args.empty() &&
                    d.args[0]->kind == Expr::Kind::StrLit)
                    dll = static_cast<const StrLit&>(*d.args[0]).strValue;
                for (const auto& kv : d.kwargs)
                    if (kv.first == "symbol" &&
                        kv.second->kind == Expr::Kind::StrLit)
                        sym = static_cast<const StrLit&>(*kv.second).strValue;
            }
    Value dv = strConst(dll);
    Value sv = strConst(sym);
    // variadic（O 节）：变参声明标记随 op 下传——降级层据此为间接 call
    // 挂 var_callee_type（LLVM 按 MS x64 varargs 发射，与 VM 位置式装载
    // 双模一致）；定参 extern 不带标记，走定参 ABI。
    std::string attrs = sig.isVariadic ? " {variadic}" : "";
    std::string ops = dv.ssa + ", " + sv.ssa;
    std::string tys = "!llvm.ptr, !llvm.ptr";
    for (const Value& a : args) {
        ops += ", " + a.ssa;
        tys += ", " + std::string(mlirTy(a.vk));
    }
    VK rvk = vkOfType(sig.ret);
    if (rvk == VK::None) {
        emit("\"lang.extern_call\"(" + ops + ")" + attrs + " : (" + tys +
             ") -> () " + opLoc());
        return Value{VK::None, ""};
    }
    std::string ssa = fresh();
    emit(ssa + " = \"lang.extern_call\"(" + ops + ")" + attrs + " : (" +
         tys + ") -> " + mlirTy(rvk) + " " + opLoc());
    return Value{rvk, ssa};
}

} // namespace mlirgen
} // namespace aot
} // namespace scourge
