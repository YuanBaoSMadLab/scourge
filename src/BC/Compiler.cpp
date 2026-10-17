// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 字节码编译器实现（SG-AST → SG-BC）— 驱动：辅助与模块装配
// ----------------------------------------------------------------------------
//  职责边界：构造/错误/行号、函数表与常量池/全局槽（declareFunc/addConst/
//  addGlobal）、指令发射原语（emit/emitJump/patchJump）、compileModule
//  （模块级执行 + main 装配，CompileAbort 捕获点）。
//  分块：表达式 CompilerExpr.cpp、语句 CompilerStmt.cpp、内部共享辅助
//  include/scourge/BC/CompilerDetail.hpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/BC/Compiler.hpp"
#include "scourge/BC/CompilerDetail.hpp"

#include <stdexcept>

namespace scourge {
namespace bc {

Compiler::Compiler(const SourceMgr& src, DiagEngine& diag,
                   const ModuleRegistry& registry,
                   const std::unordered_set<const Expr*>* promoteF64)
    : src_(src), diag_(diag), registry_(registry), promoteF64_(promoteF64) {}

void Compiler::error(const SourceRange& r, const std::string& msg) {
    diag_.error(Errc::E1007_UnsupportedFeature, file_, r,
                "字节码后端暂不支持：" + msg);
    throw CompileAbort{};
}

/// 张量字面量 → 常量池 Tensor 值（行主序展平；dtype：含任一浮点字面量
/// → f64，否则 i64）。各行形状不一致由检查器先行拒绝，此处兜底报错。
Value Compiler::constTensorOf(const ListLit& ll) {
    TensorLitAcc acc;
    acc.isInt = !tensorLitHasFloat(ll);
    if (!fillTensorLit(ll, 0, acc))
        error(ll.range, "张量字面量各行形状不一致");
    auto td = std::make_shared<TensorData>();
    td->isInt = acc.isInt;
    td->shape = std::move(acc.shape);
    if (acc.isInt)
        td->i64 = std::move(acc.i64);
    else
        td->f64 = std::move(acc.f64);
    return Value::makeTensor(std::move(td));
}

int Compiler::lineOf(const SourceRange& r) const {
    return static_cast<int>(src_.lineCol(file_, r.offset).line);
}

// ----------------------------------------------------------------------------
// 函数表与符号
// ----------------------------------------------------------------------------

int Compiler::declareFunc(const std::string& module, const std::string& name,
                          uint32_t paramCount, uint32_t requiredArgs,
                          std::vector<Value> defaults, bool isAsync) {
    FuncBC f;
    f.name = name;
    f.module = module;
    f.paramCount = paramCount;
    f.requiredArgs = requiredArgs;
    f.defaults = std::move(defaults);
    f.isAsync = isAsync;
    program_.funcs.push_back(std::move(f));
    return static_cast<int>(program_.funcs.size()) - 1;
}

int Compiler::lookupModuleFunc(const std::string& module,
                               const std::string& name) const {
    for (size_t i = 0; i < program_.funcs.size(); ++i) {
        const FuncBC& f = program_.funcs[i];
        if (!f.isTopLevel && f.module == module && f.name == name) return static_cast<int>(i);
    }
    return -1;
}

int Compiler::builtinIndex(const std::string& name) const {
    for (size_t i = 0; i < program_.funcs.size(); ++i) {
        const FuncBC& f = program_.funcs[i];
        if (f.isBuiltin && f.builtinName == name) return static_cast<int>(i);
    }
    return -1;
}

int Compiler::lookupFunc(const std::string& name) const {
    auto it = visibleFuncs_.find(name);
    return it == visibleFuncs_.end() ? -1 : it->second;
}

std::string Compiler::foldModuleKey(const Expr& base) const {
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
        if (!registry_.count(cur)) {
            // F-19：同包多模块合并命名空间——下钻段缺失时回退包根（首段）
            // 重试（与检查器 qualifiedModuleName 同构同序）。
            size_t dot = prev.find('.');
            if (dot == std::string::npos) return {};
            std::string alt = prev.substr(0, dot) + suffix;
            if (!registry_.count(alt)) return {};
            cur = alt;
        }
    }
    return cur;
}

// ----------------------------------------------------------------------------
// 常量与全局
// ----------------------------------------------------------------------------

uint32_t Compiler::addConst(Value v) {
    // 相同字面量去重（int/float/bool/str 精确匹配），程序紧凑且确定。
    for (size_t i = 0; i < program_.constants.size(); ++i) {
        const Value& c = program_.constants[i];
        if (c.kind != v.kind) continue;
        switch (v.kind) {
        case Value::Kind::Int:
            if (c.i == v.i) return static_cast<uint32_t>(i);
            break;
        case Value::Kind::Float:
            if (c.f == v.f) return static_cast<uint32_t>(i);
            break;
        case Value::Kind::Bool:
            if (c.b == v.b) return static_cast<uint32_t>(i);
            break;
        case Value::Kind::Str:
            if (c.s == v.s) return static_cast<uint32_t>(i);
            break;
        default:
            break;
        }
    }
    program_.constants.push_back(std::move(v));
    return static_cast<uint32_t>(program_.constants.size() - 1);
}

uint32_t Compiler::addGlobal(const std::string& module, const std::string& name) {
    for (size_t i = 0; i < program_.globalNames.size(); ++i) {
        if (program_.globalNames[i].first == module &&
            program_.globalNames[i].second == name)
            return static_cast<uint32_t>(i);
    }
    program_.globalNames.emplace_back(module, name);
    return static_cast<uint32_t>(program_.globalNames.size() - 1);
}

Value Compiler::constOf(const Expr& e) const {
    switch (e.kind) {
    case Expr::Kind::IntLit:
        // 缺省值边界 int→float 提升（Q3：浮点参数的整数字面量缺省值，
        // 与 AOT 调用点 sitofp 一致）。
        if (needsF64(e))
            return Value::makeFloat(
                static_cast<double>(static_cast<const IntLit&>(e).value));
        return Value::makeInt(static_cast<const IntLit&>(e).value);
    case Expr::Kind::FloatLit:
        return Value::makeFloat(static_cast<const FloatLit&>(e).value);
    case Expr::Kind::BoolLit:
        return Value::makeBool(static_cast<const BoolLit&>(e).boolValue);
    case Expr::Kind::StrLit:
        return Value::makeStr(static_cast<const StrLit&>(e).strValue);
    case Expr::Kind::NoneLit:
        return Value::makeNone();
    case Expr::Kind::TupleLit: {
        // 缺省参数元组常量：逐元素递归求值（元素限标量/嵌套元组，
        // 检查器 checkTupleLit 白名单已保证可常量化）。
        auto& tl = static_cast<const TupleLit&>(e);
        auto vec = std::make_shared<std::vector<Value>>();
        vec->reserve(tl.elems.size());
        for (const auto& el : tl.elems) vec->push_back(constOf(*el));
        return Value::makeTuple(std::move(vec));
    }
    case Expr::Kind::Unary: {
        // bc::Unary 是操作码枚举，AST 节点需显式限定 scourge::Unary。
        auto& u = static_cast<const scourge::Unary&>(e);
        Value v = constOf(*u.operand);
        if (u.op == "-" && v.kind == Value::Kind::Int) {
            v.i = -v.i;
            return v;
        }
        if (u.op == "-" && v.kind == Value::Kind::Float) {
            v.f = -v.f;
            return v;
        }
        return v; // "+" 原样
    }
    default:
        return Value::makeNone();
    }
}

// ----------------------------------------------------------------------------
// 指令发射
// ----------------------------------------------------------------------------

void Compiler::emit(Op op, uint32_t a, uint32_t b) {
    fn_->code.push_back(Instr{op, a, b});
}

void Compiler::markLine(uint32_t line) {
    auto& lines = fn_->lines;
    if (!lines.empty() && lines.back().first == here()) {
        lines.back().second = line;
        return;
    }
    if (!lines.empty() && lines.back().second == line) return;
    lines.emplace_back(here(), line);
}

uint32_t Compiler::emitJump(Op op) {
    emit(op, 0);
    return here() - 1;
}

void Compiler::patchJump(uint32_t at) { fn_->code[at].a = here(); }

void Compiler::patchTo(uint32_t at, uint32_t target) { fn_->code[at].a = target; }

void Compiler::unsupported(const Expr& e, const std::string& what) {
    error(e.range, what);
}

// ----------------------------------------------------------------------------
// 模块编译
// ----------------------------------------------------------------------------

bool Compiler::compileModule(const Module& m) {
    try {
        file_ = m.file;
        moduleName_ = m.name;

    // 声明遍 0：内建（幂等——多模块共享同一 Program 时只注册一次）。
    for (const char* b : builtinNames) {
        if (builtinIndex(b) >= 0) continue;
        FuncBC f;
        f.name = b;
        f.module = "";
        f.isBuiltin = true;
        f.builtinName = b;
        program_.funcs.push_back(std::move(f));
    }

    // 声明遍 1：import 模块的函数与全局 → 跨模块解析表（0.9.6 L 节）。
    // importedFuncs_ 只存规范全名键；首段/末段宽容/别名等绑定键经
    // modAlias_ 映射到规范名（与检查器"绑定名-真名解耦"同构）。
    importedFuncs_.clear();
    visibleFuncs_.clear();
    modAlias_.clear();
    fromGlobals_.clear();
    for (const char* b : builtinNames)
        visibleFuncs_.emplace(b, builtinIndex(b));
    // 确保模块 full 的函数表就绪（占位声明复用既有条目）；模块不在
    // registry 时注册空表——调用点由检查器先行拒绝，此处不重复诊断。
    auto ensureImported = [&](const std::string& full)
        -> const std::map<std::string, int>& {
        auto found = importedFuncs_.find(full);
        if (found != importedFuncs_.end()) return found->second;
        std::map<std::string, int> into;
        if (auto it = registry_.find(full); it != registry_.end())
            for (const auto& [fname, sig] : it->second->funcs) {
                int idx = lookupModuleFunc(full, fname);
                if (idx < 0) {
                    idx = declareFunc(full, fname,
                                      static_cast<uint32_t>(sig.params.size()),
                                      static_cast<uint32_t>(sig.requiredArgs),
                                      {}, sig.def ? sig.def->isAsync : false);
                    program_.funcs[static_cast<size_t>(idx)].nlocals =
                        0; // 声明占位；体由其模块编译
                }
                into.emplace(fname, idx);
            }
        return importedFuncs_.emplace(full, std::move(into)).first->second;
    };
    for (const auto& stmt : m.body) {
        if (stmt->kind != Stmt::Kind::Import) continue;
        auto& im = static_cast<const Import&>(*stmt);
        const std::string full = im.module();
        if (im.isFrom) {
            // from <mod> import x [as y]：解析序与检查器一致
            // （函数 → 全局 → 子模块）；全未命中由检查器报 E5003。
            const auto& funcs = ensureImported(full);
            auto rit = registry_.find(full);
            for (const auto& item : im.items) {
                const std::string& bind =
                    item.alias.empty() ? item.name : item.alias;
                if (auto fit = funcs.find(item.name); fit != funcs.end()) {
                    visibleFuncs_.emplace(bind, fit->second);
                    continue;
                }
                if (rit != registry_.end() &&
                    rit->second->globals.count(item.name)) {
                    fromGlobals_.emplace(bind,
                                         std::make_pair(full, item.name));
                    continue;
                }
                std::string sub = full + "." + item.name;
                if (registry_.count(sub)) {
                    ensureImported(sub);
                    modAlias_.emplace(bind, sub);
                }
            }
            continue;
        }
        ensureImported(full);
        modAlias_.emplace(full, full);
        if (!im.alias.empty()) {
            // import a.b as c：仅别名绑定（Python 语义）。
            modAlias_.emplace(im.alias, full);
            continue;
        }
        // Python 式首段绑定 + 末段宽容（AI 幻觉写法亦可编译；键冲突
        // 由检查器 E2007 把关，此处 emplace 首绑定优先保持确定性）。
        if (!im.path.empty()) modAlias_.emplace(im.path.front(), full);
        if (im.path.size() > 1) modAlias_.emplace(im.path.back(), full);
    }

    // 声明遍 2：本模块函数（内建优先不遮蔽，与检查器解析顺序一致）。
    // 依赖方模块可能已为本模块函数创建占位条目——复用并补全签名。
    // @extern（H 专项）：标记签名壳（体不编译，VM 走动态库 thunk；
    // AOT 走 lang.extern_call），返回类型码由检查器 sig 反解。
    auto markExtern = [&](int idx, const FuncDef& fd) {
        bool ext = false;
        for (const auto& d : fd.decorators) ext |= (d.name == "extern");
        if (!ext) return;
        FuncBC& fn = program_.funcs[static_cast<size_t>(idx)];
        fn.isExtern = true;
        fn.isVariadic = fd.isVariadic; // O 节：变参装载分叉的依据
        for (const auto& d : fd.decorators) {
            if (d.name == "extern" && !d.args.empty() &&
                d.args[0]->kind == Expr::Kind::StrLit) {
                fn.externDll = static_cast<const StrLit&>(*d.args[0]).strValue;
                break;
            }
        }
        // 链接符号（@extern(..., symbol=...)）：缺省 = 函数名，与 VM 解析
        // 及 AOT genExternCall 同源同序（Sema 已校验恰一 StrLit）。
        for (const auto& d : fd.decorators) {
            if (d.name != "extern") continue;
            for (const auto& kv : d.kwargs) {
                if (kv.first == "symbol" &&
                    kv.second->kind == Expr::Kind::StrLit) {
                    fn.externSym =
                        static_cast<const StrLit&>(*kv.second).strValue;
                }
            }
        }
        auto rt = registry_.find(m.name);
        if (rt == registry_.end()) return;
        auto st = rt->second->funcs.find(fd.name);
        if (st == rt->second->funcs.end() || !st->second.ret) return;
        switch (st->second.ret->kind) {
        case Type::Kind::Int: fn.externRet = 'i'; break;
        case Type::Kind::Float: fn.externRet = 'f'; break;
        case Type::Kind::Bool: fn.externRet = 'b'; break;
        case Type::Kind::Str: fn.externRet = 's'; break;
        default: fn.externRet = 'v'; break;
        }
    };
    std::vector<const FuncDef*> defs;
    for (const auto& stmt : m.body) {
        if (stmt->kind != Stmt::Kind::FuncDef) continue;
        auto& f = static_cast<const FuncDef&>(*stmt);
        // 泛型模板（批 9-③）不进函数表也不编译：本体无具体类型可编译，
        // 调用点已被检查器改写为实例符号（实例是普通具体类型函数）。
        if (auto rt = registry_.find(m.name); rt != registry_.end()) {
            if (auto st = rt->second->funcs.find(f.name);
                st != rt->second->funcs.end() && st->second.hasUnannotated)
                continue;
        }
        defs.push_back(&f);
        std::vector<Value> defaults;
        uint32_t required = 0;
        bool sawDefault = false;
        for (const auto& p : f.params) {
            if (p.defaultValue) {
                sawDefault = true;
                defaults.push_back(constOf(*p.defaultValue));
            } else if (!sawDefault) {
                ++required;
            }
        }
        int idx = lookupModuleFunc(m.name, f.name);
        if (idx < 0) {
            idx = declareFunc(m.name, f.name,
                              static_cast<uint32_t>(f.params.size()), required,
                              std::move(defaults), f.isAsync);
        } else {
            FuncBC& fn = program_.funcs[static_cast<size_t>(idx)];
            fn.paramCount = static_cast<uint32_t>(f.params.size());
            fn.requiredArgs = required;
            fn.defaults = std::move(defaults);
            fn.isAsync = f.isAsync;
        }
        markExtern(idx, f);
        visibleFuncs_.emplace(f.name, idx);
    }

    // 编译遍 1：各函数体。
    for (const FuncDef* fd : defs) {
        auto it = visibleFuncs_.find(fd->name);
        if (it == visibleFuncs_.end() ||
            program_.funcs[static_cast<size_t>(it->second)].module != m.name)
            continue; // 与内建重名：调用按检查器语义走内建；体不进入调用图
        FuncBC& fn = program_.funcs[static_cast<size_t>(it->second)];
        if (fn.isExtern)
            continue; // @extern：体只有 pass，不发射（H 专项）
        LocalInfo locals;
        for (const auto& p : fd->params)
            locals.slots.emplace(p.name, locals.nlocals++);
        fn_ = &fn;
        locals_ = &locals;
        loopDepth_ = 0;
        openTries_.clear();
        deferStack_.clear();
        compileStmtList(fd->body);
        emitDefers(); // 函数尾隐式 return None 前展开全部 defer
        emit(Op::ReturnNone); // 隐式 return None
        fn_->nlocals = locals.nlocals;
        fn_ = nullptr;
        locals_ = nullptr;
    }

    // 编译遍 2：模块顶层伪函数（全局初始化）。
    {
        FuncBC top;
        top.name = "<top>";
        top.module = m.name;
        top.isTopLevel = true;
        program_.funcs.push_back(std::move(top));
        FuncBC& fn = program_.funcs.back();
        LocalInfo locals; // 顶层用全局槽，不复用局部
        fn_ = &fn;
        locals_ = &locals;
        deferStack_.clear(); // 顶层无 defer（检查器已限），防跨函数残留
        compileStmtList(m.body);
        emit(Op::ReturnNone);
        fn_->nlocals = locals.nlocals;
        fn_ = nullptr;
        locals_ = nullptr;
    }
        return true;
    } catch (const CompileAbort&) {
        return false;
    }
}

} // namespace bc
} // namespace scourge
