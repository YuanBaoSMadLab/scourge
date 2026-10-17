// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 字节码 VM · 运算与张量（panic 语义集中区）
// ----------------------------------------------------------------------------
//  职责边界：doBinOp / doUnaryOp / normIndex / repeatStr / tensorBinOp /
//  tensorIntOp / tensorF64Op / tensorMatMul / tensorNew / tensorIndexRead /
//  tensorIndexStore——标量与张量全量运算，溢出/除零/越界 panic 消息与
//  AOT kRuntimeC 逐字一致。其余阶段见同目录 VM*.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/BC/VM.hpp"
#include "scourge/BC/VMDetail.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_set>

#include "scourge/Common/Capability.hpp"

namespace scourge {
namespace bc {

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
        if (r.kind == Value::Kind::Dict && r.dict) {
            if (l.kind == Value::Kind::Float && std::isnan(l.f))
                panicHere("字典键不能为 NaN");
            return Value::makeBool(r.dict->find(l) != DictData::kEmpty);
        }
        if (r.kind == Value::Kind::Set && r.set) {
            if (l.kind == Value::Kind::Float && std::isnan(l.f))
                panicHere("集合元素不能为 NaN");
            return Value::makeBool(r.set->find(l) != SetData::kEmpty);
        }
        if (r.kind == Value::Kind::Str && l.kind == Value::Kind::Str)
            return Value::makeBool(r.s.find(l.s) != std::string::npos);
        if (r.kind == Value::Kind::Range) {
            if (l.kind != Value::Kind::Int)
                panicHere("'in' range 需要整数成员");
            return Value::makeBool(l.i >= 0 && l.i < r.i);
        }
        panicHere("'in' 仅支持 list/str/range/dict/Set 右操作数");
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

} // namespace bc
} // namespace scourge
