// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT MLIR 生成器 · 调用与内建
// ----------------------------------------------------------------------------
//  genCall（内建/本模块/跨模块三分派）与 genBuiltinCall（print/str/len/
//  数学内建/张量内建等全表分派）。内部分块文件，共享模型见
//  MLIRGenDetail.hpp。
// ============================================================================
#include "MLIRGenDetail.hpp"

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
        cur += suffix;
        if (!s_.registry().count(cur)) return {};
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
        // Result 消费方法（is_ok / unwrap / unwrap_or）：载荷 VK 由检查器
        // 类型表（ExprTypeMap）反查，self 先 args 后（与 VM CallMethod 一致）。
        if (auto bt = types_.find(at.base.get());
            bt != types_.end() && bt->second &&
            bt->second->kind == Type::Kind::Result) {
            const TypePtr& rt = bt->second;
            VK pv = rt->params.empty() ? VK::Bad : vkOfType(rt->params[0]);
            if (at.name == "is_ok") {
                if (!c.args.empty())
                    unsupported(c.range, "is_ok() 不需要参数");
                Value self = genExpr(*at.base);
                std::string ssa = fresh();
                emit(ssa + " = \"lang.result_is_ok\"(" + self.ssa +
                     ") : (!llvm.ptr) -> i1");
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
                // 0.9.4：repr 格式与 VM stringify 逐字一致（sg_list_format）
                emit("\"lang.print_list\"(" + v.ssa + ") : (!llvm.ptr) -> ()");
                break;
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
        default:
            unsupported(c.args[0]->range, "str() 该参数类型");
        }
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
    // ---- IO 内建（0.9.4）：input/read_text/write_text/append_text/exists/
    // list_dir。路径窄字节串；失败 panic 的内建挂 opLoc 上下文（exists 纯
    // 查询不 panic 不挂）；list_dir 元素静态记 Str（驱动 list_get_s/print）。
    if (name == "input") {
        std::string ssa = fresh();
        emit(ssa + " = \"lang.input\"() : () -> !llvm.ptr " + opLoc());
        return Value{VK::Str, ssa};
    }
    if (name == "read_text" || name == "list_dir") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, name + "() 路径参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang." + name + "\"(" + v.ssa +
             ") : (!llvm.ptr) -> !llvm.ptr " + opLoc());
        if (name == "read_text") return Value{VK::Str, ssa};
        return Value{VK::List, ssa, false, 0, static_cast<uint8_t>(VK::Str)};
    }
    if (name == "write_text" || name == "append_text") {
        need(2);
        Value p = genExpr(*c.args[0]);
        Value s = genExpr(*c.args[1]);
        if (p.vk != VK::Str)
            unsupported(c.args[0]->range, name + "() 路径参数必须是 str");
        if (s.vk != VK::Str)
            unsupported(c.args[1]->range, name + "() 内容参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang." + name + "\"(" + p.ssa + ", " + s.ssa +
             ") : (!llvm.ptr, !llvm.ptr) -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "exists") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, "exists() 路径参数必须是 str");
        std::string ssa = fresh();
        // 能力行：挂 opLoc——sg_cap_check 拒绝时 panic 上下文与 VM 一致
        emit(ssa + " = \"lang.exists\"(" + v.ssa + ") : (!llvm.ptr) -> i1 " +
             opLoc());
        return Value{VK::Bool, ssa};
    }
    // ---- 网络/睡眠内建（0.9.4 F/G 专项）：句柄不透明 I64；失败 panic 的
    // 内建挂 opLoc 上下文（与 sg_net_* 运行时消息逐字一致）；仅 http 明文。
    if (name == "net_connect") {
        need(2);
        Value h = genExpr(*c.args[0]);
        Value p = genExpr(*c.args[1]);
        if (h.vk != VK::Str)
            unsupported(c.args[0]->range, "net_connect() 主机参数必须是 str");
        if (p.vk != VK::Int)
            unsupported(c.args[1]->range, "net_connect() 端口参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.net_connect\"(" + h.ssa + ", " + p.ssa +
             ") : (!llvm.ptr, i64) -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "net_send" || name == "net_recv") {
        need(2);
        Value s = genExpr(*c.args[0]);
        Value d = genExpr(*c.args[1]);
        if (s.vk != VK::Int)
            unsupported(c.args[0]->range, name + "() 句柄参数必须是 int");
        if (name == "net_send" && d.vk != VK::Str)
            unsupported(c.args[1]->range, "net_send() 数据参数必须是 str");
        if (name == "net_recv" && d.vk != VK::Int)
            unsupported(c.args[1]->range, "net_recv() 上限参数必须是 int");
        std::string ssa = fresh();
        if (name == "net_send") {
            emit(ssa + " = \"lang.net_send\"(" + s.ssa + ", " + d.ssa +
                 ") : (i64, !llvm.ptr) -> i64 " + opLoc());
            return Value{VK::Int, ssa};
        }
        emit(ssa + " = \"lang.net_recv\"(" + s.ssa + ", " + d.ssa +
             ") : (i64, i64) -> !llvm.ptr " + opLoc());
        return Value{VK::Str, ssa};
    }
    if (name == "net_close" || name == "sleep") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Int)
            unsupported(c.args[0]->range,
                        name + "() 参数必须是 int");
        emit("\"lang." + name + "\"(" + v.ssa + ") : (i64) -> () " +
             opLoc());
        return Value{VK::None, ""};
    }
    if (name == "net_udp_send") {
        need(3);
        Value h = genExpr(*c.args[0]);
        Value p = genExpr(*c.args[1]);
        Value d = genExpr(*c.args[2]);
        if (h.vk != VK::Str)
            unsupported(c.args[0]->range, "net_udp_send() 主机参数必须是 str");
        if (p.vk != VK::Int)
            unsupported(c.args[1]->range, "net_udp_send() 端口参数必须是 int");
        if (d.vk != VK::Str)
            unsupported(c.args[2]->range, "net_udp_send() 数据参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.net_udp_send\"(" + h.ssa + ", " + p.ssa + ", " +
             d.ssa + ") : (!llvm.ptr, i64, !llvm.ptr) -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "net_http_get") {
        need(1);
        Value u = genExpr(*c.args[0]);
        if (u.vk != VK::Str)
            unsupported(c.args[0]->range, "net_http_get() URL参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.net_http_get\"(" + u.ssa +
             ") : (!llvm.ptr) -> !llvm.ptr " + opLoc());
        return Value{VK::Str, ssa};
    }
    // F/G 迭代 2：TCP 服务端与收发超时（句柄不透明 I64；失败 panic 挂
    // opLoc，与 sg_net_* 运行时消息逐字一致）。
    if (name == "net_bind") {
        need(2);
        Value h = genExpr(*c.args[0]);
        Value p = genExpr(*c.args[1]);
        if (h.vk != VK::Str)
            unsupported(c.args[0]->range, "net_bind() 主机参数必须是 str");
        if (p.vk != VK::Int)
            unsupported(c.args[1]->range, "net_bind() 端口参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.net_bind\"(" + h.ssa + ", " + p.ssa +
             ") : (!llvm.ptr, i64) -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "net_accept") {
        need(1);
        Value s = genExpr(*c.args[0]);
        if (s.vk != VK::Int)
            unsupported(c.args[0]->range, "net_accept() 句柄参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.net_accept\"(" + s.ssa + ") : (i64) -> i64 " +
             opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "net_set_timeout") {
        need(2);
        Value s = genExpr(*c.args[0]);
        Value m = genExpr(*c.args[1]);
        if (s.vk != VK::Int)
            unsupported(c.args[0]->range,
                        "net_set_timeout() 句柄参数必须是 int");
        if (m.vk != VK::Int)
            unsupported(c.args[1]->range,
                        "net_set_timeout() 毫秒参数必须是 int");
        emit("\"lang.net_set_timeout\"(" + s.ssa + ", " + m.ssa +
             ") : (i64, i64) -> () " + opLoc());
        return Value{VK::None, ""};
    }
    // ---- 系统编程内建（0.9.4 H 专项）：args/env/时间日期/进程/信号。
    // panic 的 localtime/gmtime/spawn_wait 挂 opLoc 上下文（与 sg_* 运行时
    // 消息逐字一致）；能力行（0.9.7）也一律挂 opLoc——sg_cap_check 拒绝时
    // panic 上下文与 VM panicHere 逐字一致；列表返回静态记元素 VK（驱动
    // list_get_*/print 分派）。
    if (name == "args") {
        std::string ssa = fresh();
        emit(ssa + " = \"lang.args\"() : () -> !llvm.ptr " + opLoc());
        return Value{VK::List, ssa, false, 0, static_cast<uint8_t>(VK::Str)};
    }
    if (name == "env" || name == "has_env" || name == "unset_env") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, name + "() 名称参数必须是 str");
        std::string ssa = fresh();
        if (name == "env") {
            emit(ssa + " = \"lang.env\"(" + v.ssa +
                 ") : (!llvm.ptr) -> !llvm.ptr " + opLoc());
            return Value{VK::Str, ssa};
        }
        if (name == "has_env") {
            emit(ssa + " = \"lang.has_env\"(" + v.ssa +
                 ") : (!llvm.ptr) -> i1 " + opLoc());
            return Value{VK::Bool, ssa};
        }
        emit("\"lang.unset_env\"(" + v.ssa + ") : (!llvm.ptr) -> () " +
             opLoc());
        return Value{VK::None, ""};
    }
    if (name == "set_env") {
        need(2);
        Value n = genExpr(*c.args[0]);
        Value v = genExpr(*c.args[1]);
        if (n.vk != VK::Str)
            unsupported(c.args[0]->range, "set_env() 名称参数必须是 str");
        if (v.vk != VK::Str)
            unsupported(c.args[1]->range, "set_env() 值参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.set_env\"(" + n.ssa + ", " + v.ssa +
             ") : (!llvm.ptr, !llvm.ptr) -> i1 " + opLoc());
        return Value{VK::Bool, ssa};
    }
    if (name == "epoch" || name == "epoch_ms" || name == "signal_caught") {
        std::string ssa = fresh();
        if (name == "signal_caught") {
            emit(ssa + " = \"lang.signal_caught\"() : () -> i1 " + opLoc());
            return Value{VK::Bool, ssa};
        }
        emit(ssa + " = \"lang." + name + "\"() : () -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "signal_reset") {
        emit("\"lang.signal_reset\"() : () -> () " + opLoc());
        return Value{VK::None, ""};
    }
    if (name == "localtime" || name == "gmtime") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Int)
            unsupported(c.args[0]->range, name + "() 时间参数必须是 int");
        std::string ssa = fresh();
        emit(ssa + " = \"lang." + name + "\"(" + v.ssa + ") : (i64) -> !llvm.ptr " +
             opLoc());
        return Value{VK::List, ssa, false, 0, static_cast<uint8_t>(VK::Int)};
    }
    if (name == "system") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, "system() 命令参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.system\"(" + v.ssa + ") : (!llvm.ptr) -> i64 " +
             opLoc());
        return Value{VK::Int, ssa};
    }
    if (name == "spawn_wait") {
        need(2);
        Value p = genExpr(*c.args[0]);
        Value l = genExpr(*c.args[1]);
        if (p.vk != VK::Str)
            unsupported(c.args[0]->range, "spawn_wait() 路径参数必须是 str");
        if (l.vk != VK::List)
            unsupported(c.args[1]->range, "spawn_wait() 参数列表必须是 List");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.spawn_wait\"(" + p.ssa + ", " + l.ssa +
             ") : (!llvm.ptr, !llvm.ptr) -> i64 " + opLoc());
        return Value{VK::Int, ssa};
    }
    // ---- M 专项：标准库广度（统计/倒序/哈希）。统计接受张量/列表
    // （运行时结构不同各一套入口），恒返回 f64；空序列 panic 双模一致。 ----
    if (name == "mean" || name == "var" || name == "std") {
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
        if (v.vk != VK::Tensor)
            unsupported(c.args[0]->range, "sum() 仅支持张量");
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
        if (v.vk != VK::Tensor)
            unsupported(c.args[0]->range, name + "() 仅支持张量");
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
    unsupported(c.range, "内建 " + name + "()（该内建）");
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
    // sym = 函数名（约定：Lang 函数名即动态库导出符号名）。
    std::string dll;
    if (sig.def)
        for (const auto& d : sig.def->decorators)
            if (d.name == "extern" && !d.args.empty() &&
                d.args[0]->kind == Expr::Kind::StrLit)
                dll = static_cast<const StrLit&>(*d.args[0]).strValue;
    Value dv = strConst(dll);
    Value sv = strConst(sig.name);
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
