// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 字节码 VM — 标准库广度内建（M 专项：统计/倒序/哈希）
// ----------------------------------------------------------------------------
//  职责边界：只实现 mean/var/std/reversed/sha256/crc32 六内建；doBuiltin
//  白名单分派进来（VM.cpp 尾段一行路由，本文件是唯一实现处）。
//  语义契约（与 src/Tool/BuilderRTStd.hpp 的 sg_* 逐字一致）：
//    * mean/var/std 接受张量与数值列表（int/float 元素），恒返回 f64；
//      空输入 panic；方差为总体方差（÷N）；NaN 按 IEEE 传播不拦截；
//      求和顺序固定为左→右单遍（双模 IEEE 逐位一致的依据）。
//    * reversed：列表元素倒序新列表；str 按字节倒序（非 Unicode 码点感知）。
//    * sha256(str) → 64 位小写 hex（FIPS 180-4，公有领域参考实现）；
//      crc32(str) → CRC-32/ISO-HDLC（IEEE 802.3，zlib 同款参数），恒非负。
// ============================================================================
#include "scourge/BC/VM.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace scourge {
namespace bc {

namespace {

/// SHA-256（FIPS 180-4；与 BuilderRTStd.hpp 的 sg_sha256 独立实现，
/// 规范决定结果，NIST 测试向量对拍锁定双模一致）。
struct Sha256 {
    uint32_t st[8];
    uint64_t bits;
    uint8_t buf[64];
    size_t buflen;
};

uint32_t shaRotr(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }

void shaBlock(Sha256& c, const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
        0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
        0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
        0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351,
        0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1,
        0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624,
        0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c,
        0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa,
        0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int k = 0; k < 16; ++k)
        w[k] = (uint32_t(p[k * 4]) << 24) | (uint32_t(p[k * 4 + 1]) << 16) |
               (uint32_t(p[k * 4 + 2]) << 8) | uint32_t(p[k * 4 + 3]);
    for (int k = 16; k < 64; ++k) {
        uint32_t s0 = shaRotr(w[k - 15], 7) ^ shaRotr(w[k - 15], 18) ^
                      (w[k - 15] >> 3);
        uint32_t s1 =
            shaRotr(w[k - 2], 17) ^ shaRotr(w[k - 2], 19) ^ (w[k - 2] >> 10);
        w[k] = w[k - 16] + s0 + w[k - 7] + s1;
    }
    uint32_t a = c.st[0], b = c.st[1], cc = c.st[2], d = c.st[3];
    uint32_t e = c.st[4], f = c.st[5], g = c.st[6], h = c.st[7];
    for (int k = 0; k < 64; ++k) {
        uint32_t S1 = shaRotr(e, 6) ^ shaRotr(e, 11) ^ shaRotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[k] + w[k];
        uint32_t S0 = shaRotr(a, 2) ^ shaRotr(a, 13) ^ shaRotr(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.st[0] += a; c.st[1] += b; c.st[2] += cc; c.st[3] += d;
    c.st[4] += e; c.st[5] += f; c.st[6] += g; c.st[7] += h;
}

std::string sha256Hex(const std::string& s) {
    Sha256 c;
    c.st[0] = 0x6a09e667; c.st[1] = 0xbb67ae85;
    c.st[2] = 0x3c6ef372; c.st[3] = 0xa54ff53a;
    c.st[4] = 0x510e527f; c.st[5] = 0x9b05688c;
    c.st[6] = 0x1f83d9ab; c.st[7] = 0x5be0cd19;
    c.bits = 0;
    c.buflen = 0;
    auto update = [&](const uint8_t* p, size_t n) {
        c.bits += uint64_t(n) * 8;
        while (n > 0) {
            size_t take = 64 - c.buflen;
            if (take > n) take = n;
            memcpy(c.buf + c.buflen, p, take);
            c.buflen += take;
            p += take;
            n -= take;
            if (c.buflen == 64) {
                shaBlock(c, c.buf);
                c.buflen = 0;
            }
        }
    };
    update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    uint64_t bitlen = c.bits;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t z = 0;
    while (c.buflen != 56) update(&z, 1);
    uint8_t lenb[8];
    for (int k = 0; k < 8; ++k)
        lenb[k] = uint8_t(bitlen >> (56 - k * 8));
    memcpy(c.buf + 56, lenb, 8);
    shaBlock(c, c.buf);
    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int k = 0; k < 8; ++k)
        for (int b = 3; b >= 0; --b) {
            uint8_t byte = uint8_t(c.st[k] >> (b * 8));
            out.push_back(hexd[byte >> 4]);
            out.push_back(hexd[byte & 15]);
        }
    return out;
}

/// CRC-32/ISO-HDLC（IEEE 802.3；zlib/gzip 同款参数）。与 BuilderRTStd.hpp
/// 的 sg_crc32 独立实现，规范决定结果，"123456789"→0xCBF43926 向量锁定。
uint32_t crc32Of(const std::string& s) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t k = 0; k < 256; ++k) {
            uint32_t c = k;
            for (int b = 0; b < 8; ++b)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
            table[k] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t ch : s) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/// 数值序列统一求值（两遍：mean → 偏差平方和；左→右单遍序，双模一致）。
/// mode: 0=mean 1=var 2=std。两向量恰一者非空（int 序列或 float 序列）。
double seqAvg(const std::vector<int64_t>* iv, const std::vector<double>* fv,
              int mode) {
    size_t n = iv ? iv->size() : fv->size();
    double sum = 0.0;
    if (iv) {
        for (int64_t x : *iv) sum += double(x);
    } else {
        for (double x : *fv) sum += x;
    }
    double m = sum / double(n);
    if (mode == 0) return m;
    double acc = 0.0;
    if (iv) {
        for (int64_t x : *iv) {
            double d = double(x) - m;
            acc += d * d;
        }
    } else {
        for (double x : *fv) {
            double d = x - m;
            acc += d * d;
        }
    }
    double v = acc / double(n); // 总体方差（÷N）
    if (mode == 1) return v;
    return std::sqrt(v);
}

} // namespace

void VM::stdBuiltin(const std::string& n, std::vector<Value>& a) {
    // ---- mean/var/std：张量与数值列表，恒返回 f64 ----
    if (n == "mean" || n == "var" || n == "std") {
        int mode = n == "mean" ? 0 : (n == "var" ? 1 : 2);
        if (a.size() != 1) panicHere(n + "() 恰好需要 1 个参数");
        const Value& v = a[0];
        if (v.kind == Value::Kind::Tensor && v.tensor) {
            const TensorData& t = *v.tensor;
            if (t.size() == 0) panicHere(n + "() 不支持空序列");
            double r = seqAvg(t.isInt ? &t.i64 : nullptr,
                              t.isInt ? nullptr : &t.f64, mode);
            push(Value::makeFloat(r));
            return;
        }
        if (v.kind == Value::Kind::List && v.list) {
            if (v.list->empty()) panicHere(n + "() 不支持空序列");
            // 元素必须全数值（int/float 混合提升 double；str/bool 拒绝）
            bool anyF = false;
            for (const Value& e : *v.list) {
                if (e.kind == Value::Kind::Float) anyF = true;
                else if (e.kind != Value::Kind::Int)
                    panicHere(n + "() 仅支持数值列表（int/float 元素）");
            }
            double r;
            if (anyF) {
                std::vector<double> xs;
                xs.reserve(v.list->size());
                for (const Value& e : *v.list)
                    xs.push_back(e.kind == Value::Kind::Float ? e.f
                                                              : double(e.i));
                r = seqAvg(nullptr, &xs, mode);
            } else {
                std::vector<int64_t> xs;
                xs.reserve(v.list->size());
                for (const Value& e : *v.list) xs.push_back(e.i);
                r = seqAvg(&xs, nullptr, mode);
            }
            push(Value::makeFloat(r));
            return;
        }
        panicHere(n + "() 仅支持张量或数值列表");
    }
    // ---- reversed：List → 倒序新列表；str → 字节倒序 ----
    if (n == "reversed") {
        if (a.size() != 1) panicHere("reversed() 恰好需要 1 个参数");
        const Value& v = a[0];
        if (v.kind == Value::Kind::List && v.list) {
            auto out = std::make_shared<std::vector<Value>>();
            out->reserve(v.list->size());
            for (auto it = v.list->rbegin(); it != v.list->rend(); ++it)
                out->push_back(*it);
            Value r = v; // 保留其他槽位（未来扩展）
            r.list = std::move(out);
            push(std::move(r));
            return;
        }
        if (v.kind == Value::Kind::Str) {
            std::string out(v.s.rbegin(), v.s.rend());
            push(Value::makeStr(std::move(out)));
            return;
        }
        panicHere("reversed() 仅支持列表或字符串");
    }
    // ---- sha256：str → 64 位小写 hex ----
    if (n == "sha256") {
        if (a.size() != 1) panicHere("sha256() 恰好需要 1 个参数");
        if (a[0].kind != Value::Kind::Str)
            panicHere("sha256() 参数必须是 str");
        push(Value::makeStr(sha256Hex(a[0].s)));
        return;
    }
    // ---- crc32：str → 非负 int ----
    if (n == "crc32") {
        if (a.size() != 1) panicHere("crc32() 恰好需要 1 个参数");
        if (a[0].kind != Value::Kind::Str)
            panicHere("crc32() 参数必须是 str");
        push(Value::makeInt(int64_t(crc32Of(a[0].s))));
        return;
    }
    panicHere("VM 内部错误：stdBuiltin 未处理 '" + n + "'");
}

} // namespace bc
} // namespace scourge
