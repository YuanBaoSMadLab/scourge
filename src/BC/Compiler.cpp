// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 字节码编译器实现（SG-AST → SG-BC）
// ============================================================================
#include "scourge/BC/Compiler.hpp"

#include <stdexcept>

namespace scourge {
namespace bc {

namespace {

/// 编译中途放弃（诊断已登记）——用异常退栈，compileModule 捕获后报 false。
struct CompileAbort {};

const char* builtinNames[] = {
    "print",     "str",   "len", "sum",       "now",   "range", "ok", "err",
    "abs",       "sqrt",  "exp", "log",       "max",   "min",
    "transpose", "reshape", "data_ptr",
    // v0.9.3 标量数学/随机/排序（与 MLIRGen builtinNames 同表）
    "sin",  "cos",  "tan",   "asin", "acos", "atan", "sinh", "cosh",
    "tanh", "atan2", "floor", "ceil", "round", "trunc",
    "random", "seed", "sorted",
    // 0.9.4 IO 子系统（与 MLIRGen builtinNames 同表）
    "input", "read_text", "write_text", "append_text", "exists",
    "list_dir",
    // 0.9.4 F/G 专项：网络客户端与睡眠（与 MLIRGen builtinNames 同表）
    "net_connect", "net_send", "net_recv", "net_close", "net_udp_send",
    "net_http_get", "sleep",
    "net_bind", "net_accept", "net_set_timeout",
    // 0.9.4 H 专项：系统编程（args/env/时间/进程/信号；同表）
    "args", "env", "has_env", "set_env", "unset_env", "epoch", "epoch_ms",
    "localtime", "gmtime", "system", "spawn_wait", "signal_caught",
    "signal_reset",
    // 0.9.4 M 专项：标准库广度（统计/倒序/哈希；与 MLIRGen builtinNames 同表）
    "mean", "var", "std", "reversed", "sha256", "crc32",
    // 0.9.4 M 专项：JSON（动态 Json 值类型）
    "json_decode"};

bool isBuiltinName(const std::string& n) {
    for (const char* b : builtinNames)
        if (n == b) return true;
    return false;
}

/// 与检查器 checkListLit 同规则：非空且元素全为数值字面量（含一元负号、
/// 嵌套数值列表）的字面量在类型系统中即张量。编译器按同一判定将其编为
/// Tensor 常量——与检查器语义零分叉，禁止静默降级为 List。
bool isTensorLiteral(const ListLit& ll) {
    if (ll.elems.empty()) return false;
    for (const auto& el : ll.elems) {
        switch (el->kind) {
        case Expr::Kind::IntLit:
        case Expr::Kind::FloatLit:
            continue;
        case Expr::Kind::Unary: {
            auto& u = static_cast<const scourge::Unary&>(*el);
            if (u.op != "-") return false;
            Expr::Kind k = u.operand->kind;
            if (k != Expr::Kind::IntLit && k != Expr::Kind::FloatLit)
                return false;
            continue;
        }
        case Expr::Kind::ListLit:
            if (!isTensorLiteral(static_cast<const ListLit&>(*el)))
                return false;
            continue;
        default:
            return false;
        }
    }
    return true;
}

/// 张量字面量是否含浮点字面量（含嵌套）——决定 dtype（f64 / i64）。
bool tensorLitHasFloat(const ListLit& ll) {
    for (const auto& el : ll.elems) {
        switch (el->kind) {
        case Expr::Kind::FloatLit:
            return true;
        case Expr::Kind::IntLit:
            break;
        case Expr::Kind::Unary: {
            auto& u = static_cast<const scourge::Unary&>(*el);
            if (u.operand->kind == Expr::Kind::FloatLit) return true;
            break;
        }
        case Expr::Kind::ListLit:
            if (tensorLitHasFloat(static_cast<const ListLit&>(*el)))
                return true;
            break;
        default:
            break;
        }
    }
    return false;
}

/// 张量字面量展平累积器：形状逐维写入，元素按 dtype 收集。
struct TensorLitAcc {
    bool isInt = true;
    std::vector<int64_t> shape;
    std::vector<double> f64;
    std::vector<int64_t> i64;
};

/// 递归填充（行主序）；返回 false = 各行形状不一致。
/// 各行一致性已由检查器校验（check 先于编译），此处防御性兜底。
bool fillTensorLit(const ListLit& ll, size_t depth, TensorLitAcc& acc) {
    if (depth >= acc.shape.size()) {
        acc.shape.push_back(static_cast<int64_t>(ll.elems.size()));
    } else if (acc.shape[depth] != static_cast<int64_t>(ll.elems.size())) {
        return false;
    }
    for (const auto& el : ll.elems) {
        switch (el->kind) {
        case Expr::Kind::IntLit: {
            const int64_t v = static_cast<const IntLit&>(*el).value;
            if (acc.isInt) acc.i64.push_back(v);
            else acc.f64.push_back(static_cast<double>(v));
            break;
        }
        case Expr::Kind::FloatLit: { // 预扫描已保证 isInt == false
            acc.f64.push_back(static_cast<const FloatLit&>(*el).value);
            break;
        }
        case Expr::Kind::Unary: { // isTensorLiteral 已保证 "-" + 数值字面量
            auto& u = static_cast<const scourge::Unary&>(*el);
            if (u.operand->kind == Expr::Kind::IntLit) {
                const int64_t v =
                    -static_cast<const IntLit&>(*u.operand).value;
                if (acc.isInt) acc.i64.push_back(v);
                else acc.f64.push_back(static_cast<double>(v));
            } else {
                acc.f64.push_back(
                    -static_cast<const FloatLit&>(*u.operand).value);
            }
            break;
        }
        case Expr::Kind::ListLit:
            if (!fillTensorLit(static_cast<const ListLit&>(*el), depth + 1,
                               acc))
                return false;
            break;
        default:
            return false; // isTensorLiteral 已保证不可达
        }
    }
    return true;
}

} // namespace

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
        cur += suffix;
        if (!registry_.count(cur)) return {};
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
// 表达式
// ----------------------------------------------------------------------------

void Compiler::compileExpr(const Expr& e) {
    markLine(static_cast<uint32_t>(lineOf(e.range)));
    switch (e.kind) {
    case Expr::Kind::IntLit:
    case Expr::Kind::FloatLit:
    case Expr::Kind::BoolLit:
    case Expr::Kind::StrLit:
    case Expr::Kind::NoneLit:
        emit(Op::LoadConst, addConst(constOf(e)));
        return;

    case Expr::Kind::Name: {
        auto& n = static_cast<const Name&>(e);
        if (locals_) {
            auto it = locals_->slots.find(n.name);
            if (it != locals_->slots.end()) {
                emit(Op::LoadLocal, it->second);
                return;
            }
        }
        if (isBuiltinName(n.name) || lookupFunc(n.name) >= 0) {
            unsupported(e, "函数作为头等值将在 P1 提供；当前函数只能被调用");
        }
        // from-import 常量（0.9.6）：别名 → 源模块全局重定向。
        if (auto fg = fromGlobals_.find(n.name); fg != fromGlobals_.end()) {
            emit(Op::LoadGlobal, addGlobal(fg->second.first, fg->second.second));
            return;
        }
        // 模块级全局（含导入模块的常量经 Attr 走 LoadGlobal）
        emit(Op::LoadGlobal, addGlobal(moduleName_, n.name));
        return;
    }

    case Expr::Kind::Unary: {
        // bc::Unary 是操作码枚举，AST 节点需显式限定 scourge::Unary。
        auto& u = static_cast<const scourge::Unary&>(e);
        compileExpr(*u.operand);
        Unary op = u.op == "-"  ? Unary::Neg
                   : u.op == "+" ? Unary::Pos
                                 : Unary::Not;
        emit(Op::UnaryOp, static_cast<uint32_t>(op));
        return;
    }

    case Expr::Kind::Binary: {
        auto& b = static_cast<const Binary&>(e);
        // 短路求值：and/or 保留左值作为短路结果（与 AOT 语义一致）。
        if (b.op == "and") {
            compileExpr(*b.lhs);
            uint32_t j = emitJump(Op::JumpIfFalseOrPop);
            compileExpr(*b.rhs);
            patchJump(j);
            return;
        }
        if (b.op == "or") {
            compileExpr(*b.lhs);
            uint32_t j = emitJump(Op::JumpIfTrueOrPop);
            compileExpr(*b.rhs);
            patchJump(j);
            return;
        }
        compileExpr(*b.lhs);
        compileExpr(*b.rhs);
        // 矩阵乘：独立指令（运行时校验秩与内维，与检查器 E2004 同义）。
        if (b.op == "@") {
            emit(Op::MatMul);
            return;
        }
        Bin op = Bin::Add;
        if (b.op == "+") op = Bin::Add;
        else if (b.op == "-") op = Bin::Sub;
        else if (b.op == "*") op = Bin::Mul;
        else if (b.op == "/") op = Bin::Div;
        else if (b.op == "//") op = Bin::FloorDiv;
        else if (b.op == "%") op = Bin::Mod;
        else if (b.op == "**") op = Bin::Pow;
        else if (b.op == "==") op = Bin::Eq;
        else if (b.op == "!=") op = Bin::Ne;
        else if (b.op == "<") op = Bin::Lt;
        else if (b.op == "<=") op = Bin::Le;
        else if (b.op == ">") op = Bin::Gt;
        else if (b.op == ">=") op = Bin::Ge;
        else if (b.op == "in") op = Bin::In;
        else unsupported(e, "运算符 '" + b.op + "'");
        emit(Op::BinOp, static_cast<uint32_t>(op));
        return;
    }

    case Expr::Kind::Ternary: {
        auto& t = static_cast<const Ternary&>(e);
        compileExpr(*t.cond);
        uint32_t jFalse = emitJump(Op::JumpIfFalse);
        compileExpr(*t.thenE);
        uint32_t jEnd = emitJump(Op::Jump);
        patchJump(jFalse);
        compileExpr(*t.elseE);
        patchJump(jEnd);
        return;
    }

    case Expr::Kind::Call: {
        auto& c = static_cast<const Call&>(e);
        if (c.callee->kind == Expr::Kind::Attr) {
            auto& at = static_cast<const Attr&>(*c.callee);
            // 跨模块调用：m.func(args) / a.b.func(args)（0.9.6 点分链）。
            // 声明遍已为 import 模块建立函数索引（importedFuncs_），
            // 模块前缀经 foldModuleKey 折叠为键后按索引调用。
            std::string qm = foldModuleKey(*at.base);
            if (!qm.empty()) {
                auto mit = importedFuncs_.find(qm);
                if (mit != importedFuncs_.end()) {
                    auto fit = mit->second.find(at.name);
                    if (fit != mit->second.end()) {
                        for (const auto& a : c.args) {
                            compileExpr(*a);
                            if (needsF64(*a)) emit(Op::IntToFloat); // Q3
                        }
                        emit(Op::Call, static_cast<uint32_t>(fit->second),
                             static_cast<uint32_t>(c.args.size()));
                        return;
                    }
                }
            }
            // Result 消费方法：self 与实参依次压栈后 CallMethod。
            // 0.9.4 追加 str/List 成员方法（contains/index 同名——VM 运行时
            // 按 self.kind 分派；名称映射须与 Sema checkAttr 签名表一致）。
            Method m;
            if (at.name == "is_ok") m = Method::IsOk;
            else if (at.name == "unwrap") m = Method::Unwrap;
            else if (at.name == "unwrap_or") m = Method::UnwrapOr;
            else if (at.name == "upper") m = Method::StrUpper;
            else if (at.name == "lower") m = Method::StrLower;
            else if (at.name == "trim") m = Method::StrTrim;
            else if (at.name == "trim_start") m = Method::StrTrimStart;
            else if (at.name == "trim_end") m = Method::StrTrimEnd;
            else if (at.name == "split") m = Method::StrSplit;
            else if (at.name == "join") m = Method::StrJoin;
            else if (at.name == "replace") m = Method::StrReplace;
            else if (at.name == "find") m = Method::StrFind;
            else if (at.name == "contains") m = Method::Contains;
            else if (at.name == "index") m = Method::Index;
            else if (at.name == "startswith") m = Method::StrStartsWith;
            else if (at.name == "endswith") m = Method::StrEndsWith;
            else if (at.name == "count") m = Method::ListCount;
            // 0.9.4 M 专项：Json 成员方法（Sema checkAttr 已按 base kind
            // 分派；VM callValueMethod 按 self.kind==Json 路由到 VMJson.cpp）
            else if (at.name == "is_null") m = Method::JsonIsNull;
            else if (at.name == "is_bool") m = Method::JsonIsBool;
            else if (at.name == "is_int") m = Method::JsonIsInt;
            else if (at.name == "is_float") m = Method::JsonIsFloat;
            else if (at.name == "is_str") m = Method::JsonIsStr;
            else if (at.name == "is_list") m = Method::JsonIsList;
            else if (at.name == "is_obj") m = Method::JsonIsObj;
            else if (at.name == "as_int") m = Method::JsonAsInt;
            else if (at.name == "as_float") m = Method::JsonAsFloat;
            else if (at.name == "as_bool") m = Method::JsonAsBool;
            else if (at.name == "as_str") m = Method::JsonAsStr;
            else if (at.name == "at") m = Method::JsonAt;
            else if (at.name == "get") m = Method::JsonGet;
            else if (at.name == "len") m = Method::JsonLen;
            else if (at.name == "keys") m = Method::JsonKeys;
            else if (at.name == "dump") m = Method::JsonDump;
            else unsupported(e, "成员方法 '" + at.name + "'");
            compileExpr(*at.base); // self
            for (const auto& a : c.args) compileExpr(*a);
            emit(Op::CallMethod, static_cast<uint32_t>(m),
                 static_cast<uint32_t>(c.args.size()));
            return;
        }
        if (c.callee->kind != Expr::Kind::Name)
            unsupported(*c.callee, "该调用形式");
        auto& n = static_cast<const Name&>(*c.callee);
        int idx = lookupFunc(n.name);
        if (idx < 0) unsupported(e, "未解析的函数 '" + n.name + "'");
        for (const auto& a : c.args) {
            compileExpr(*a);
            if (needsF64(*a)) emit(Op::IntToFloat); // Q3 边界提升（实参）
        }
        emit(Op::Call, static_cast<uint32_t>(idx),
             static_cast<uint32_t>(c.args.size()));
        return;
    }

    case Expr::Kind::Attr: {
        auto& at = static_cast<const Attr&>(e);
        // 模块常量访问：m.CONST / a.b.CONST → LoadGlobal(规范模块名, 名)
        // （0.9.6 点分链；绑定键经 foldModuleKey 还原真实全限定名）。
        std::string qm = foldModuleKey(*at.base);
        if (!qm.empty()) {
            emit(Op::LoadGlobal, addGlobal(qm, at.name));
            return;
        }
        unsupported(e, "非调用位置的成员访问 '" + at.name + "'");
    }

    case Expr::Kind::Index: {
        auto& ix = static_cast<const Index&>(e);
        // 张量支持多维（部分索引返回子张量）；List/Str/Range 仅单下标
        //（由检查器保证），b 携带下标个数。
        compileExpr(*ix.base);
        for (const auto& idx : ix.indices) compileExpr(*idx);
        emit(Op::LoadIndex, 0, static_cast<uint32_t>(ix.indices.size()));
        return;
    }

    case Expr::Kind::ListLit: {
        auto& ll = static_cast<const ListLit&>(e);
        // 单一事实源 = 检查器 checkedAsTensor：张量字面量（全常量 → 常量池；
        // 含运行时元素 → NewTensor 逐元素构造）；其余一律 NewList——
        // 含 0.9.3 List 数值元素路径（List 注解上下文的数值字面量）。
        if (ll.checkedAsTensor) {
            if (isTensorLiteral(ll)) {
                emit(Op::LoadConst, addConst(constTensorOf(ll)));
                return;
            }
            for (const auto& el : ll.elems) compileExpr(*el);
            emit(Op::NewTensor, 0, static_cast<uint32_t>(ll.elems.size()));
            return;
        }
        for (const auto& el : ll.elems) {
            compileExpr(*el);
            if (needsF64(*el)) emit(Op::IntToFloat); // Q3 List[float] 元素提升
        }
        emit(Op::NewList, static_cast<uint32_t>(ll.elems.size()));
        return;
    }

    case Expr::Kind::DictLit: {
        auto& dl = static_cast<const DictLit&>(e);
        // 检查器判为空集合（Set[T] 注解上下文的空 {}）→ NewSet(0)
        //（单一事实源 checkedAsSet，与 checkedAsTensor 同架构）。
        if (dl.checkedAsSet) {
            emit(Op::NewSet, 0);
            return;
        }
        // 栈序 k1,v1,k2,v2,...；float 统一键/值的 int 元素做边界提升
        //（Q3，promoteF64_ 单一事实源，与 ListLit 同规则）。
        for (const auto& p : dl.pairs) {
            compileExpr(*p.first);
            if (needsF64(*p.first)) emit(Op::IntToFloat);
            compileExpr(*p.second);
            if (needsF64(*p.second)) emit(Op::IntToFloat);
        }
        emit(Op::NewDict, static_cast<uint32_t>(dl.pairs.size()));
        return;
    }

    case Expr::Kind::SetLit: {
        auto& sl = static_cast<const SetLit&>(e);
        // 栈序 e1,e2,...；float 统一元素的 int 元素做边界提升
        //（Q3，promoteF64_ 单一事实源，与 DictLit 同规则）。
        for (const auto& el : sl.elems) {
            compileExpr(*el);
            if (needsF64(*el)) emit(Op::IntToFloat);
        }
        emit(Op::NewSet, static_cast<uint32_t>(sl.elems.size()));
        return;
    }

    case Expr::Kind::TupleLit: {
        // 异构定长不可变元组：逐元素编译后 NewTuple 装箱（不做跨元素
        // float 统一提升，与检查器 checkTupleLit 白名单一致）。
        auto& tl = static_cast<const TupleLit&>(e);
        for (const auto& el : tl.elems) compileExpr(*el);
        emit(Op::NewTuple, static_cast<uint32_t>(tl.elems.size()));
        return;
    }
    case Expr::Kind::Await:
        // await 类型透传（Task 语义 P2 落地）；VM 内同步执行。
        compileExpr(*static_cast<const Await&>(e).operand);
        return;
    }
    unsupported(e, "该表达式");
}

// ----------------------------------------------------------------------------
// 语句
// ----------------------------------------------------------------------------

void Compiler::compileStmtList(const StmtList& list) {
    for (const auto& s : list) compileStmt(*s);
}

void Compiler::emitDefers() {
    // LIFO 内联展开词法上位于该 return 之前的 defer 块。panic 传播走
    // 运行时 catch 栈回退，不经过 return 点，故不触发 defer（契约见
    // AST Defer 注释；VM/AOT 双模同构）。
    for (size_t k = deferStack_.size(); k > 0; --k)
        compileStmtList(*deferStack_[k - 1]);
}

void Compiler::compileStmt(const Stmt& s) {
    switch (s.kind) {
    case Stmt::Kind::Assign: {
        auto& a = static_cast<const Assign&>(s);
        if (a.target->kind == Expr::Kind::Name) {
            auto& n = static_cast<const Name&>(*a.target);
            compileExpr(*a.value);
            if (needsF64(*a.value)) emit(Op::IntToFloat); // Q3 边界提升
            if (fn_->isTopLevel) {
                emit(Op::StoreGlobal, addGlobal(moduleName_, n.name));
            } else {
                auto it = locals_->slots.find(n.name);
                if (it == locals_->slots.end()) {
                    it = locals_->slots
                             .emplace(n.name, locals_->nlocals++)
                             .first;
                }
                emit(Op::StoreLocal, it->second);
            }
            return;
        }
        // 下标赋值：base, 下标…, value → StoreIndex（张量须全秩，检查器保证）
        auto& ix = static_cast<const Index&>(*a.target);
        compileExpr(*ix.base);
        for (const auto& idx : ix.indices) compileExpr(*idx);
        compileExpr(*a.value);
        emit(Op::StoreIndex, 0, static_cast<uint32_t>(ix.indices.size()));
        return;
    }

    case Stmt::Kind::AugAssign: {
        auto& a = static_cast<const AugAssign&>(s);
        auto& n = static_cast<const Name&>(*a.target);
        uint32_t slot;
        if (fn_->isTopLevel) {
            slot = addGlobal(moduleName_, n.name);
            emit(Op::LoadGlobal, slot);
        } else {
            auto it = locals_->slots.find(n.name);
            if (it == locals_->slots.end()) {
                diag_.error(Errc::E2003_UnknownName, file_, n.range,
                            "增强赋值目标未声明");
                throw CompileAbort{};
            }
            slot = it->second;
            emit(Op::LoadLocal, slot);
        }
        compileExpr(*a.value);
        Bin op = Bin::Add;
        if (a.op == "+=") op = Bin::Add;
        else if (a.op == "-=") op = Bin::Sub;
        else if (a.op == "*=") op = Bin::Mul;
        else if (a.op == "/=") op = Bin::Div;
        else if (a.op == "//=") op = Bin::FloorDiv;
        else if (a.op == "%=") op = Bin::Mod;
        else if (a.op == "**=") op = Bin::Pow;
        else error(s.range, "增强赋值运算符 '" + a.op + "'");
        emit(Op::BinOp, static_cast<uint32_t>(op));
        if (fn_->isTopLevel) emit(Op::StoreGlobal, slot);
        else emit(Op::StoreLocal, slot);
        return;
    }

    case Stmt::Kind::ExprStmt:
        compileExpr(*static_cast<const ExprStmt&>(s).expr);
        emit(Op::Pop);
        return;

    case Stmt::Kind::If: {
        auto& st = static_cast<const If&>(s);
        compileExpr(*st.cond);
        uint32_t jFalse = emitJump(Op::JumpIfFalse);
        compileStmtList(st.thenBody);
        std::vector<uint32_t> ends;
        for (const auto& e : st.elifs) {
            uint32_t jNext = emitJump(Op::Jump);
            ends.push_back(jNext);
            patchJump(jFalse);
            compileExpr(*e.cond);
            jFalse = emitJump(Op::JumpIfFalse);
            compileStmtList(e.body);
        }
        uint32_t jElse = emitJump(Op::Jump);
        ends.push_back(jElse);
        patchJump(jFalse);
        compileStmtList(st.elseBody);
        for (uint32_t j : ends) patchJump(j);
        return;
    }

    case Stmt::Kind::While: {
        auto& st = static_cast<const While&>(s);
        uint32_t top = here();
        compileExpr(*st.cond);
        uint32_t jEnd = emitJump(Op::JumpIfFalse);
        loopDepth_++;
        breakJumps_.emplace_back();
        continueJumps_.emplace_back();
        loopEntryTries_.push_back(openTries_.size());
        compileStmtList(st.body);
        loopEntryTries_.pop_back();
        loopDepth_--;
        emit(Op::Jump, top); // 回边：重新求值条件
        for (uint32_t j : continueJumps_.back()) patchTo(j, top);
        continueJumps_.pop_back();
        for (uint32_t j : breakJumps_.back()) patchJump(j);
        breakJumps_.pop_back();
        patchJump(jEnd);
        return;
    }

    case Stmt::Kind::For: {
        auto& st = static_cast<const For&>(s);
        // tmp 迭代源 + counter 两槽；循环变量单独一槽。
        // N2 修复：iter/counter 为循环内部状态，不进名字映射——嵌套同名
        // 循环各自持有独立槽，内层不再覆写外层迭代器（否则外层回边读到
        // 已耗尽的内层迭代器而静默提前终止）。var 槽仍按名复用：与 Python
        // 一致，外层每次推进由 ForNext 后的 StoreLocal 重新赋值。
        auto freshSlot = [&]() { return locals_->nlocals++; };
        compileExpr(*st.iter);
        uint32_t tmp = freshSlot();
        emit(Op::StoreLocal, tmp);
        uint32_t counter = freshSlot();
        emit(Op::LoadConst, addConst(Value::makeInt(0)));
        emit(Op::StoreLocal, counter);
        auto slotOf = [&](const std::string& name) {
            auto it = locals_->slots.find(name);
            if (it == locals_->slots.end())
                it = locals_->slots.emplace(name, locals_->nlocals++).first;
            return it->second;
        };
        uint32_t var = slotOf(st.var);

        uint32_t top = here();
        emit(Op::ForNext, tmp, counter);   // 推进：压入下一元素或 false
        uint32_t jEnd = emitJump(Op::JumpIfFalse);
        emit(Op::StoreLocal, var);

        loopDepth_++;
        breakJumps_.emplace_back();
        continueJumps_.emplace_back();
        loopEntryTries_.push_back(openTries_.size());
        compileStmtList(st.body);
        loopEntryTries_.pop_back();
        loopDepth_--;
        emit(Op::Jump, top); // 回边：推进迭代器取下一元素
        for (uint32_t j : continueJumps_.back()) patchTo(j, top);
        continueJumps_.pop_back();
        for (uint32_t j : breakJumps_.back()) patchJump(j);
        breakJumps_.pop_back();
        patchJump(jEnd);
        return;
    }

    case Stmt::Kind::Return: {
        auto& st = static_cast<const Return&>(s);
        if (st.value) {
            compileExpr(*st.value);
            if (needsF64(*st.value)) emit(Op::IntToFloat); // Q3 边界提升（返回）
        }
        // defer 先于返回指令：返回值已驻留栈顶，defer 块语句栈自平衡
        // （语句不变式），执行完弹返回值——与 AOT 展开语义逐点对齐。
        emitDefers();
        if (st.value) {
            emit(Op::Return);
        } else {
            emit(Op::ReturnNone);
        }
        return;
    }

    case Stmt::Kind::Break:
    case Stmt::Kind::Continue: {
        if (loopEntryTries_.empty()) return; // 检查器已报 E2009
        // 穿越退出循环内已进入的 try（运行时 catch 栈保持一致）。
        size_t nExit = openTries_.size() - loopEntryTries_.back();
        for (size_t k = 0; k < nExit; ++k) emit(Op::ExitTry);
        if (s.kind == Stmt::Kind::Break) {
            if (breakJumps_.empty()) return; // 检查器已报 E2009
            breakJumps_.back().push_back(emitJump(Op::Jump));
        } else {
            if (continueJumps_.empty()) return;
            continueJumps_.back().push_back(emitJump(Op::Jump));
        }
        return;
    }

    case Stmt::Kind::Pass:
        return;

    case Stmt::Kind::Defer:
        // 仅登记不发射：展开发生在 return 点（emitDefers，LIFO）。
        deferStack_.push_back(&static_cast<const Defer&>(s).body);
        return;

    case Stmt::Kind::Assert: {
        auto& st = static_cast<const Assert&>(s);
        markLine(static_cast<uint32_t>(lineOf(s.range)));
        compileExpr(*st.cond);
        if (st.msg) compileExpr(*st.msg);
        emit(Op::Assert, 0, st.msg ? 1u : 0u);
        return;
    }

    case Stmt::Kind::Try: {
        auto& st = static_cast<const Try&>(s);
        // EnterTry 压运行时 catch（handler pc + 当前栈深）；body 正常结束
        // 时 ExitTry 弹出；panic 时 VM 按 catch 栈回退到 handler。
        emit(Op::EnterTry, 0); // handler 待 patch
        uint32_t enterAt = here() - 1;
        openTries_.push_back(enterAt);

        compileStmtList(st.body);

        openTries_.pop_back();
        emit(Op::ExitTry);
        uint32_t jOver = emitJump(Op::Jump);
        fn_->code[enterAt].a = here(); // handler 入口
        compileStmtList(st.handlerBody);
        patchJump(jOver);
        return;
    }

    case Stmt::Kind::Import:
    case Stmt::Kind::FuncDef:
        return; // 声明在模块级处理
    }
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
