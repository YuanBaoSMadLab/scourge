// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 栈式虚拟机实现（SG-BC 解释器）
// ----------------------------------------------------------------------------
//  语义纪律（与 AOT 共享前端，双模不漂移）：
//    * '/' 真除法恒为 float；'//' 与 '%' 保持 int（Python 语义）；
//    * 整数加/减/乘/幂/取负溢出、除零、索引越界、断言失败、unwrap 失败态
//      一律 panic（可被 try/except 捕获），无 UB；
//    * panic 经运行时 catch 栈回退：帧栈截断到捕获帧、操作数栈回退到
//      EnterTry 时的深度、pc 置于 handler（编译器已 patch）。
// ============================================================================
#include "scourge/BC/VM.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace scourge {
namespace bc {

namespace {

// ----------------------------------------------------------------------------
// 值辅助
// ----------------------------------------------------------------------------

bool isNumericKind(Value::Kind k) {
    return k == Value::Kind::Int || k == Value::Kind::Float ||
           k == Value::Kind::Bool;
}

int64_t toInt(const Value& v) {
    switch (v.kind) {
    case Value::Kind::Int: return v.i;
    case Value::Kind::Bool: return v.b ? 1 : 0;
    default: return 0; // 调用方已保证数值种类
    }
}

double asDouble(const Value& v) {
    switch (v.kind) {
    case Value::Kind::Float: return v.f;
    case Value::Kind::Bool: return v.b ? 1.0 : 0.0;
    default: return static_cast<double>(v.i);
    }
}

bool truthy(const Value& v) {
    switch (v.kind) {
    case Value::Kind::Int: return v.i != 0;
    case Value::Kind::Float: return v.f != 0.0;
    case Value::Kind::Bool: return v.b;
    case Value::Kind::Str: return !v.s.empty();
    case Value::Kind::List: return v.list && !v.list->empty();
    case Value::Kind::Range: return v.i > 0;
    case Value::Kind::Result: return v.isOk;
    case Value::Kind::None: default: return false;
    }
}

bool valuesEqual(const Value& l, const Value& r);

bool valuesEqual(const Value& l, const Value& r) {
    if (isNumericKind(l.kind) && isNumericKind(r.kind)) {
        if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int)
            return l.i == r.i;
        return asDouble(l) == asDouble(r);
    }
    if (l.kind != r.kind) return false;
    switch (l.kind) {
    case Value::Kind::Str: return l.s == r.s;
    case Value::Kind::None: return true;
    case Value::Kind::Range: return l.i == r.i;
    case Value::Kind::List: {
        if (l.list.get() == r.list.get()) return true;
        if (!l.list || !r.list || l.list->size() != r.list->size()) return false;
        for (size_t k = 0; k < l.list->size(); ++k)
            if (!valuesEqual((*l.list)[k], (*r.list)[k])) return false;
        return true;
    }
    case Value::Kind::Result: {
        if (l.isOk != r.isOk) return false;
        if (!l.isOk) return l.s == r.s;
        Value lp = l, rp = r; // 还原载荷再比较（支持嵌套 Result）
        lp.kind = l.payloadKind;
        rp.kind = r.payloadKind;
        return valuesEqual(lp, rp);
    }
    default: return false;
    }
}

/// base ** exp（exp >= 0）；溢出返回 false。
bool ipowInt(int64_t base, int64_t exp, int64_t& out) {
    int64_t result = 1;
    while (exp > 0) {
        if (exp & 1) {
            if (__builtin_mul_overflow(result, base, &result)) return false;
        }
        exp >>= 1;
        if (exp > 0 && __builtin_mul_overflow(base, base, &base)) return false;
    }
    out = result;
    return true;
}

// ----------------------------------------------------------------------------
// 字符串化（确定性格式化：print / str() / 嵌套容器 repr 共用）
// ----------------------------------------------------------------------------

/// 形状的人类可读形式："(2, 3)"（panic 信息用）。
std::string shapeToString(const std::vector<int64_t>& shape) {
    std::string out = "(";
    for (size_t k = 0; k < shape.size(); ++k) {
        if (k) out += ", ";
        out += std::to_string(shape[k]);
    }
    out += ")";
    return out;
}

std::string escapeStr(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default: out += ch;
        }
    }
    out += "\"";
    return out;
}

/// 最短往返表示（15→16→17 位精度逐级尝试）；整数浮点补 ".0"（与 Python 一致）。
std::string floatToString(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    char buf[64];
    for (int prec = 15; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    std::string s = buf;
    if (s.find('.') == std::string::npos &&
        s.find('e') == std::string::npos && s.find('E') == std::string::npos)
        s += ".0";
    return s;
}

/// 张量行主序嵌套括号格式化：offset 为该维度块的起始元素下标。
void formatTensorRec(const TensorData& t, size_t dim, size_t offset,
                     std::string& out) {
    out += '[';
    if (dim + 1 == t.shape.size()) {
        const int64_t n = t.shape[dim];
        for (int64_t k = 0; k < n; ++k) {
            if (k) out += ", ";
            const size_t idx = offset + static_cast<size_t>(k);
            out += t.isInt ? std::to_string(t.i64[idx])
                           : floatToString(t.f64[idx]);
        }
    } else {
        int64_t stride = 1;
        for (size_t k = dim + 1; k < t.shape.size(); ++k)
            stride *= t.shape[k];
        const int64_t n = t.shape[dim];
        for (int64_t k = 0; k < n; ++k) {
            if (k) out += ", ";
            formatTensorRec(t, dim + 1,
                            offset + static_cast<size_t>(k) *
                                         static_cast<size_t>(stride),
                            out);
        }
    }
    out += ']';
}

std::string toString(const Value& v, bool repr) {
    switch (v.kind) {
    case Value::Kind::Int: return std::to_string(v.i);
    case Value::Kind::Float: return floatToString(v.f);
    case Value::Kind::Bool: return v.b ? "True" : "False";
    case Value::Kind::None: return "None";
    case Value::Kind::Str: return repr ? escapeStr(v.s) : v.s;
    case Value::Kind::List: {
        std::string out = "[";
        if (v.list) {
            for (size_t k = 0; k < v.list->size(); ++k) {
                if (k) out += ", ";
                out += toString((*v.list)[k], true);
            }
        }
        out += "]";
        return out;
    }
    case Value::Kind::Range:
        return "range(0, " + std::to_string(v.i) + ")";
    case Value::Kind::Result: {
        if (v.isOk) {
            Value p = v;
            p.kind = v.payloadKind;
            return "Ok(" + toString(p, true) + ")";
        }
        return "Err(" + escapeStr(v.s) + ")";
    }
    case Value::Kind::Tensor: {
        if (!v.tensor) return "Tensor(?) []";
        // Tensor(2, 3) [[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]
        const TensorData& t = *v.tensor;
        std::string out = "Tensor" + shapeToString(t.shape) + " ";
        formatTensorRec(t, 0, 0, out);
        return out;
    }
    }
    return "None";
}

std::string displayName(const FuncBC& f) {
    if (f.isTopLevel) return "<top " + f.module + ">";
    if (f.isBuiltin) return f.builtinName;
    return f.module.empty() ? f.name : f.module + "." + f.name;
}

} // namespace

// ----------------------------------------------------------------------------
// Program 查询
// ----------------------------------------------------------------------------

int Program::findFunc(const std::string& module, const std::string& name) const {
    for (size_t i = 0; i < funcs.size(); ++i) {
        const FuncBC& f = funcs[i];
        if (f.isBuiltin) continue;
        if (f.module == module && f.name == name) return static_cast<int>(i);
    }
    return -1;
}

std::string stringify(const Value& v) { return toString(v, false); }

// ----------------------------------------------------------------------------
// 生命周期与顶层执行
// ----------------------------------------------------------------------------

VM::VM(Program program, VMOptions opts)
    : program_(std::move(program)), opts_(std::move(opts)) {}

bool VM::run(const std::vector<std::string>& modules, const std::string& entry) {
    globals_.assign(program_.globalNames.size(), Value::makeNone());
    frames_.clear();
    catches_.clear();
    lastReturn_ = Value::makeNone();
    panicked_ = false;
    exitCode_ = 0;

    // 各模块 <top>：全局初始化（modules 已是依赖层序，被依赖者先初始化）。
    for (const auto& name : modules) {
        int t = program_.findFunc(name, "<top>");
        if (t < 0) continue;
        callFunc(t, 0);
        execute();
        if (panicked_) return false;
    }

    // 入口函数：int 返回值即退出码（REPL 求值包装经 entryFunc 指定，
    // 非 int 返回时 exitCode 取 0，不影响 REPL）。
    const std::string entryName =
        opts_.entryFunc.empty() ? "main" : opts_.entryFunc;
    int mi = program_.findFunc(entry, entryName);
    if (mi >= 0) {
        callFunc(mi, 0);
        execute();
        if (panicked_) return false;
        exitCode_ = lastReturn_.kind == Value::Kind::Int
                        ? static_cast<int>(lastReturn_.i)
                        : 0;
    }
    return true;
}

void VM::execute() {
    while (!frames_.empty()) {
        try {
            runLoop();
        } catch (RuntimePanic& rp) {
            if (!recover(rp.panic)) {
                panicked_ = true;
                return;
            }
        }
    }
}

void VM::runLoop() {
    while (!frames_.empty()) {
        Frame& f = frames_.back();
        if (f.pc >= f.fn->code.size()) {
            doReturn(Value::makeNone()); // 编译器保证 ReturnNone 收尾；防御
            continue;
        }
        Instr in = f.fn->code[f.pc];
        ++f.pc;
        dispatch(in);
    }
}

// ----------------------------------------------------------------------------
// 操作数栈
// ----------------------------------------------------------------------------

Value VM::pop() {
    Frame& f = frames_.back();
    if (f.stack.empty()) panicHere("VM 内部错误：操作数栈下溢");
    Value v = std::move(f.stack.back());
    f.stack.pop_back();
    return v;
}

void VM::push(Value v) { frames_.back().stack.push_back(std::move(v)); }

void VM::doReturn(Value v) {
    // 帧入池（保留 locals/stack 容量）——深递归/调用密集场景不再反复堆分配。
    // 池上限取 maxDepth：帧总数受调用深度护栏约束，池不会无限增长。
    if (freeFrames_.size() < opts_.maxDepth)
        freeFrames_.push_back(std::move(frames_.back()));
    frames_.pop_back();
    // return 可能发生在 try 体内 → 清理随之失效的 catch 记录。
    while (!catches_.empty() && catches_.back().frameIndex >= frames_.size())
        catches_.pop_back();
    if (!frames_.empty())
        frames_.back().stack.push_back(std::move(v));
    else
        lastReturn_ = std::move(v);
}

// ----------------------------------------------------------------------------
// 调用
// ----------------------------------------------------------------------------

VM::Frame VM::takeFreeFrame() {
    Frame fr;
    if (!freeFrames_.empty()) {
        fr = std::move(freeFrames_.back());
        freeFrames_.pop_back();
    }
    fr.pc = 0;
    fr.stack.clear(); // 析构旧值（引用计数正确回落），缓冲容量保留
    return fr;
}

void VM::callFunc(int fidx, uint32_t argc) {
    const FuncBC& fn = program_.funcs[static_cast<size_t>(fidx)];

    if (fn.isBuiltin) {
        if (frames_.empty()) panicHere("VM 内部错误：内建函数无调用帧");
        doBuiltin(fn, argc);
        return;
    }

    if (argc < fn.requiredArgs || argc > fn.paramCount)
        panicHere("函数 '" + displayName(fn) + "' 实参个数不符（需要 " +
                  std::to_string(fn.requiredArgs) + "~" +
                  std::to_string(fn.paramCount) + " 个，实际 " +
                  std::to_string(argc) + " 个）");
    if (!frames_.empty() && frames_.size() >= opts_.maxDepth)
        panicHere("调用深度超限（上限 " + std::to_string(opts_.maxDepth) +
                  " 层）");
    if (fn.code.empty())
        panicHere("函数 '" + displayName(fn) +
                  "' 无函数体（所属模块未编译进字节码）");

    if (frames_.empty()) {
        // 根调用（<top> / main 直启）：无调用者栈，实参只能来自缺省值。
        Frame fr = takeFreeFrame();
        fr.fn = &fn;
        fr.locals.resize(fn.nlocals);
        for (uint32_t k = argc; k < fn.paramCount; ++k) {
            uint32_t di = k - fn.requiredArgs;
            fr.locals[k] = di < fn.defaults.size()
                               ? fn.defaults[di]
                               : Value::makeNone();
        }
        frames_.push_back(std::move(fr));
        return;
    }

    Frame& caller = frames_.back();
    if (caller.stack.size() < argc) panicHere("VM 内部错误：实参栈下溢");

    Frame fr = takeFreeFrame();
    fr.fn = &fn;
    fr.locals.resize(fn.nlocals);
    size_t base = caller.stack.size() - argc;
    for (uint32_t k = 0; k < argc; ++k)
        fr.locals[k] = std::move(caller.stack[base + k]);
    caller.stack.resize(base);
    for (uint32_t k = argc; k < fn.paramCount; ++k) {
        uint32_t di = k - fn.requiredArgs;
        fr.locals[k] = di < fn.defaults.size() ? fn.defaults[di]
                                               : Value::makeNone();
    }
    frames_.push_back(std::move(fr)); // caller 引用自此失效
}

void VM::doBuiltin(const FuncBC& fn, uint32_t argc) {
    std::vector<Value> a(argc);
    for (uint32_t k = argc; k-- > 0;) a[k] = pop();

    const std::string& n = fn.builtinName;
    auto need = [&](uint32_t want) {
        if (a.size() != want)
            panicHere("内建 " + n + "() 需要 " + std::to_string(want) +
                      " 个参数");
    };

    if (n == "print") {
        std::string line;
        for (size_t k = 0; k < a.size(); ++k) {
            if (k) line += ' ';
            line += stringify(a[k]);
        }
        line += '\n';
        if (opts_.out)
            opts_.out(line);
        else
            std::fputs(line.c_str(), stdout);
        push(Value::makeNone());
        return;
    }
    if (n == "str") {
        need(1);
        push(Value::makeStr(stringify(a[0])));
        return;
    }
    if (n == "len") {
        need(1);
        if (a[0].kind == Value::Kind::List && a[0].list)
            push(Value::makeInt(static_cast<int64_t>(a[0].list->size())));
        else if (a[0].kind == Value::Kind::Str)
            push(Value::makeInt(static_cast<int64_t>(a[0].s.size())));
        else
            panicHere("len() 不支持该类型");
        return;
    }
    if (n == "sum") {
        need(1);
        if (a[0].kind == Value::Kind::Tensor && a[0].tensor) {
            const TensorData& t = *a[0].tensor;
            if (t.isInt) {
                int64_t acc = 0;
                for (int64_t x : t.i64) {
                    int64_t out = 0;
                    if (__builtin_add_overflow(acc, x, &out))
                        panicHere("整数求和溢出");
                    acc = out;
                }
                push(Value::makeInt(acc));
            } else {
                double acc = 0.0;
                for (double x : t.f64) acc += x;
                push(Value::makeFloat(acc));
            }
            return;
        }
        panicHere("sum() 仅支持张量");
        return;
    }
    if (n == "now") {
        // 单调时钟秒数（double）：基准测试与训练计时用，无参数。
        const auto t = std::chrono::steady_clock::now().time_since_epoch();
        push(Value::makeFloat(
            std::chrono::duration<double>(t).count()));
        return;
    }
    if (n == "range") {
        need(1);
        if (a[0].kind != Value::Kind::Int) panicHere("range() 参数必须是 int");
        // 负数原样存入：ForNext 首轮即结束（与 Python range(负) 为空一致）。
        push(Value::makeRange(a[0].i));
        return;
    }
    if (n == "ok") {
        need(1);
        push(Value::makeResult(a[0], true));
        return;
    }
    if (n == "err") {
        need(1);
        if (a[0].kind != Value::Kind::Str) panicHere("err() 参数必须是 str");
        push(Value::makeResult(Value::makeNone(), false, a[0].s));
        return;
    }
    // abs/sqrt/exp/log：逐元素数学函数（标量或数值张量）。abs 保 dtype，
    // sqrt/exp/log 结果恒为 float（负数的 sqrt/log 依 IEEE 754 得 nan/-inf）。
    if (n == "abs" || n == "sqrt" || n == "exp" || n == "log") {
        need(1);
        const bool keepInt = (n == "abs");
        auto apply1 = [&](double x) -> double {
            if (n == "sqrt") return std::sqrt(x);
            if (n == "exp") return std::exp(x);
            return std::log(x);
        };
        const Value& v = a[0];
        if (v.kind == Value::Kind::Int) {
            if (keepInt)
                push(Value::makeInt(v.i < 0 ? -v.i : v.i));
            else
                push(Value::makeFloat(apply1(static_cast<double>(v.i))));
            return;
        }
        if (v.kind == Value::Kind::Float) {
            push(Value::makeFloat(keepInt ? std::fabs(v.f) : apply1(v.f)));
            return;
        }
        if (v.kind == Value::Kind::Tensor && v.tensor) {
            const TensorData& t = *v.tensor;
            const bool outInt = keepInt && t.isInt;
            auto td = std::make_shared<TensorData>();
            td->isInt = outInt;
            td->shape = t.shape;
            if (outInt) {
                td->i64.resize(t.i64.size());
                for (size_t k = 0; k < t.i64.size(); ++k)
                    td->i64[k] = t.i64[k] < 0 ? -t.i64[k] : t.i64[k];
            } else if (t.isInt) {
                td->f64.resize(t.i64.size());
                for (size_t k = 0; k < t.i64.size(); ++k)
                    td->f64[k] = apply1(static_cast<double>(t.i64[k]));
            } else {
                td->f64.resize(t.f64.size());
                for (size_t k = 0; k < t.f64.size(); ++k)
                    td->f64[k] = keepInt ? std::fabs(t.f64[k]) : apply1(t.f64[k]);
            }
            push(Value::makeTensor(std::move(td)));
            return;
        }
        panicHere(n + "() 仅支持数值或数值张量");
        return;
    }
    // max/min：张量归约到元素类型标量（空张量报错）。
    if (n == "max" || n == "min") {
        need(1);
        if (a[0].kind != Value::Kind::Tensor || !a[0].tensor)
            panicHere(n + "() 仅支持张量归约");
        const TensorData& t = *a[0].tensor;
        if (t.size() == 0) panicHere(n + "() 不支持空张量");
        const bool wantMax = (n == "max");
        if (t.isInt) {
            int64_t best = t.i64[0];
            for (int64_t x : t.i64)
                best = wantMax ? (x > best ? x : best) : (x < best ? x : best);
            push(Value::makeInt(best));
        } else {
            double best = t.f64[0];
            for (double x : t.f64)
                best = wantMax ? (x > best ? x : best) : (x < best ? x : best);
            push(Value::makeFloat(best));
        }
        return;
    }
    // transpose：二维转置 (m, n) → (n, m)。
    if (n == "transpose") {
        need(1);
        if (a[0].kind != Value::Kind::Tensor || !a[0].tensor)
            panicHere("transpose() 要求二维张量");
        const TensorData& t = *a[0].tensor;
        if (t.shape.size() != 2)
            panicHere("transpose() 要求二维张量，实际 " + shapeToString(t.shape));
        const int64_t m = t.shape[0];
        const int64_t c = t.shape[1];
        auto td = std::make_shared<TensorData>();
        td->isInt = t.isInt;
        td->shape = {c, m};
        const size_t count = static_cast<size_t>(m * c);
        if (td->isInt) {
            td->i64.resize(count);
            for (int64_t i = 0; i < m; ++i)
                for (int64_t j = 0; j < c; ++j)
                    td->i64[static_cast<size_t>(j * m + i)] =
                        t.i64[static_cast<size_t>(i * c + j)];
        } else {
            td->f64.resize(count);
            for (int64_t i = 0; i < m; ++i)
                for (int64_t j = 0; j < c; ++j)
                    td->f64[static_cast<size_t>(j * m + i)] =
                        t.f64[static_cast<size_t>(i * c + j)];
        }
        push(Value::makeTensor(std::move(td)));
        return;
    }
    // reshape：数据顺序不变，仅重组形状；元素总数运行时校验。
    if (n == "reshape") {
        if (a.size() < 2) panicHere("reshape() 需要张量 + 至少 1 个维度");
        if (a[0].kind != Value::Kind::Tensor || !a[0].tensor)
            panicHere("reshape() 首参必须是张量");
        std::vector<int64_t> ns;
        int64_t prod = 1;
        for (size_t k = 1; k < a.size(); ++k) {
            if (a[k].kind != Value::Kind::Int)
                panicHere("reshape() 维度必须是 int");
            ns.push_back(a[k].i);
            prod *= a[k].i;
        }
        const TensorData& t = *a[0].tensor;
        if (prod != static_cast<int64_t>(t.size()))
            panicHere("reshape() 元素总数不一致：" + shapeToString(t.shape) +
                      "（共 " + std::to_string(t.size()) + " 个）与目标形状（共 " +
                      std::to_string(prod) + " 个）");
        auto td = std::make_shared<TensorData>(t); // 值语义拷贝
        td->shape = std::move(ns);
        push(Value::makeTensor(std::move(td)));
        return;
    }
    // ---- v0.9.3 标量数学/随机/排序（与 AOT kRuntimeC 语义逐字一致） ----
    // 三角/双曲：int 提升 f64，恒返回 float；依 IEEE 754（asin(2) → nan）。
    if (n == "sin" || n == "cos" || n == "tan" || n == "asin" ||
        n == "acos" || n == "atan" || n == "sinh" || n == "cosh" ||
        n == "tanh") {
        need(1);
        double x;
        if (a[0].kind == Value::Kind::Int)
            x = static_cast<double>(a[0].i);
        else if (a[0].kind == Value::Kind::Float)
            x = a[0].f;
        else
            panicHere(n + "() 仅支持标量数值");
        double r;
        if (n == "sin")
            r = std::sin(x);
        else if (n == "cos")
            r = std::cos(x);
        else if (n == "tan")
            r = std::tan(x);
        else if (n == "asin")
            r = std::asin(x);
        else if (n == "acos")
            r = std::acos(x);
        else if (n == "atan")
            r = std::atan(x);
        else if (n == "sinh")
            r = std::sinh(x);
        else if (n == "cosh")
            r = std::cosh(x);
        else
            r = std::tanh(x);
        push(Value::makeFloat(r));
        return;
    }
    if (n == "atan2") {
        need(2);
        double xy[2];
        for (int k = 0; k < 2; ++k) {
            if (a[k].kind == Value::Kind::Int)
                xy[k] = static_cast<double>(a[k].i);
            else if (a[k].kind == Value::Kind::Float)
                xy[k] = a[k].f;
            else
                panicHere("atan2() 仅支持标量数值");
        }
        push(Value::makeFloat(std::atan2(xy[0], xy[1])));
        return;
    }
    // 取整族：int 恒等；float → int（NaN/±inf/结果超 int64 范围 panic，
    // 不回绕）。round = 半到偶（nearbyint，默认舍入模式，与 Python 3 一致）。
    if (n == "floor" || n == "ceil" || n == "round" || n == "trunc") {
        need(1);
        if (a[0].kind == Value::Kind::Int) {
            push(Value::makeInt(a[0].i));
            return;
        }
        if (a[0].kind != Value::Kind::Float)
            panicHere(n + "() 仅支持标量数值");
        double x = a[0].f;
        double r = n == "floor" ? std::floor(x)
                 : n == "ceil"  ? std::ceil(x)
                 : n == "round" ? std::nearbyint(x)
                                : std::trunc(x);
        if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
            panicHere(n + "() 结果超出 int64 范围");
        push(Value::makeInt(static_cast<int64_t>(r)));
        return;
    }
    // random/seed：splitmix64 确定性序列（与 kRuntimeC sg_rng_* 逐位一致，
    // 双模差分对拍依据）。random ∈ [0,1)，53 位精度。
    if (n == "random") {
        if (!a.empty()) panicHere("random() 不需要参数");
        push(Value::makeFloat(static_cast<double>(rngNext() >> 11) *
                              (1.0 / 9007199254740992.0)));
        return;
    }
    if (n == "seed") {
        need(1);
        if (a[0].kind != Value::Kind::Int) panicHere("seed() 参数必须是 int");
        rngState_ = static_cast<uint64_t>(a[0].i); // 负数按 64 位补码解释
        push(Value::makeNone());
        return;
    }
    // sorted(List[T]) → 新列表：稳定升序。int/bool 同类精确比较，float 同类
    // 位型比较，int/float 混合以 double 比较（检查器已统一元素类型，混合仅限
    // 未注解路径）。str 按字节序。含 NaN 的列表显式拒绝（见下）。
    if (n == "sorted") {
        need(1);
        if (a[0].kind != Value::Kind::List || !a[0].list)
            panicHere("sorted() 仅支持列表");
        // NaN 显式拒绝（与 AOT sg_sorted 同语义同消息）：NaN 不可比破坏全序。
        for (const Value& v : *a[0].list)
            if (v.kind == Value::Kind::Float && std::isnan(v.f))
                panicHere("sorted() 不支持含 NaN 的浮点元素");
        auto out = std::make_shared<std::vector<Value>>(*a[0].list);
        std::stable_sort(out->begin(), out->end(),
                         [this](const Value& l, const Value& r) {
                             if (l.kind == Value::Kind::Int &&
                                 r.kind == Value::Kind::Int)
                                 return l.i < r.i;
                             if (l.kind == Value::Kind::Float &&
                                 r.kind == Value::Kind::Float)
                                 return l.f < r.f;
                             if (l.kind == Value::Kind::Str &&
                                 r.kind == Value::Kind::Str)
                                 return l.s < r.s;
                             if (l.kind == Value::Kind::Bool &&
                                 r.kind == Value::Kind::Bool)
                                 return l.b < r.b;
                             if ((l.kind == Value::Kind::Int ||
                                  l.kind == Value::Kind::Float) &&
                                 (r.kind == Value::Kind::Int ||
                                  r.kind == Value::Kind::Float)) {
                                 double lx = l.kind == Value::Kind::Float
                                                 ? l.f
                                                 : static_cast<double>(l.i);
                                 double rx = r.kind == Value::Kind::Float
                                                 ? r.f
                                                 : static_cast<double>(r.i);
                                 return lx < rx;
                             }
                             panicHere("sorted() 列表元素类型不可比较");
                         });
        push(Value::makeList(std::move(out)));
        return;
    }
    panicHere("VM 内部错误：未知内建 '" + n + "'");
}

uint64_t VM::rngNext() {
    // splitmix64：与 kRuntimeC 的 sg_rng_next 实现逐位一致（单一算法语义，
    // 双模各自实现、差分对拍锁定）。
    rngState_ += 0x9E3779B97F4A7C15ull;
    uint64_t z = rngState_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// ----------------------------------------------------------------------------
// 指令分派
// ----------------------------------------------------------------------------

void VM::dispatch(Instr in) {
    Frame& f = frames_.back();

    switch (in.op) {
    case Op::LoadConst:
        push(program_.constants[in.a]);
        return;

    case Op::LoadLocal:
        if (in.a >= f.locals.size()) panicHere("VM 内部错误：局部槽越界");
        push(f.locals[in.a]);
        return;

    case Op::StoreLocal:
        if (in.a >= f.locals.size()) panicHere("VM 内部错误：局部槽越界");
        f.locals[in.a] = pop();
        return;

    case Op::LoadGlobal:
        if (in.a >= globals_.size()) panicHere("VM 内部错误：全局槽越界");
        push(globals_[in.a]);
        return;

    case Op::StoreGlobal:
        if (in.a >= globals_.size()) panicHere("VM 内部错误：全局槽越界");
        globals_[in.a] = pop();
        return;

    case Op::Pop:
        pop();
        return;

    case Op::BinOp: {
        Value r = pop();
        Value l = pop();
        // Int×Int 快路径：标量循环/递归的热点，绕过 doBinOp 的通用类型分派链。
        // 覆盖 add/sub/mul（溢出 panic 文本与 doBinOp 逐字一致）与六种比较；
        // 除法族/Pow/And/Or/In 及非 Int 操作数一律走通用路径，语义零分叉。
        if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
            const int64_t x = l.i, y = r.i;
            switch (static_cast<Bin>(in.a)) {
            case Bin::Add: {
                int64_t o;
                if (__builtin_add_overflow(x, y, &o)) panicHere("整数加法溢出");
                push(Value::makeInt(o));
                return;
            }
            case Bin::Sub: {
                int64_t o;
                if (__builtin_sub_overflow(x, y, &o)) panicHere("整数减法溢出");
                push(Value::makeInt(o));
                return;
            }
            case Bin::Mul: {
                int64_t o;
                if (__builtin_mul_overflow(x, y, &o)) panicHere("整数乘法溢出");
                push(Value::makeInt(o));
                return;
            }
            case Bin::Lt: push(Value::makeBool(x < y)); return;
            case Bin::Le: push(Value::makeBool(x <= y)); return;
            case Bin::Gt: push(Value::makeBool(x > y)); return;
            case Bin::Ge: push(Value::makeBool(x >= y)); return;
            case Bin::Eq: push(Value::makeBool(x == y)); return;
            case Bin::Ne: push(Value::makeBool(x != y)); return;
            default: break; // 其余走 doBinOp
            }
        }
        push(doBinOp(static_cast<Bin>(in.a), l, r));
        return;
    }

    case Op::UnaryOp: {
        Value v = pop();
        push(doUnaryOp(static_cast<Unary>(in.a), v));
        return;
    }

    case Op::Jump:
        f.pc = in.a;
        return;

    case Op::JumpIfFalse: {
        Value v = pop();
        if (!truthy(v)) f.pc = in.a;
        return;
    }

    case Op::JumpIfFalseOrPop:
    case Op::JumpIfTrueOrPop: {
        if (f.stack.empty()) panicHere("VM 内部错误：操作数栈下溢");
        bool t = truthy(f.stack.back());
        bool taken = in.op == Op::JumpIfFalseOrPop ? !t : t;
        if (taken)
            f.pc = in.a; // 短路：保留操作数作为结果值
        else
            f.stack.pop_back();
        return;
    }

    case Op::Call:
        callFunc(static_cast<int>(in.a), in.b);
        return; // frames_ 已变，勿再触碰 f

    case Op::CallMethod: {
        uint32_t n = in.b;
        if (f.stack.size() < static_cast<size_t>(n) + 1)
            panicHere("VM 内部错误：方法调用栈下溢");
        std::vector<Value> args(n);
        for (uint32_t k = n; k-- > 0;) args[k] = pop();
        Value self = pop();
        switch (static_cast<Method>(in.a)) {
        case Method::IsOk:
            if (self.kind != Value::Kind::Result)
                panicHere("is_ok() 只能用于 Result");
            push(Value::makeBool(self.isOk));
            return;
        case Method::Unwrap:
        case Method::UnwrapOr:
            // 检查器 P0 语义：unwrap(缺省)/unwrap_or(缺省) 同形——必须携带
            // 1 个缺省值参数，ok 取载荷、失败态取缺省（无裸 unwrap 路径）。
            if (self.kind != Value::Kind::Result)
                panicHere("unwrap 只能用于 Result");
            if (args.size() != 1)
                panicHere("unwrap()/unwrap_or() 需要 1 个缺省值参数");
            if (self.isOk) {
                Value p = self;
                p.kind = self.payloadKind;
                push(std::move(p));
            } else {
                push(std::move(args[0]));
            }
            return;
        }
        panicHere("VM 内部错误：未知方法");
    }

    case Op::Return:
        doReturn(pop());
        return;

    case Op::ReturnNone:
        doReturn(Value::makeNone());
        return;

    case Op::Print: {
        Value v = pop();
        std::string line = stringify(v);
        line += '\n';
        if (opts_.out)
            opts_.out(line);
        else
            std::fputs(line.c_str(), stdout);
        return;
    }

    case Op::NewList: {
        if (f.stack.size() < in.a) panicHere("VM 内部错误：NewList 栈下溢");
        auto vec = std::make_shared<std::vector<Value>>();
        vec->resize(in.a);
        for (size_t k = in.a; k-- > 0;) (*vec)[k] = pop();
        push(Value::makeList(std::move(vec)));
        return;
    }

    case Op::LoadIndex: {
        const uint32_t n = in.b;
        if (n == 0) panicHere("VM 内部错误：下标数量为 0");
        if (f.stack.size() < static_cast<size_t>(n) + 1)
            panicHere("VM 内部错误：下标栈下溢");
        std::vector<Value> idxs(n);
        for (uint32_t k = n; k-- > 0;) idxs[k] = pop();
        Value base = pop();
        switch (base.kind) {
        case Value::Kind::List: {
            if (n != 1) panicHere("VM 内部错误：List 下标数量不符");
            if (!base.list) panicHere("VM 内部错误：空列表对象");
            int64_t len = static_cast<int64_t>(base.list->size());
            int64_t i = normIndex(toInt(idxs[0]), len, "列表");
            push((*base.list)[static_cast<size_t>(i)]);
            return;
        }
        case Value::Kind::Str: {
            if (n != 1) panicHere("VM 内部错误：Str 下标数量不符");
            // P1 按字节索引（UTF-8 多字节字符返回单字节切片）。
            int64_t len = static_cast<int64_t>(base.s.size());
            int64_t i = normIndex(toInt(idxs[0]), len, "字符串");
            push(Value::makeStr(
                std::string(1, base.s[static_cast<size_t>(i)])));
            return;
        }
        case Value::Kind::Range: {
            if (n != 1) panicHere("VM 内部错误：Range 下标数量不符");
            int64_t i = normIndex(toInt(idxs[0]), base.i, "range");
            push(Value::makeInt(i));
            return;
        }
        case Value::Kind::Tensor: {
            if (!base.tensor) panicHere("VM 内部错误：空张量对象");
            push(tensorIndexRead(base, idxs));
            return;
        }
        default:
            panicHere("该类型不支持下标读取");
        }
    }

    case Op::StoreIndex: {
        const uint32_t n = in.b;
        if (n == 0) panicHere("VM 内部错误：下标数量为 0");
        Value val = pop();
        if (f.stack.size() < static_cast<size_t>(n) + 1)
            panicHere("VM 内部错误：下标栈下溢");
        std::vector<Value> idxs(n);
        for (uint32_t k = n; k-- > 0;) idxs[k] = pop();
        Value base = pop();
        if (base.kind == Value::Kind::Tensor) {
            if (!base.tensor) panicHere("VM 内部错误：空张量对象");
            tensorIndexStore(base, idxs, val);
            return;
        }
        if (n != 1) panicHere("VM 内部错误：下标数量不符");
        if (base.kind != Value::Kind::List || !base.list)
            panicHere("下标赋值仅支持 list 与 tensor");
        int64_t len = static_cast<int64_t>(base.list->size());
        int64_t i = normIndex(toInt(idxs[0]), len, "列表");
        (*base.list)[static_cast<size_t>(i)] = std::move(val);
        return;
    }

    case Op::Assert: {
        std::string msg;
        if (in.b) {
            Value m = pop();
            if (m.kind == Value::Kind::Str) msg = m.s;
        }
        Value c = pop();
        if (!truthy(c))
            panicHere(msg.empty() ? std::string("断言失败") : "断言失败：" + msg);
        return;
    }

    case Op::EnterTry:
        catches_.push_back(
            CatchPoint{frames_.size() - 1, in.a, f.stack.size()});
        return;

    case Op::ExitTry:
        if (catches_.empty() ||
            catches_.back().frameIndex != frames_.size() - 1)
            panicHere("VM 内部错误：ExitTry 无匹配 EnterTry");
        catches_.pop_back();
        return;

    case Op::MatMul: {
        Value r = pop();
        Value l = pop();
        push(tensorMatMul(l, r));
        return;
    }
    case Op::NewTensor:
        push(tensorNew(in.b));
        return;

    case Op::ForNext: {
        if (in.a >= f.locals.size() || in.b >= f.locals.size())
            panicHere("VM 内部错误：ForNext 槽越界");
        const Value& src = f.locals[in.a];
        int64_t c = f.locals[in.b].i;
        // 约定：有下一元素 → 压入元素 + true；耗尽 → 仅压入 false。
        // 元素与结束标志分离，元素本身为假值（0/空串）不会误判终止。
        if (src.kind == Value::Kind::Range) {
            if (c < src.i) {
                f.locals[in.b] = Value::makeInt(c + 1);
                push(Value::makeInt(c));
                push(Value::makeBool(true));
            } else {
                push(Value::makeBool(false));
            }
            return;
        }
        if (src.kind == Value::Kind::List && src.list) {
            int64_t len = static_cast<int64_t>(src.list->size());
            if (c < len) {
                f.locals[in.b] = Value::makeInt(c + 1);
                push((*src.list)[static_cast<size_t>(c)]);
                push(Value::makeBool(true));
            } else {
                push(Value::makeBool(false));
            }
            return;
        }
        panicHere("for 迭代源仅支持 range 与 list");
    }

    case Op::IntToFloat: {
        // Q3 边界提升：int → float（存储/传参/返回边界的双模一致转换）。
        // 幂等：缺省值边界已在编译期（constOf）转为 f64，此处透传。
        Value v = pop();
        if (v.kind == Value::Kind::Int)
            push(Value::makeFloat(static_cast<double>(v.i)));
        else if (v.kind == Value::Kind::Float)
            push(v);
        else
            panicHere("VM 内部错误：IntToFloat 操作数不是数值");
        return;
    }
    }
    panicHere("VM 内部错误：未知指令");
}

// ----------------------------------------------------------------------------
// 运算（panic 语义集中区）
// ----------------------------------------------------------------------------

Value VM::doBinOp(Bin op, const Value& l, const Value& r) {
    // 张量逐元素 / 标量广播 —— 形状与元素类型已由检查器静态校验；
    // 比较/and/or/in 不支持张量（检查器拒绝），落到统一 panic。
    if (l.kind == Value::Kind::Tensor || r.kind == Value::Kind::Tensor) {
        switch (op) {
        case Bin::Add:
        case Bin::Sub:
        case Bin::Mul:
        case Bin::Div:
        case Bin::FloorDiv:
        case Bin::Mod:
        case Bin::Pow:
            return tensorBinOp(op, l, r);
        default:
            break;
        }
    }

    const bool ln = isNumericKind(l.kind);
    const bool rn = isNumericKind(r.kind);

    switch (op) {
    case Bin::Add:
    case Bin::Sub:
    case Bin::Mul: {
        // 字符串扩展（与检查器一致：str+str、str*int）。
        if (op == Bin::Add && l.kind == Value::Kind::Str &&
            r.kind == Value::Kind::Str)
            return Value::makeStr(l.s + r.s);
        if (op == Bin::Mul) {
            if (l.kind == Value::Kind::Str && rn) return repeatStr(l.s, toInt(r));
            if (r.kind == Value::Kind::Str && ln) return repeatStr(r.s, toInt(l));
        }
        if (!ln || !rn) break;
        if (l.kind == Value::Kind::Float || r.kind == Value::Kind::Float) {
            double x = asDouble(l), y = asDouble(r);
            if (op == Bin::Add) return Value::makeFloat(x + y);
            if (op == Bin::Sub) return Value::makeFloat(x - y);
            return Value::makeFloat(x * y);
        }
        int64_t x = toInt(l), y = toInt(r), out = 0;
        bool of = op == Bin::Add   ? __builtin_add_overflow(x, y, &out)
                  : op == Bin::Sub ? __builtin_sub_overflow(x, y, &out)
                                   : __builtin_mul_overflow(x, y, &out);
        if (of)
            panicHere(op == Bin::Add   ? "整数加法溢出"
                      : op == Bin::Sub ? "整数减法溢出"
                                       : "整数乘法溢出");
        return Value::makeInt(out);
    }

    case Bin::Div: {
        if (!ln || !rn) break;
        if (l.kind == Value::Kind::Float || r.kind == Value::Kind::Float) {
            double d = asDouble(r);
            if (d == 0.0) panicHere("浮点除零");
            return Value::makeFloat(asDouble(l) / d);
        }
        if (toInt(r) == 0) panicHere("整数除零");
        // 真除法恒为 float（与检查器一致）。
        return Value::makeFloat(static_cast<double>(toInt(l)) /
                                static_cast<double>(toInt(r)));
    }

    case Bin::FloorDiv: {
        if (!ln || !rn) break;
        if (l.kind == Value::Kind::Float || r.kind == Value::Kind::Float) {
            double d = asDouble(r);
            if (d == 0.0) panicHere("浮点整除零");
            return Value::makeFloat(std::floor(asDouble(l) / d));
        }
        int64_t a = toInt(l), b = toInt(r);
        if (b == 0) panicHere("整数整除零");
        if (a == INT64_MIN && b == -1) panicHere("整数整除溢出");
        int64_t q = a / b;
        if (a % b != 0 && ((a < 0) != (b < 0))) --q; // Python 向下取整
        return Value::makeInt(q);
    }

    case Bin::Mod: {
        if (!ln || !rn) break;
        if (l.kind == Value::Kind::Float || r.kind == Value::Kind::Float) {
            double d = asDouble(r);
            if (d == 0.0) panicHere("浮点取模零");
            double m = std::fmod(asDouble(l), d);
            if (m != 0.0 && ((m < 0.0) != (d < 0.0))) m += d; // 与除数同号
            return Value::makeFloat(m);
        }
        int64_t a = toInt(l), b = toInt(r);
        if (b == 0) panicHere("整数取模零");
        if (a == INT64_MIN && b == -1) return Value::makeInt(0);
        int64_t m = a % b;
        if (m != 0 && ((m < 0) != (b < 0))) m += b; // Python 语义
        return Value::makeInt(m);
    }

    case Bin::Pow: {
        if (!ln || !rn) break;
        if (l.kind != Value::Kind::Float && r.kind != Value::Kind::Float &&
            toInt(r) >= 0) {
            int64_t out = 0;
            if (!ipowInt(toInt(l), toInt(r), out)) panicHere("整数幂运算溢出");
            return Value::makeInt(out);
        }
        double base = asDouble(l), e = asDouble(r);
        if (base == 0.0 && e < 0.0) panicHere("零的负次幂");
        return Value::makeFloat(std::pow(base, e));
    }

    case Bin::Eq:
        return Value::makeBool(valuesEqual(l, r));

    case Bin::Ne:
        return Value::makeBool(!valuesEqual(l, r));

    case Bin::Lt:
    case Bin::Le:
    case Bin::Gt:
    case Bin::Ge: {
        if (ln && rn) {
            if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
                bool t = op == Bin::Lt ? l.i < r.i
                        : op == Bin::Le ? l.i <= r.i
                        : op == Bin::Gt ? l.i > r.i
                                        : l.i >= r.i;
                return Value::makeBool(t);
            }
            double x = asDouble(l), y = asDouble(r);
            bool t = op == Bin::Lt ? x < y
                     : op == Bin::Le ? x <= y
                     : op == Bin::Gt ? x > y
                                     : x >= y;
            return Value::makeBool(t);
        }
        if (l.kind == Value::Kind::Str && r.kind == Value::Kind::Str) {
            int c = l.s.compare(r.s);
            bool t = op == Bin::Lt ? c < 0
                     : op == Bin::Le ? c <= 0
                     : op == Bin::Gt ? c > 0
                                     : c >= 0;
            return Value::makeBool(t);
        }
        panicHere("该类型不支持大小比较");
    }

    case Bin::And:
        return Value::makeBool(truthy(l) && truthy(r));

    case Bin::Or:
        return Value::makeBool(truthy(l) || truthy(r));

    case Bin::In: {
        if (r.kind == Value::Kind::List && r.list) {
            for (const auto& e : *r.list)
                if (valuesEqual(l, e)) return Value::makeBool(true);
            return Value::makeBool(false);
        }
        if (r.kind == Value::Kind::Str && l.kind == Value::Kind::Str)
            return Value::makeBool(r.s.find(l.s) != std::string::npos);
        if (r.kind == Value::Kind::Range) {
            if (l.kind != Value::Kind::Int)
                panicHere("'in' range 需要整数成员");
            return Value::makeBool(l.i >= 0 && l.i < r.i);
        }
        panicHere("'in' 仅支持 list/str/range 右操作数");
    }
    }
    panicHere("运算符不支持的操作数类型");
}

Value VM::doUnaryOp(Unary op, const Value& v) {
    switch (op) {
    case Unary::Neg:
        if (v.kind == Value::Kind::Int) {
            if (v.i == INT64_MIN) panicHere("整数取负溢出");
            return Value::makeInt(-v.i);
        }
        if (v.kind == Value::Kind::Float) return Value::makeFloat(-v.f);
        if (v.kind == Value::Kind::Bool) return Value::makeInt(v.b ? -1 : 0);
        panicHere("取负不支持该类型");
    case Unary::Pos:
        if (v.kind == Value::Kind::Int) return v;
        if (v.kind == Value::Kind::Float) return v;
        if (v.kind == Value::Kind::Bool)
            return Value::makeInt(v.b ? 1 : 0);
        panicHere("一元 '+' 不支持该类型");
    case Unary::Not:
        return Value::makeBool(!truthy(v));
    }
    panicHere("VM 内部错误：未知一元运算");
}

int64_t VM::normIndex(int64_t idx, int64_t len, const char* what) {
    int64_t orig = idx;
    if (idx < 0) idx += len; // Python 语义：负下标从尾部计
    if (idx < 0 || idx >= len)
        panicHere(std::string(what) + "索引越界：" + std::to_string(orig) +
                  "（长度 " + std::to_string(len) + "）");
    return idx;
}

Value VM::repeatStr(const std::string& s, int64_t n) {
    if (n < 0) n = 0; // Python 语义：负次数 → 空串
    constexpr int64_t kMaxBytes = int64_t(64) << 20; // 64MB 护栏
    if (n != 0 && static_cast<int64_t>(s.size()) > kMaxBytes / n)
        panicHere("字符串重复结果过长（上限 64MB）");
    std::string out;
    out.reserve(s.size() * static_cast<size_t>(n));
    for (int64_t k = 0; k < n; ++k) out += s;
    return Value::makeStr(std::move(out));
}

// ----------------------------------------------------------------------------
// 张量执行（P1）：逐元素 + 标量广播、@、下标读写
// ----------------------------------------------------------------------------

/// 张量侧元素按 f64 读取（I64 张量提升为 double）。
double tensorAsF64(const TensorData& t, size_t k) {
    return t.isInt ? static_cast<double>(t.i64[k]) : t.f64[k];
}

Value VM::tensorBinOp(Bin op, const Value& l, const Value& r) {
    const bool lt = l.kind == Value::Kind::Tensor;
    const bool rt = r.kind == Value::Kind::Tensor;
    if (lt && !l.tensor) panicHere("VM 内部错误：空张量对象");
    if (rt && !r.tensor) panicHere("VM 内部错误：空张量对象");
    const TensorData* ta = lt ? l.tensor.get() : nullptr;
    const TensorData* tb = rt ? r.tensor.get() : nullptr;

    // 标量操作数必须为数值（str 等类型检查器已拒绝，此处防御）。
    if (!lt && !isNumericKind(l.kind)) panicHere("运算符不支持的操作数类型");
    if (!rt && !isNumericKind(r.kind)) panicHere("运算符不支持的操作数类型");

    if (lt && rt && ta->shape != tb->shape)
        panicHere("张量形状不兼容：" + shapeToString(ta->shape) + " 与 " +
                  shapeToString(tb->shape));

    // 结果元素类型：int∘int → int；真除法 "/" 恒为 float（与标量语义一致）。
    const bool lInt = lt ? ta->isInt : (l.kind != Value::Kind::Float);
    const bool rInt = rt ? tb->isInt : (r.kind != Value::Kind::Float);
    const bool outInt = op != Bin::Div && lInt && rInt;

    const TensorData& a = lt ? *ta : *tb; // 张量侧（双侧时取左）
    const Value& sc = lt ? r : l;         // 标量侧（双侧时未用）
    const size_t n = a.size();

    auto td = std::make_shared<TensorData>();
    td->isInt = outInt;
    td->shape = a.shape;
    if (outInt) {
        td->i64.resize(n);
        for (size_t k = 0; k < n; ++k) {
            // outInt 时张量侧必为 I64；标量侧经 toInt 取整。
            const int64_t x = lt ? ta->i64[k] : toInt(sc);
            const int64_t y = rt ? tb->i64[k] : toInt(sc);
            td->i64[k] = tensorIntOp(op, x, y);
        }
    } else {
        td->f64.resize(n);
        for (size_t k = 0; k < n; ++k) {
            const double x = lt ? tensorAsF64(*ta, k) : asDouble(sc);
            const double y = rt ? tensorAsF64(*tb, k) : asDouble(sc);
            td->f64[k] = tensorF64Op(op, x, y);
        }
    }
    return Value::makeTensor(std::move(td));
}

int64_t VM::tensorIntOp(Bin op, int64_t x, int64_t y) {
    switch (op) {
    case Bin::Add: {
        int64_t o = 0;
        if (__builtin_add_overflow(x, y, &o)) panicHere("整数加法溢出");
        return o;
    }
    case Bin::Sub: {
        int64_t o = 0;
        if (__builtin_sub_overflow(x, y, &o)) panicHere("整数减法溢出");
        return o;
    }
    case Bin::Mul: {
        int64_t o = 0;
        if (__builtin_mul_overflow(x, y, &o)) panicHere("整数乘法溢出");
        return o;
    }
    case Bin::FloorDiv: {
        if (y == 0) panicHere("整数整除零");
        if (x == INT64_MIN && y == -1) panicHere("整数整除溢出");
        int64_t q = x / y;
        if (x % y != 0 && ((x < 0) != (y < 0))) --q; // Python 向下取整
        return q;
    }
    case Bin::Mod: {
        if (y == 0) panicHere("整数取模零");
        if (x == INT64_MIN && y == -1) return 0;
        int64_t m = x % y;
        if (m != 0 && ((x < 0) != (y < 0))) m += y; // Python 语义
        return m;
    }
    case Bin::Pow: {
        int64_t o = 0;
        if (y < 0 || !ipowInt(x, y, o)) panicHere("整数幂运算溢出");
        return o;
    }
    default:
        break; // Div 恒为 float，不会进入 I64 路径
    }
    panicHere("VM 内部错误：未知张量整数运算");
}

double VM::tensorF64Op(Bin op, double x, double y) {
    switch (op) {
    case Bin::Add: return x + y;
    case Bin::Sub: return x - y;
    case Bin::Mul: return x * y;
    case Bin::Div:
        if (y == 0.0) panicHere("浮点除零");
        return x / y;
    case Bin::FloorDiv:
        if (y == 0.0) panicHere("浮点整除零");
        return std::floor(x / y);
    case Bin::Mod: {
        if (y == 0.0) panicHere("浮点取模零");
        double m = std::fmod(x, y);
        if (m != 0.0 && ((m < 0.0) != (y < 0.0))) m += y; // 与除数同号
        return m;
    }
    case Bin::Pow:
        if (x == 0.0 && y < 0.0) panicHere("零的负次幂");
        return std::pow(x, y);
    default:
        break;
    }
    panicHere("VM 内部错误：未知张量浮点运算");
}

Value VM::tensorMatMul(const Value& l, const Value& r) {
    if (l.kind != Value::Kind::Tensor || r.kind != Value::Kind::Tensor ||
        !l.tensor || !r.tensor)
        panicHere("matmul（@）要求两个二维张量");
    const TensorData& a = *l.tensor;
    const TensorData& b = *r.tensor;
    if (a.shape.size() != 2 || b.shape.size() != 2)
        panicHere("matmul（@）要求两个二维张量，实际 " +
                  shapeToString(a.shape) + " 与 " + shapeToString(b.shape));
    if (a.shape[1] != b.shape[0])
        panicHere("matmul 维度不匹配：" + shapeToString(a.shape) + " 与 " +
                  shapeToString(b.shape) + "（左列数 != 右行数）");

    const int64_t m = a.shape[0];
    const int64_t k = a.shape[1];
    const int64_t n = b.shape[1];
    auto td = std::make_shared<TensorData>();
    td->isInt = a.isInt && b.isInt;
    td->shape = {m, n};
    const size_t count = static_cast<size_t>(m * n);
    if (td->isInt) {
        td->i64.resize(count);
        for (int64_t i = 0; i < m; ++i) {
            for (int64_t j = 0; j < n; ++j) {
                int64_t acc = 0;
                for (int64_t p = 0; p < k; ++p) {
                    const int64_t av = a.i64[static_cast<size_t>(i * k + p)];
                    const int64_t bv = b.i64[static_cast<size_t>(p * n + j)];
                    int64_t prod = 0;
                    if (__builtin_mul_overflow(av, bv, &prod) ||
                        __builtin_add_overflow(acc, prod, &acc))
                        panicHere("整数乘法溢出");
                }
                td->i64[static_cast<size_t>(i * n + j)] = acc;
            }
        }
    } else {
        td->f64.resize(count);
        for (int64_t i = 0; i < m; ++i) {
            for (int64_t j = 0; j < n; ++j) {
                double acc = 0.0;
                for (int64_t p = 0; p < k; ++p) {
                    acc += tensorAsF64(a, static_cast<size_t>(i * k + p)) *
                           tensorAsF64(b, static_cast<size_t>(p * n + j));
                }
                td->f64[static_cast<size_t>(i * n + j)] = acc;
            }
        }
    }
    return Value::makeTensor(std::move(td));
}

Value VM::tensorNew(uint32_t n) {
    // 弹出 n 个元素；栈序与源序相反，先反转。
    std::vector<Value> es;
    es.reserve(n);
    for (uint32_t k = 0; k < n; ++k) es.push_back(pop());
    std::reverse(es.begin(), es.end());

    bool allScalar = true;
    for (const Value& v : es) {
        if (v.kind == Value::Kind::Int || v.kind == Value::Kind::Float) {
            // 标量：保持 allScalar
        } else if (v.kind == Value::Kind::Tensor && v.tensor) {
            allScalar = false;
        } else {
            panicHere("NewTensor 元素必须为数值或张量");
        }
    }

    auto td = std::make_shared<TensorData>();
    if (allScalar) { // 一维：dtype 按元素实际 kind（任一 Float → f64）
        bool anyFloat = false;
        for (const Value& v : es) anyFloat |= v.kind == Value::Kind::Float;
        td->isInt = !anyFloat;
        td->shape = {static_cast<int64_t>(n)};
        if (td->isInt) {
            td->i64.resize(n);
            for (uint32_t k = 0; k < n; ++k) td->i64[k] = toInt(es[k]);
        } else {
            td->f64.resize(n);
            for (uint32_t k = 0; k < n; ++k) td->f64[k] = asDouble(es[k]);
        }
        return Value::makeTensor(std::move(td));
    }

    // 全张量行拼接：行形状一致校验（检查器 E2004 已挡，此处防御）。
    const std::vector<int64_t>& row = es[0].tensor->shape;
    for (size_t k = 1; k < es.size(); ++k) {
        if (es[k].tensor->shape != row)
            panicHere("NewTensor 行形状不一致：" + shapeToString(row) +
                      " 与 " + shapeToString(es[k].tensor->shape));
    }
    bool allInt = true;
    for (const Value& v : es) allInt &= v.tensor->isInt;
    td->isInt = allInt;
    td->shape.push_back(static_cast<int64_t>(es.size()));
    td->shape.insert(td->shape.end(), row.begin(), row.end());
    for (const Value& v : es) {
        const TensorData& t = *v.tensor;
        if (td->isInt) {
            td->i64.insert(td->i64.end(), t.i64.begin(), t.i64.end());
        } else if (t.isInt) { // 混合 dtype → 提升 f64（对齐 unifyNumeric）
            for (int64_t x : t.i64) td->f64.push_back(static_cast<double>(x));
        } else {
            td->f64.insert(td->f64.end(), t.f64.begin(), t.f64.end());
        }
    }
    return Value::makeTensor(std::move(td));
}

Value VM::tensorIndexRead(const Value& base, std::vector<Value>& idxs) {
    const TensorData& t = *base.tensor;
    const size_t rank = t.shape.size();
    const size_t n = idxs.size();
    if (n > rank)
        panicHere("下标数量 " + std::to_string(n) + " 超过张量秩 " +
                  std::to_string(rank));
    int64_t flat = 0;
    for (size_t k = 0; k < n; ++k) {
        const int64_t i = normIndex(toInt(idxs[k]), t.shape[k], "张量");
        flat = flat * t.shape[k] + i;
    }
    if (n == rank) {
        const size_t off = static_cast<size_t>(flat);
        return t.isInt ? Value::makeInt(t.i64[off])
                       : Value::makeFloat(t.f64[off]);
    }
    // 部分索引 → 子张量拷贝（行主序连续切片）。
    int64_t rest = 1;
    for (size_t k = n; k < rank; ++k) rest *= t.shape[k];
    auto td = std::make_shared<TensorData>();
    td->isInt = t.isInt;
    td->shape.assign(t.shape.begin() + static_cast<std::ptrdiff_t>(n),
                     t.shape.end());
    const size_t start = static_cast<size_t>(flat) * static_cast<size_t>(rest);
    const auto beginOff = static_cast<std::ptrdiff_t>(start);
    const auto endOff = static_cast<std::ptrdiff_t>(start) +
                        static_cast<std::ptrdiff_t>(rest);
    if (t.isInt)
        td->i64.assign(t.i64.begin() + beginOff, t.i64.begin() + endOff);
    else
        td->f64.assign(t.f64.begin() + beginOff, t.f64.begin() + endOff);
    return Value::makeTensor(std::move(td));
}

void VM::tensorIndexStore(Value& base, std::vector<Value>& idxs,
                          const Value& val) {
    TensorData& t = *base.tensor;
    const size_t rank = t.shape.size();
    if (idxs.size() != rank)
        panicHere("张量下标赋值需提供全部 " + std::to_string(rank) +
                  " 维索引（当前 " + std::to_string(idxs.size()) + " 个）");
    int64_t flat = 0;
    for (size_t k = 0; k < idxs.size(); ++k) {
        const int64_t i = normIndex(toInt(idxs[k]), t.shape[k], "张量");
        flat = flat * t.shape[k] + i;
    }
    const size_t off = static_cast<size_t>(flat);
    if (!isNumericKind(val.kind))
        panicHere("元素类型不匹配：张量元素必须为数值");
    if (t.isInt) {
        if (val.kind == Value::Kind::Float) {
            // 检查器已保证 int 元素不接受浮点赋值；此处防御性收窄。
            if (val.f != std::floor(val.f))
                panicHere("元素类型不匹配：浮点值无法赋给整型张量元素");
            t.i64[off] = static_cast<int64_t>(val.f);
        } else {
            t.i64[off] = toInt(val);
        }
    } else {
        t.f64[off] = asDouble(val);
    }
}

// ----------------------------------------------------------------------------
// panic 与恢复
// ----------------------------------------------------------------------------

void VM::panicHere(const std::string& msg) {
    Panic p;
    p.message = msg;
    if (!frames_.empty()) {
        const Frame& f = frames_.back();
        p.func = displayName(*f.fn);
        p.module = f.fn->module;
        uint32_t pc = f.pc > 0 ? f.pc - 1 : 0;
        for (const auto& entry : f.fn->lines) {
            if (entry.first > pc) break;
            p.line = entry.second;
        }
    }
    throw RuntimePanic{std::move(p)};
}

bool VM::recover(const Panic& p) {
    lastPanic_ = p; // 已捕获同样记录（诊断可观测）
    if (catches_.empty()) return false;
    CatchPoint cp = catches_.back();
    catches_.pop_back();
    if (cp.frameIndex >= frames_.size()) return false; // 防御：catch 栈损坏
    frames_.resize(cp.frameIndex + 1);                 // 丢弃更深的调用帧
    Frame& f = frames_.back();
    if (f.stack.size() > cp.stackDepth) f.stack.resize(cp.stackDepth);
    f.pc = cp.handlerPc;
    return true;
}

} // namespace bc
} // namespace scourge
