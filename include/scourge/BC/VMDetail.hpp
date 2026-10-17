// ============================================================================
//  Scourge 编译器 — 字节码 VM · 值辅助（VM*.cpp 分块专用，内部共享头）
// ----------------------------------------------------------------------------
//  职责边界：Value 上的纯函数——数值判定/取值（isNumericKind / toInt /
//  asDouble）、真值（truthy）、相等（valuesEqual，数值跨型提升与 str 字节
//  比较的唯一语义源）、整数幂溢出仿真（ipowInt）、显示格式化
//  （shapeToString / escapeStr / floatToString / formatTensorRec / toString /
//  displayName）。原 VM.cpp 匿名命名空间实现原地转 inline，语义逐字节不变；
//  panic 消息纪律与 AOT kRuntimeC 逐字对齐（双模差分由 verify 探针锁定）。
//  仅供 src/BC/VM*.cpp 引用。
// ============================================================================
#pragma once

#include "scourge/BC/VM.hpp"

#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace scourge {
namespace bc {

// ----------------------------------------------------------------------------
// 真并发 M1：共享世界（仅 VMThread.cpp 与 VM 构造消费）
// ----------------------------------------------------------------------------
/// 通道：无界消息队列（M1 处方：send 永不阻塞）+ 关闭旗标。锁序纪律：
/// 恒 gil → Chan::m（chan_recv 在 gil 已放后持 m 等 cv，醒来先取回 gil），
/// 任何路径不得反向，否则 send/recv/close 三方互锁。
struct Chan {
    std::deque<std::string> q;
    std::mutex m;
    std::condition_variable cv;
    bool closed = false;
};

/// 主实例与全部 worker 共享。registry（threads_/locks_/chans_）为 1 起始
/// 句柄表、0 = 空槽（join/close 后回收）；全部操作都在 GIL 持有下执行，
/// 故无需独立注册表锁。rngState：random/seed 的 splitmix64 状态（与 AOT
/// 进程级单例同语义，worker 共享保证双模序列一致）。
struct ThreadWorld {
    std::mutex gil;
    std::shared_ptr<std::vector<Value>> globals =
        std::make_shared<std::vector<Value>>();
    std::vector<std::unique_ptr<std::thread>> threads;
    std::vector<std::shared_ptr<std::mutex>> locks;
    std::vector<std::shared_ptr<Chan>> chans;
    uint64_t rngState = 0;
};

// ----------------------------------------------------------------------------
// 值辅助
// ----------------------------------------------------------------------------

inline bool isNumericKind(Value::Kind k) {
    return k == Value::Kind::Int || k == Value::Kind::Float ||
           k == Value::Kind::Bool;
}

inline int64_t toInt(const Value& v) {
    switch (v.kind) {
    case Value::Kind::Int: return v.i;
    case Value::Kind::Bool: return v.b ? 1 : 0;
    default: return 0; // 调用方已保证数值种类
    }
}

inline double asDouble(const Value& v) {
    switch (v.kind) {
    case Value::Kind::Float: return v.f;
    case Value::Kind::Bool: return v.b ? 1.0 : 0.0;
    default: return static_cast<double>(v.i);
    }
}

inline bool truthy(const Value& v) {
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

inline bool valuesEqual(const Value& l, const Value& r);

inline bool valuesEqual(const Value& l, const Value& r) {
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
    case Value::Kind::Tuple: {
        // 与 List 同构比较（元素可异构；数值族跨类型相等由递归处理）。
        if (l.list.get() == r.list.get()) return true;
        if (!l.list || !r.list || l.list->size() != r.list->size()) return false;
        for (size_t k = 0; k < l.list->size(); ++k)
            if (!valuesEqual((*l.list)[k], (*r.list)[k])) return false;
        return true;
    }
    case Value::Kind::Json: {
        // 结构递归相等（0.9.4 M 专项）：形态、标量值、数组元素、对象成员
        // （键集与值，忽略插入序）逐项一致。
        if (l.json.get() == r.json.get()) return true;
        if (!l.json || !r.json || l.json->jk != r.json->jk) return false;
        const JsonData& a = *l.json;
        const JsonData& b = *r.json;
        switch (a.jk) {
        case JsonData::JK::Null: return true;
        case JsonData::JK::Bool: return a.b == b.b;
        case JsonData::JK::Int: return a.i == b.i;
        case JsonData::JK::Float: return a.f == b.f;
        case JsonData::JK::Str: return a.s == b.s;
        case JsonData::JK::Arr: {
            size_t na = a.arr ? a.arr->size() : 0;
            size_t nb = b.arr ? b.arr->size() : 0;
            if (na != nb) return false;
            for (size_t k = 0; k < na; ++k)
                if (!valuesEqual((*a.arr)[k], (*b.arr)[k])) return false;
            return true;
        }
        case JsonData::JK::Obj: {
            size_t na = a.obj ? a.obj->entries.size() : 0;
            size_t nb = b.obj ? b.obj->entries.size() : 0;
            if (na != nb) return false;
            for (const auto& e : a.obj->entries) {
                uint32_t f = b.obj->find(e.key);
                if (f == DictData::kEmpty) return false;
                if (!valuesEqual(e.val, b.obj->entries[f].val)) return false;
            }
            return true;
        }
        }
        return false;
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
inline bool ipowInt(int64_t base, int64_t exp, int64_t& out) {
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
inline std::string shapeToString(const std::vector<int64_t>& shape) {
    std::string out = "(";
    for (size_t k = 0; k < shape.size(); ++k) {
        if (k) out += ", ";
        out += std::to_string(shape[k]);
    }
    out += ")";
    return out;
}

inline std::string escapeStr(const std::string& s) {
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
inline std::string floatToString(double d) {
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
inline void formatTensorRec(const TensorData& t, size_t dim, size_t offset,
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

inline std::string toString(const Value& v, bool repr) {
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
    case Value::Kind::Dict: {
        // Python 风格 repr：{"a": 1, 2.5: "x"}；键为 str 时带引号转义。
        std::string out = "{";
        if (v.dict) {
            bool first = true;
            for (const auto& e : v.dict->entries) {
                if (!first) out += ", ";
                first = false;
                out += toString(e.key, true);
                out += ": ";
                out += toString(e.val, true);
            }
        }
        out += "}";
        return out;
    }
    case Value::Kind::Set: {
        // Python 风格 repr：{1, 2, 3}；元素为 str 时带引号转义；迭代序 =
        // 首插序（与 for 一致，确定性）。
        std::string out = "{";
        if (v.set) {
            bool first = true;
            for (const auto& e : v.set->entries) {
                if (!first) out += ", ";
                first = false;
                out += toString(e, true);
            }
        }
        out += "}";
        return out;
    }
    case Value::Kind::Tuple: {
        // Python 风格 repr：(1, "a", (2,))——括号包裹、", " 连接、元素一律
        // repr（str 带引号转义）、单元素尾逗号、空 ()。
        std::string out = "(";
        if (v.list) {
            for (size_t k = 0; k < v.list->size(); ++k) {
                if (k) out += ", ";
                out += toString((*v.list)[k], true);
            }
            if (v.list->size() == 1) out += ',';
        }
        out += ")";
        return out;
    }
    case Value::Kind::Json:
        // 紧凑 JSON 文本（与 dump()/AOT sg_print_json 逐字一致；JSON 本身
        // 的字符串引号/转义即 repr 形态，repr 参数不改变输出）。
        return jsonDump(v);
    case Value::Kind::Range:
        return "range(0, " + std::to_string(v.i) + ")";    case Value::Kind::Result: {
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

inline std::string displayName(const FuncBC& f) {
    if (f.isTopLevel) return "<top " + f.module + ">";
    if (f.isBuiltin) return f.builtinName;
    return f.module.empty() ? f.name : f.module + "." + f.name;
}

} // namespace bc
} // namespace scourge
