// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge SG-BC 序列化实现 — Program ↔ 字节流
// ----------------------------------------------------------------------------
//  编码顺序（编码器与解码器共用同一份隐式契约，任一侧改动即 bump 版本）：
//    魔数 "SGBC" | u32 格式版本 | 工具链标签 | 入口名 | u64 源指纹
//    | 模块名表 | 模块路径表
//    | 常量池 | 全局名表 | 函数表
//  Value 按类别编码载荷字段；Result 额外携带 isOk 与 payloadKind，
//  成功态按 payloadKind 编码载荷、失败态仅携带错误信息。
//  v2：新增 Tensor 常量（dtype + 形状 + 元素）与 MatMul 指令、
//  LoadIndex/StoreIndex 的 b=下标个数语义（P1 张量执行）。
//  v5：FuncBC 新增 @extern 三元组（kFlagExtern + externDll + externRet，
//  0.9.4 H 专项——旧格式函数无 extern，缺省字段语义安全）。
// ============================================================================
#include "scourge/BC/Serialize.hpp"

#include <cstring>

namespace scourge {
namespace bc {

namespace {

constexpr char kMagic[4] = {'S', 'G', 'B', 'C'};
constexpr uint32_t kFormatVersion = 5;
constexpr uint8_t kOpCount = static_cast<uint8_t>(Op::NewTensor) + 1;

constexpr uint8_t kFlagBuiltin = 1;
constexpr uint8_t kFlagTopLevel = 2;
constexpr uint8_t kFlagAsync = 4;
constexpr uint8_t kFlagExtern = 8;

// ----------------------------------------------------------------------------
// 字节流写端（小端，逐字段显式布局）
// ----------------------------------------------------------------------------
struct Writer {
    std::string b;
    void raw(const void* p, size_t n) {
        b.append(static_cast<const char*>(p), n);
    }
    void u8(uint8_t v) { b.push_back(static_cast<char>(v)); }
    void u32(uint32_t v) { raw(&v, sizeof v); }
    void u64(uint64_t v) { raw(&v, sizeof v); }
    void i64(int64_t v) { raw(&v, sizeof v); }
    void f64(double v) {
        uint64_t t;
        std::memcpy(&t, &v, sizeof t);
        raw(&t, sizeof t);
    }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
};

// ----------------------------------------------------------------------------
// 字节流读端：全程边界校验，一次越界即整体失败
// ----------------------------------------------------------------------------
struct Reader {
    const std::string& b;
    size_t p = 0;
    bool ok = true;

    bool raw(void* dst, size_t n) {
        if (!ok || n > b.size() - p) {
            ok = false;
            return false;
        }
        std::memcpy(dst, b.data() + p, n);
        p += n;
        return true;
    }
    bool u8(uint8_t& v) { return raw(&v, sizeof v); }
    bool u32(uint32_t& v) { return raw(&v, sizeof v); }
    bool u64(uint64_t& v) { return raw(&v, sizeof v); }
    bool i64(int64_t& v) { return raw(&v, sizeof v); }
    bool f64(double& v) {
        uint64_t t;
        if (!raw(&t, sizeof t)) return false;
        std::memcpy(&v, &t, sizeof v);
        return true;
    }
    bool str(std::string& s) {
        uint32_t n;
        if (!u32(n)) return false;
        if (n > b.size() - p) {
            ok = false;
            return false;
        }
        s.assign(b.data() + p, n);
        p += n;
        return true;
    }
    /// 剩余字节数（p 恒 ≤ b.size()）。
    size_t remaining() const { return b.size() - p; }
};

// ----------------------------------------------------------------------------
// Value 编解码
// ----------------------------------------------------------------------------

void encodeValueRec(Writer& w, const Value& v); // 与 encodePayload 互递归

/// 按值类别编码载荷字段（不含 kind 字节）。Result 载荷由调用方特判。
void encodePayload(Writer& w, Value::Kind k, const Value& v) {
    switch (k) {
    case Value::Kind::Int:
        w.i64(v.i);
        break;
    case Value::Kind::Float:
        w.f64(v.f);
        break;
    case Value::Kind::Bool:
        w.u8(v.b ? 1 : 0);
        break;
    case Value::Kind::Str:
        w.str(v.s);
        break;
    case Value::Kind::None:
        break;
    case Value::Kind::List: {
        const uint32_t n = v.list ? static_cast<uint32_t>(v.list->size()) : 0;
        w.u32(n);
        if (v.list)
            for (const auto& el : *v.list) encodeValueRec(w, el);
        break;
    }
    case Value::Kind::Range:
        w.i64(v.i);
        break;
    case Value::Kind::Tensor: {
        if (!v.tensor) { // 防御：空张量按 0 秩 0 元素编码
            w.u8(0);
            w.u32(0);
            w.u32(0);
            break;
        }
        const TensorData& t = *v.tensor;
        w.u8(t.isInt ? 1 : 0);
        w.u32(static_cast<uint32_t>(t.shape.size()));
        for (int64_t d : t.shape) w.i64(d);
        const uint32_t n = static_cast<uint32_t>(t.size());
        w.u32(n);
        if (t.isInt)
            for (int64_t x : t.i64) w.i64(x);
        else
            for (double x : t.f64) w.f64(x);
        break;
    }
    case Value::Kind::Result:
        break; // 当前值模型：载荷不会是 Result
    }
}

void encodeValueRec(Writer& w, const Value& v) {
    w.u8(static_cast<uint8_t>(v.kind));
    if (v.kind == Value::Kind::Result) {
        w.u8(v.isOk ? 1 : 0);
        w.u8(static_cast<uint8_t>(v.payloadKind));
        if (v.isOk)
            encodePayload(w, v.payloadKind, v); // 成功态携带载荷
        else
            w.str(v.s); // 失败态仅携带错误信息
        return;
    }
    encodePayload(w, v.kind, v);
}

bool decodePayload(Reader& r, Value::Kind k, Value& v);

bool decodeValueRec(Reader& r, Value& v) {
    uint8_t k;
    if (!r.u8(k)) return false;
    if (k > static_cast<uint8_t>(Value::Kind::Tensor)) {
        r.ok = false;
        return false;
    }
    v = Value{};
    v.kind = static_cast<Value::Kind>(k);
    if (v.kind == Value::Kind::Result) {
        uint8_t isOk = 0;
        uint8_t pk = 0;
        if (!r.u8(isOk) || !r.u8(pk)) return false;
        if (pk > static_cast<uint8_t>(Value::Kind::Result)) {
            r.ok = false;
            return false;
        }
        v.isOk = isOk != 0;
        v.payloadKind = static_cast<Value::Kind>(pk);
        if (v.isOk) return decodePayload(r, v.payloadKind, v);
        return r.str(v.s);
    }
    return decodePayload(r, v.kind, v);
}

bool decodePayload(Reader& r, Value::Kind k, Value& v) {
    switch (k) {
    case Value::Kind::Int:
        return r.i64(v.i);
    case Value::Kind::Float:
        return r.f64(v.f);
    case Value::Kind::Bool: {
        uint8_t b = 0;
        if (!r.u8(b)) return false;
        v.b = b != 0;
        return true;
    }
    case Value::Kind::Str:
        return r.str(v.s);
    case Value::Kind::None:
        return true;
    case Value::Kind::List: {
        uint32_t n = 0;
        if (!r.u32(n)) return false;
        // 每个元素至少占 1 字节 → 计数不得超出剩余，防虚高 reserve。
        if (n > r.remaining()) {
            r.ok = false;
            return false;
        }
        auto ls = std::make_shared<std::vector<Value>>();
        ls->reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            Value el;
            if (!decodeValueRec(r, el)) return false;
            ls->push_back(std::move(el));
        }
        v.list = std::move(ls);
        return true;
    }
    case Value::Kind::Range:
        return r.i64(v.i);
    case Value::Kind::Tensor: {
        uint8_t isInt = 0;
        if (!r.u8(isInt)) return false;
        uint32_t rank = 0;
        if (!r.u32(rank)) return false;
        if (rank > r.remaining()) { // 每维至少 1 字节，防计数虚高
            r.ok = false;
            return false;
        }
        auto td = std::make_shared<TensorData>();
        td->isInt = isInt != 0;
        td->shape.resize(rank);
        for (auto& d : td->shape)
            if (!r.i64(d)) return false;
        uint32_t n = 0;
        if (!r.u32(n)) return false;
        const size_t elemBytes = td->isInt ? sizeof(int64_t) : sizeof(double);
        if (n > r.remaining() / elemBytes) {
            r.ok = false;
            return false;
        }
        // 形状之积必须等于元素数（一致性与溢出双重校验）。
        int64_t expect = 1;
        for (int64_t d : td->shape) {
            if (d < 0 || __builtin_mul_overflow(expect, d, &expect)) {
                r.ok = false;
                return false;
            }
        }
        if (static_cast<uint64_t>(expect) != n) {
            r.ok = false;
            return false;
        }
        if (td->isInt) {
            td->i64.resize(n);
            for (auto& x : td->i64)
                if (!r.i64(x)) return false;
        } else {
            td->f64.resize(n);
            for (auto& x : td->f64)
                if (!r.f64(x)) return false;
        }
        v.tensor = std::move(td);
        return true;
    }
    case Value::Kind::Result:
        r.ok = false;
        return false;
    }
    return false;
}

// ----------------------------------------------------------------------------
// 函数编解码
// ----------------------------------------------------------------------------

void encodeFunc(Writer& w, const FuncBC& f) {
    w.str(f.name);
    w.str(f.module);
    w.u32(f.paramCount);
    w.u32(f.requiredArgs);
    w.u32(static_cast<uint32_t>(f.defaults.size()));
    for (const auto& d : f.defaults) encodeValueRec(w, d);
    w.u32(f.nlocals);
    w.u32(static_cast<uint32_t>(f.code.size()));
    for (const auto& c : f.code) {
        w.u8(static_cast<uint8_t>(c.op));
        w.u32(c.a);
        w.u32(c.b);
    }
    w.u32(static_cast<uint32_t>(f.lines.size()));
    for (const auto& l : f.lines) {
        w.u32(l.first);
        w.u32(l.second);
    }
    uint8_t flags = 0;
    if (f.isBuiltin) flags |= kFlagBuiltin;
    if (f.isTopLevel) flags |= kFlagTopLevel;
    if (f.isAsync) flags |= kFlagAsync;
    if (f.isExtern) flags |= kFlagExtern;
    w.u8(flags);
    w.str(f.builtinName);
    if (f.isExtern) {
        w.str(f.externDll);
        w.u8(static_cast<uint8_t>(f.externRet));
    }
}

bool decodeFunc(Reader& r, FuncBC& f) {
    if (!r.str(f.name) || !r.str(f.module)) return false;
    if (!r.u32(f.paramCount) || !r.u32(f.requiredArgs)) return false;

    uint32_t nd = 0;
    if (!r.u32(nd)) return false;
    if (nd > r.remaining()) {
        r.ok = false;
        return false;
    }
    f.defaults.clear();
    for (; nd > 0; --nd) {
        Value d;
        if (!decodeValueRec(r, d)) return false;
        f.defaults.push_back(std::move(d));
    }

    if (!r.u32(f.nlocals)) return false;

    uint32_t nc = 0;
    if (!r.u32(nc)) return false;
    if (nc > r.remaining() / 5) { // 每条指令至少 5 字节
        r.ok = false;
        return false;
    }
    f.code.resize(nc);
    for (auto& c : f.code) {
        uint8_t op = 0;
        uint32_t a = 0;
        uint32_t b = 0;
        if (!r.u8(op) || !r.u32(a) || !r.u32(b)) return false;
        if (op >= kOpCount) {
            r.ok = false;
            return false;
        }
        c.op = static_cast<Op>(op);
        c.a = a;
        c.b = b;
    }

    uint32_t nl = 0;
    if (!r.u32(nl)) return false;
    if (nl > r.remaining() / 8) { // 每条行映射至少 8 字节
        r.ok = false;
        return false;
    }
    f.lines.resize(nl);
    for (auto& l : f.lines)
        if (!r.u32(l.first) || !r.u32(l.second)) return false;

    uint8_t flags = 0;
    if (!r.u8(flags)) return false;
    f.isBuiltin = (flags & kFlagBuiltin) != 0;
    f.isTopLevel = (flags & kFlagTopLevel) != 0;
    f.isAsync = (flags & kFlagAsync) != 0;
    if (!r.str(f.builtinName)) return false;
    if (!(flags & kFlagExtern)) return true;
    f.isExtern = true;
    uint8_t ret = 0;
    if (!r.str(f.externDll) || !r.u8(ret)) return false;
    f.externRet = static_cast<char>(ret);
    return true;
}

} // namespace

// ----------------------------------------------------------------------------
// 入口 API
// ----------------------------------------------------------------------------

uint64_t fnv1a64(const void* data, size_t len, uint64_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

std::string encodeProgram(const Program& p, const CacheMeta& meta) {
    Writer w;
    w.raw(kMagic, sizeof kMagic);
    w.u32(kFormatVersion);
    w.str(meta.toolchain);
    w.str(meta.entry);
    w.u64(meta.sourceHash);

    w.u32(static_cast<uint32_t>(meta.modules.size()));
    for (const auto& s : meta.modules) w.str(s);
    w.u32(static_cast<uint32_t>(meta.paths.size()));
    for (const auto& s : meta.paths) w.str(s);

    w.u32(static_cast<uint32_t>(p.constants.size()));
    for (const auto& c : p.constants) encodeValueRec(w, c);

    w.u32(static_cast<uint32_t>(p.globalNames.size()));
    for (const auto& g : p.globalNames) {
        w.str(g.first);
        w.str(g.second);
    }

    w.u32(static_cast<uint32_t>(p.funcs.size()));
    for (const auto& f : p.funcs) encodeFunc(w, f);
    return std::move(w.b);
}

bool decodeProgram(const std::string& bytes, Program& p, CacheMeta& meta) {
    Reader r{bytes};
    char magic[4] = {};
    if (!r.raw(magic, sizeof magic)) return false;
    if (std::memcmp(magic, kMagic, sizeof magic) != 0) return false;
    uint32_t ver = 0;
    if (!r.u32(ver) || ver != kFormatVersion) return false;

    meta = CacheMeta{};
    p = Program{};
    if (!r.str(meta.toolchain) || !r.str(meta.entry) || !r.u64(meta.sourceHash))
        return false;

    uint32_t n = 0;
    if (!r.u32(n)) return false;
    for (; n > 0; --n) {
        std::string s;
        if (!r.str(s)) return false;
        meta.modules.push_back(std::move(s));
    }
    if (!r.u32(n)) return false;
    for (; n > 0; --n) {
        std::string s;
        if (!r.str(s)) return false;
        meta.paths.push_back(std::move(s));
    }

    if (!r.u32(n)) return false;
    if (n > r.remaining()) return false; // 防计数虚高
    for (; n > 0; --n) {
        Value c;
        if (!decodeValueRec(r, c)) return false;
        p.constants.push_back(std::move(c));
    }
    if (!r.u32(n)) return false;
    if (n > r.remaining()) return false;
    for (; n > 0; --n) {
        std::pair<std::string, std::string> g;
        if (!r.str(g.first) || !r.str(g.second)) return false;
        p.globalNames.push_back(std::move(g));
    }
    if (!r.u32(n)) return false;
    if (n > r.remaining()) return false;
    for (; n > 0; --n) {
        FuncBC f;
        if (!decodeFunc(r, f)) return false;
        p.funcs.push_back(std::move(f));
    }

    return r.ok && r.p == bytes.size(); // 必须精确消费（拒尾随垃圾）
}

} // namespace bc
} // namespace scourge
