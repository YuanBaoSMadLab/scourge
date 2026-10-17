// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 字节码 VM · 内建函数执行
// ----------------------------------------------------------------------------
//  职责边界：doBuiltin——内建名 → 执行的白名单分派（数学/随机/排序/类型/
//  容器/文件/字符串工具等）；net_*/sleep/std/json/@extern 已分流至
//  VMNet/VMStd/VMJson/VMExtern.cpp。panic 消息与 AOT kRuntimeC 逐字一致。
//  其余阶段见同目录 VM*.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/BC/VM.hpp"
#include "scourge/BC/VMDetail.hpp"

#include <algorithm>
#include <cerrno>
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

// ============================================================================
// fmt() 格式化（0.9.9，f-string :spec 降糖目标）
// ----------------------------------------------------------------------------
// spec 文法与静态约束见 TypeCheckerCall.cpp validateFmtSpec（Sema 已拒绝
// 非法 spec，此处不设防）。实现与 AOT sg_fmt_*（BuilderRTStr.hpp）逐字
// 同构：snprintf 产主体 + UTF-8 码点计宽填充——同库同机下双模逐字节一致。
// ============================================================================
namespace {

struct FmtSpec {
    char fill = ' ';
    char align = 0; // 0 = 类型默认（数值右对齐、str 左对齐）
    bool plus = false;
    bool zero = false;
    int width = 0;
    int prec = -1; // -1 = 未指定
    char type = 0;
};

FmtSpec parseFmtSpec(const std::string& s) {
    FmtSpec f;
    size_t i = 0;
    auto isAlign = [](char c) { return c == '<' || c == '>' || c == '^'; };
    if (i < s.size() && isAlign(s[i])) {
        f.align = s[i++];
    } else if (s.size() - i >= 2 && isAlign(s[i + 1])) {
        f.fill = s[i];
        f.align = s[i + 1];
        i += 2;
    }
    if (i < s.size() && s[i] == '+') { f.plus = true; ++i; }
    if (i < s.size() && s[i] == '0' && i + 1 < s.size() &&
        s[i + 1] >= '0' && s[i + 1] <= '9') {
        f.zero = true; ++i;
    }
    while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        f.width = f.width * 10 + (s[i++] - '0');
    if (i < s.size() && s[i] == '.') {
        f.prec = 0;
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9')
            f.prec = f.prec * 10 + (s[i++] - '0');
    }
    if (i < s.size()) f.type = s[i];
    return f;
}

/// UTF-8 码点数（续字节 10xxxxxx 不计）。
size_t cpCount(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

/// 截断到前 prec 个码点（fmt_str 专用）。
std::string cpTrunc(const std::string& s, size_t prec) {
    size_t n = 0;
    for (size_t k = 0; k < s.size(); ++k) {
        if ((static_cast<unsigned char>(s[k]) & 0xC0) != 0x80) {
            if (n == prec) return s.substr(0, k);
            ++n;
        }
    }
    return s;
}

/// 码点计宽填充：'>' 左填充 / '<' 右填充 / '^' 居中（余数偏右，与 Python
/// 一致）；缺省对齐由调用方按类型传（数值 '>'、str '<'）。fill 取单字节
/// （ASCII 填充符为常规用法；非 ASCII fill 两模同字节处理，不破坏一致）。
std::string cpPad(const std::string& body, const FmtSpec& f, char defAlign) {
    char align = f.align ? f.align : defAlign;
    size_t cps = cpCount(body);
    size_t pad = f.width > static_cast<int>(cps)
                     ? static_cast<size_t>(f.width) - cps
                     : 0;
    if (align == '<') return body + std::string(pad, f.fill);
    if (align == '^') {
        size_t left = pad / 2;
        return std::string(left, f.fill) + body +
               std::string(pad - left, f.fill);
    }
    return std::string(pad, f.fill) + body;
}

/// 数值主体统一处理：符号剥离 → 零填充（旗标时插符号后）→ 宽度填充。
static std::string fmtSigned(const std::string& num, bool neg,
                             const FmtSpec& f) {
    std::string sign = neg ? "-" : (f.plus ? "+" : "");
    if (f.zero && !f.align) {
        size_t body = sign.size() + num.size();
        size_t zeros = f.width > static_cast<int>(body)
                           ? static_cast<size_t>(f.width) - body
                           : 0;
        return sign + std::string(zeros, '0') + num;
    }
    return cpPad(sign + num, f, '>');
}

std::string fmtI64(int64_t v, const FmtSpec& f) {
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    std::string num = buf;
    return fmtSigned(num[0] == '-' ? num.substr(1) : num, v < 0, f);
}

std::string fmtF64(double v, const FmtSpec& f) {
    int prec = f.prec >= 0 ? f.prec : 6;
    char type = f.type ? f.type : 'f';
    char buf[512];
    if (type == '%') {
        snprintf(buf, sizeof buf, "%.*f%%", prec, v * 100.0);
    } else {
        char fs[16];
        snprintf(fs, sizeof fs, "%%.%d%c", prec, type); // "%.Nf/e/g"
        snprintf(buf, sizeof buf, fs, v);
    }
    std::string num = buf;
    return fmtSigned(num[0] == '-' ? num.substr(1) : num, v < 0, f);
}

std::string fmtStr(const std::string& v, const FmtSpec& f) {
    std::string body =
        f.prec >= 0 ? cpTrunc(v, static_cast<size_t>(f.prec)) : v;
    return cpPad(body, f, '<');
}

} // namespace

void VM::doBuiltin(const FuncBC& fn, uint32_t argc) {
    std::vector<Value> a(argc);
    for (uint32_t k = argc; k-- > 0;) a[k] = pop();

    const std::string& n = fn.builtinName;
    // 能力运行时门禁（0.9.7）：静态检查（E5004）已挡住正常程序，此处是
    // 纵深防御——SG-BC 缓存被篡改/手搓字节码时，环境交互内建仍不可绕过。
    // 消息与 AOT sg_cap_check 逐字一致（Common/Capability.hpp 单点生成）。
    {
        Cap cap;
        if (capOfBuiltin(n, cap) && !(program_.capMask & capBit(cap)))
            panicHere(capPanicMsg(cap, n));
    }
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
    // 0.9.9 类型转换/码点（纯函数；panic 消息与 AOT sg_* 运行时逐字一致）。
    if (n == "int") {
        need(1);
        const Value& v = a[0];
        if (v.kind == Value::Kind::Int) {
            push(v);
            return;
        }
        if (v.kind == Value::Kind::Float) {
            // NaN/±inf/越界显式 panic（不回绕）；截断向零（同 trunc 语义）
            if (!(v.f >= -9223372036854775808.0 && v.f < 9223372036854775808.0))
                panicHere("int() 浮点越界");
            push(Value::makeInt(static_cast<int64_t>(v.f)));
            return;
        }
        if (v.kind == Value::Kind::Bool) {
            push(Value::makeInt(v.b ? 1 : 0));
            return;
        }
        if (v.kind == Value::Kind::Str) {
            // 全消费校验：前导/尾随空白或残渣均拒绝（与 strtod 路径同哲学）
            char* end = nullptr;
            errno = 0;
            long long r = std::strtoll(v.s.c_str(), &end, 10);
            if (errno == ERANGE || end == v.s.c_str() || *end != '\0')
                panicHere("int() 无法解析 '" + v.s + "'");
            push(Value::makeInt(static_cast<int64_t>(r)));
            return;
        }
        panicHere("int() 不支持 " + stringify(v));
        return;
    }
    if (n == "float") {
        need(1);
        const Value& v = a[0];
        if (v.kind == Value::Kind::Float) {
            push(v);
            return;
        }
        if (v.kind == Value::Kind::Int) {
            push(Value::makeFloat(static_cast<double>(v.i)));
            return;
        }
        if (v.kind == Value::Kind::Bool) {
            push(Value::makeFloat(v.b ? 1.0 : 0.0));
            return;
        }
        if (v.kind == Value::Kind::Str) {
            // 仅全消费校验（strtod 上溢 → ±inf 与 Python float() 同语义，
            // 不判 errno；与 AOT sg_str_to_f64 逐字一致）
            char* end = nullptr;
            double r = std::strtod(v.s.c_str(), &end);
            if (end == v.s.c_str() || *end != '\0')
                panicHere("float() 无法解析 '" + v.s + "'");
            push(Value::makeFloat(r));
            return;
        }
        panicHere("float() 不支持 " + stringify(v));
        return;
    }
    if (n == "ord") {
        need(1);
        if (a[0].kind != Value::Kind::Str)
            panicHere("ord() 参数必须是 str");
        // UTF-8 首码点解码；非单码点（空串/多字符/坏序列）panic
        const std::string& s = a[0].s;
        if (s.empty()) panicHere("ord() 需要单字符字符串");
        unsigned char c0 = static_cast<unsigned char>(s[0]);
        int64_t cp = 0;
        size_t len = 0;
        if (c0 < 0x80) {
            cp = c0;
            len = 1;
        } else if ((c0 & 0xE0) == 0xC0) {
            cp = c0 & 0x1F;
            len = 2;
        } else if ((c0 & 0xF0) == 0xE0) {
            cp = c0 & 0x0F;
            len = 3;
        } else if ((c0 & 0xF8) == 0xF0) {
            cp = c0 & 0x07;
            len = 4;
        } else {
            panicHere("ord() 需要单字符字符串");
        }
        if (s.size() != len) panicHere("ord() 需要单字符字符串");
        for (size_t k = 1; k < len; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[k]);
            if ((cc & 0xC0) != 0x80) panicHere("ord() 需要单字符字符串");
            cp = (cp << 6) | (cc & 0x3F);
        }
        push(Value::makeInt(cp));
        return;
    }
    if (n == "chr") {
        need(1);
        if (a[0].kind != Value::Kind::Int)
            panicHere("chr() 参数必须是 int");
        int64_t cp = a[0].i;
        // 代理区（D800-DFFF）非标量码点，一并拒绝
        if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            panicHere("chr() 码点越界： " + std::to_string(cp));
        std::string out;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        push(Value::makeStr(std::move(out)));
        return;
    }
    // fmt(expr, "spec")（0.9.9）：f-string :spec 降糖目标。spec 合法性已由
    // Sema validateFmtSpec 静态保证，此处按值类型分派（与 AOT sg_fmt_*
    // 逐字同构）。
    if (n == "fmt") {
        need(2);
        if (a[1].kind != Value::Kind::Str)
            panicHere("fmt() 格式说明符必须是 str");
        FmtSpec f = parseFmtSpec(a[1].s);
        const Value& v = a[0];
        switch (v.kind) {
        case Value::Kind::Int:
            push(Value::makeStr(fmtI64(v.i, f)));
            return;
        case Value::Kind::Float:
            push(Value::makeStr(fmtF64(v.f, f)));
            return;
        case Value::Kind::Str:
            push(Value::makeStr(fmtStr(v.s, f)));
            return;
        case Value::Kind::Bool:
            push(Value::makeStr(fmtStr(v.b ? "True" : "False", f)));
            return;
        default:
            panicHere("fmt() 不支持 " + stringify(v));
        }
    }
    if (n == "len") {
        need(1);
        if (a[0].kind == Value::Kind::List && a[0].list)
            push(Value::makeInt(static_cast<int64_t>(a[0].list->size())));
        else if (a[0].kind == Value::Kind::Tuple && a[0].list)
            push(Value::makeInt(static_cast<int64_t>(a[0].list->size())));
        else if (a[0].kind == Value::Kind::Dict && a[0].dict)
            push(Value::makeInt(
                static_cast<int64_t>(a[0].dict->entries.size())));
        else if (a[0].kind == Value::Kind::Set && a[0].set)
            push(Value::makeInt(
                static_cast<int64_t>(a[0].set->entries.size())));
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
        // List 归约（0.9.9）：任一 float 元素 → f64 累加（int/bool 提升），
        // 否则 i64 精确累加（bool 按 0/1）；空列表 = 0（与张量空和一致）。
        if (a[0].kind == Value::Kind::List && a[0].list) {
            bool anyFloat = false;
            for (const Value& e : *a[0].list)
                if (e.kind == Value::Kind::Float) {
                    anyFloat = true;
                    break;
                }
            if (anyFloat) {
                double acc = 0.0;
                for (const Value& e : *a[0].list) {
                    if (e.kind == Value::Kind::Float) acc += e.f;
                    else if (e.kind == Value::Kind::Int)
                        acc += static_cast<double>(e.i);
                    else if (e.kind == Value::Kind::Bool)
                        acc += e.b ? 1.0 : 0.0;
                }
                push(Value::makeFloat(acc));
            } else {
                int64_t acc = 0;
                for (const Value& e : *a[0].list) {
                    int64_t x = e.kind == Value::Kind::Int
                                    ? e.i
                                    : (e.kind == Value::Kind::Bool && e.b ? 1
                                                                          : 0);
                    int64_t out = 0;
                    if (__builtin_add_overflow(acc, x, &out))
                        panicHere("整数求和溢出");
                    acc = out;
                }
                push(Value::makeInt(acc));
            }
            return;
        }
        panicHere("sum() 仅支持张量或数值列表");
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
    // max/min：张量/列表归约到元素类型标量（0.9.9 接 List；空序列 panic
    // 消息与统计内建同款，AOT sg_list_reduce_* 逐字一致）。
    if (n == "max" || n == "min") {
        need(1);
        if (a[0].kind == Value::Kind::List && a[0].list) {
            if (a[0].list->empty()) panicHere(n + "() 不支持空序列");
            bool anyFloat = false;
            for (const Value& e : *a[0].list)
                if (e.kind == Value::Kind::Float) {
                    anyFloat = true;
                    break;
                }
            const bool wantMax = (n == "max");
            if (anyFloat) {
                double best = 0.0;
                bool first = true;
                for (const Value& e : *a[0].list) {
                    double x = e.kind == Value::Kind::Float
                                   ? e.f
                                   : e.kind == Value::Kind::Int
                                         ? static_cast<double>(e.i)
                                         : (e.b ? 1.0 : 0.0);
                    best = first       ? x
                           : wantMax   ? (x > best ? x : best)
                                       : (x < best ? x : best);
                    first = false;
                }
                push(Value::makeFloat(best));
            } else {
                int64_t best = 0;
                bool first = true;
                for (const Value& e : *a[0].list) {
                    int64_t x = e.kind == Value::Kind::Int
                                    ? e.i
                                    : (e.kind == Value::Kind::Bool && e.b ? 1
                                                                          : 0);
                    best = first       ? x
                           : wantMax   ? (x > best ? x : best)
                                       : (x < best ? x : best);
                    first = false;
                }
                push(Value::makeInt(best));
            }
            return;
        }
        if (a[0].kind != Value::Kind::Tensor || !a[0].tensor)
            panicHere(n + "() 仅支持张量或 List 归约；其他类型请"
                        "逐元素循环比较");
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
    // data_ptr（O 节 FFI 宽类型域）：取连续原生缓冲首地址（int 承载）。
    // 仅 Tensor[int]/Tensor[float]/str——List 元素是 tagged 值非连续数组，
    // Sema 已拦；此处白名单再拦属防御。存活期契约：张量地址仅在对象
    // 存活期内有效（跨 extern 调用必须把张量保存在变量中——内联链式
    // 使用时 Value 弹栈即析构，双模同此风险，靠文档约定而非实现兜底）；
    // str 地址由 intern 池兜底为进程期稳定（见下方 Str 分支）。
    // 空张量显式 panic：VM 空 vector::data() 可能为 nullptr 而 AOT 恒分配，
    // 是天然双模分歧点，必须在此收敛。
    if (n == "data_ptr") {
        need(1);
        if (a[0].kind == Value::Kind::Tensor && a[0].tensor) {
            const TensorData& t = *a[0].tensor;
            if (t.size() == 0) panicHere("data_ptr() 不支持空张量");
            const void* p = t.isInt ? static_cast<const void*>(t.i64.data())
                                    : static_cast<const void*>(t.f64.data());
            push(Value::makeInt(
                static_cast<int64_t>(reinterpret_cast<intptr_t>(p))));
            return;
        }
        if (a[0].kind == Value::Kind::Str) {
            // str 的 SSO 副本随弹栈 Value 析构即悬空（短串缓冲在 Value
            // 内部，strlen 读到 0/AV），地址必须指向进程期稳定副本——
            // 静态 intern 池深拷贝持有，同内容返回同地址；AOT 侧 str 为
            // 全局常量（llvm.ptrtoint），天然进程期存活。双模契约：
            // str 地址只读透传；池上限显式 panic 而非静默泄漏。
            static std::unordered_set<std::string> pool;
            if (pool.size() >= 4096)
                panicHere("data_ptr() 字符串地址池已满（上限 4096）");
            push(Value::makeInt(static_cast<int64_t>(reinterpret_cast<intptr_t>(
                pool.insert(a[0].s).first->c_str()))));
            return;
        }
        panicHere("data_ptr() 仅支持 Tensor[int]/Tensor[float]/str");
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
        world_->rngState = static_cast<uint64_t>(a[0].i); // 负数按 64 位补码解释
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
    // ---- IO 内建（0.9.4）：函数式文件/目录/标准输入。与 AOT sg_* 运行时的
    // 约定逐字对齐：路径窄字节串；文本一律二进制模式（禁 CRT 文本转换，
    // 否则 CRLF 差异破坏双模逐字节一致）；失败 panic 消息与 sg_* 逐字一致。
    if (n == "input") {
        if (!a.empty()) panicHere("input() 不需要参数");
        // fgetc 逐字节到 '\n'（与 sg_input 同构）；去行尾 '\r'（二进制
        // 重定向时出现，文本模式 stdin 已由 CRT 转换）。
        std::string line;
        int c, got = 0;
        while ((c = std::fgetc(stdin)) != EOF) {
            got = 1;
            if (c == '\n') break;
            line.push_back(static_cast<char>(c));
        }
        if (!got) panicHere("input() 输入流已结束");
        while (!line.empty() && line.back() == '\r') line.pop_back();
        push(Value::makeStr(std::move(line)));
        return;
    }
    if (n == "read_text") {
        need(1);
        if (a[0].kind != Value::Kind::Str)
            panicHere("read_text() 路径参数必须是 str");
        std::ifstream f(a[0].s, std::ios::binary);
        if (!f) panicHere("read_text() 无法打开文件: " + a[0].s);
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        if (f.bad()) panicHere("read_text() 无法读取文件: " + a[0].s);
        push(Value::makeStr(std::move(content)));
        return;
    }
    if (n == "write_text" || n == "append_text") {
        need(2);
        if (a[0].kind != Value::Kind::Str || a[1].kind != Value::Kind::Str)
            panicHere(n + "() 参数必须是 str");
        std::ios::openmode mode = std::ios::binary | std::ios::trunc;
        if (n == "append_text") mode = std::ios::binary | std::ios::app;
        std::ofstream f(a[0].s, mode);
        if (!f) panicHere(n + "() 无法打开文件: " + a[0].s);
        f.write(a[1].s.data(), static_cast<std::streamsize>(a[1].s.size()));
        f.flush();
        if (!f) panicHere(n + "() 写入失败: " + a[0].s);
        push(Value::makeInt(static_cast<int64_t>(a[1].s.size())));
        return;
    }
    if (n == "exists") {
        need(1);
        if (a[0].kind != Value::Kind::Str)
            panicHere("exists() 路径参数必须是 str");
        std::error_code ec;
        push(Value::makeBool(std::filesystem::exists(a[0].s, ec)));
        return;
    }
    if (n == "list_dir") {
        need(1);
        if (a[0].kind != Value::Kind::Str)
            panicHere("list_dir() 路径参数必须是 str");
        std::error_code ec;
        std::filesystem::directory_iterator it(a[0].s, ec);
        if (ec) panicHere("list_dir() 无法打开目录: " + a[0].s);
        auto out = std::make_shared<std::vector<Value>>();
        for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (ec) panicHere("list_dir() 无法读取目录: " + a[0].s);
            // 与 sg_list_dir 对齐：跳过 "." ".."（MSVC iterator 本就不产生，
            // 显式过滤防实现漂移）；按字节序排序（枚举顺序不可依赖——
            // 双模确定性红线）。
            std::string name = it->path().filename().string();
            if (name == "." || name == "..") continue;
            out->push_back(Value::makeStr(std::move(name)));
        }
        std::sort(out->begin(), out->end(),
                  [](const Value& l, const Value& r) { return l.s < r.s; });
        push(Value::makeList(std::move(out)));
        return;
    }
    // ---- 网络/睡眠（F/G 专项）：前缀分派到 VMNet.cpp 分块 ----
    if (n.rfind("net_", 0) == 0 || n == "sleep") {
        netBuiltin(n, a);
        return;
    }
    // ---- 系统编程（H 专项）：白名单分派到 VMSys.cpp 分块 ----
    static const char* const kSysBuiltins[] = {
        "args", "env", "has_env", "set_env", "unset_env", "epoch", "epoch_ms",
        "localtime", "gmtime", "system", "spawn_wait", "signal_caught",
        "signal_reset"};
    for (const char* s : kSysBuiltins) {
        if (n == s) {
            sysBuiltin(n, a);
            return;
        }
    }
    // ---- 标准库广度（M 专项）：白名单分派到 VMStd.cpp 分块 ----
    static const char* const kStdBuiltins[] = {
        "mean", "var", "std", "median", "reversed", "sha256", "crc32"};
    for (const char* s : kStdBuiltins) {
        if (n == s) {
            stdBuiltin(n, a);
            return;
        }
    }
    // ---- JSON（M 专项）：白名单分派到 VMJson.cpp 分块 ----
    static const char* const kJsonBuiltins[] = {"json_decode"};
    for (const char* s : kJsonBuiltins) {
        if (n == s) {
            jsonBuiltin(n, a);
            return;
        }
    }
    // ---- 真并发 M1：白名单分派到 VMThread.cpp 分块 ----
    static const char* const kThreadBuiltins[] = {
        "thread_spawn", "thread_join", "lock_new", "lock_acquire",
        "lock_release", "channel_new", "chan_send", "chan_recv", "chan_close"};
    for (const char* s : kThreadBuiltins) {
        if (n == s) {
            threadBuiltin(n, a);
            return;
        }
    }
    // ---- GUI M1+M2（gui 域）：平台绑定内建显式单模（GPU 先例）——
    // Win32 AOT 专属，VM 侧授予 capability 后运行到仍固定 panic。
    if (n.rfind("gui_", 0) == 0)
        panicHere("gui_* 仅 build 模式可用（GUI 为 Win32 AOT 专属）");
    panicHere("VM 内部错误：未知内建 '" + n + "'");
}

} // namespace bc
} // namespace scourge
