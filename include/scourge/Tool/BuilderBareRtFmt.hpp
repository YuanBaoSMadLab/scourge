// Scourge bare runtime — fmt() segment (kBareRtFmtC)
// Appended in BuilderBare.cpp after str segment, before str-tail segment.

namespace scourge {
namespace bare {

// 原始字符串内容必须与拆出的原段逐字节一致，故不以段注释开头。
inline const char kBareRtFmtC[] = R"BRF(/* ==== fmt() 格式化（f-string :spec 降糖） ==== */
typedef struct {
    char fill;
    char align;
    int plus;
    int zero;
    int width;
    int prec;
    char type;
} SgFmtSpec;

static SgFmtSpec sg_fmtspec_parse(const char* s) {
    SgFmtSpec f;
    f.fill = ' ';
    f.align = 0;
    f.plus = 0;
    f.zero = 0;
    f.width = 0;
    f.prec = -1;
    f.type = 0;
    unsigned long long i = 0, n = strlen(s);
    if (i < n && (s[i] == '<' || s[i] == '>' || s[i] == '^')) {
        f.align = s[i++];
    } else if (n - i >= 2 &&
               (s[i + 1] == '<' || s[i + 1] == '>' || s[i + 1] == '^')) {
        f.fill = s[i];
        f.align = s[i + 1];
        i += 2;
    }
    if (i < n && s[i] == '+') {
        f.plus = 1;
        ++i;
    }
    if (i < n && s[i] == '0' && i + 1 < n && s[i + 1] >= '0' &&
        s[i + 1] <= '9') {
        f.zero = 1;
        ++i;
    }
    /* width/prec 上限 1000000/100（与 Sema 同值；饱和累加防篡改输入溢出）。 */
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        int d = s[i++] - '0';
        f.width = f.width > (1000000 - d) / 10 ? 1000000 : f.width * 10 + d;
    }
    if (i < n && s[i] == '.') {
        f.prec = 0;
        ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            int d = s[i++] - '0';
            f.prec = f.prec > (100 - d) / 10 ? 100 : f.prec * 10 + d;
        }
    }
    if (i < n) f.type = s[i];
    return f;
}

/* UTF-8 码点数（续字节 10xxxxxx 不计）。 */
static unsigned long long sg_cp_count(const char* s) {
    unsigned long long n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p)
        if ((*p & 0xC0) != 0x80) ++n;
    return n;
}

/* 截断到前 prec 个码点的字节长度。 */
static unsigned long long sg_cp_trunc_bytes(const char* s,
                                            unsigned long long prec) {
    unsigned long long n = 0;
    for (unsigned long long k = 0; s[k]; ++k) {
        if (((unsigned char)s[k] & 0xC0) != 0x80) {
            if (n == prec) return k;
            ++n;
        }
    }
    return strlen(s);
}

/* 码点计宽填充：'>' 左填充 / '<' 右填充 / '^' 居中（余数偏右）。 */
static char* sg_cp_pad(const char* body, SgFmtSpec f, char defAlign) {
    char align = f.align ? f.align : defAlign;
    unsigned long long cps = sg_cp_count(body);
    unsigned long long pad =
        f.width > (int)cps ? (unsigned long long)f.width - cps : 0;
    unsigned long long bl = strlen(body);
    char* out = (char*)sg_balloc(bl + pad + 1);
    char* w = out;
    if (align == '<') {
        for (unsigned long long i = 0; i < bl; ++i) *w++ = body[i];
        for (unsigned long long i = 0; i < pad; ++i) *w++ = f.fill;
    } else if (align == '^') {
        unsigned long long left = pad / 2;
        for (unsigned long long i = 0; i < left; ++i) *w++ = f.fill;
        for (unsigned long long i = 0; i < bl; ++i) *w++ = body[i];
        for (unsigned long long i = 0; i < pad - left; ++i) *w++ = f.fill;
    } else {
        for (unsigned long long i = 0; i < pad; ++i) *w++ = f.fill;
        for (unsigned long long i = 0; i < bl; ++i) *w++ = body[i];
    }
    *w = 0;
    return out;
}

/* 数值主体统一处理：符号剥离 → 零填充（旗标时插符号后）→ 宽度填充。 */
static char* sg_fmt_signed(const char* num, int neg, SgFmtSpec f) {
    const char* sign = neg ? "-" : (f.plus ? "+" : "");
    unsigned long long nl = strlen(num), sl = strlen(sign);
    if (f.zero && !f.align) {
        unsigned long long body = sl + nl;
        unsigned long long zeros =
            f.width > (int)body ? (unsigned long long)f.width - body : 0;
        char* out = (char*)sg_balloc(sl + zeros + nl + 1);
        char* w = out;
        for (unsigned long long i = 0; i < sl; ++i) *w++ = sign[i];
        for (unsigned long long i = 0; i < zeros; ++i) *w++ = '0';
        for (unsigned long long i = 0; i < nl; ++i) *w++ = num[i];
        *w = 0;
        return out;
    }
    char* joined = (char*)sg_balloc(sl + nl + 1);
    char* w = joined;
    for (unsigned long long i = 0; i < sl; ++i) *w++ = sign[i];
    for (unsigned long long i = 0; i < nl; ++i) *w++ = num[i];
    *w = 0;
    char* out = sg_cp_pad(joined, f, '>');
    return out;
}

const char* sg_format_i64(int64_t v, const char* spec) {
    SgFmtSpec f = sg_fmtspec_parse(spec);
    char buf[32];
    sg_i64_str((long long)v, buf);
    return sg_fmt_signed(buf + (buf[0] == '-' ? 1 : 0), v < 0, f);
}

/* %.Nf 精确定点：X = round_he(|v|×10^N)（大数引擎），插小数点后拼装。
 * %e：P=prec+1 位有效数字科学计数；%g：P=prec 位（同 sg_fmt_f64 判据）；
 * %%：×100 后定点。 */
static char* sg_f64_fixed(double v, int N) { /* |v| 的 N 位小数（半到偶） */
    union { double d; uint64_t u; } ua;
    ua.d = sg_abs_f64(v);
    int rawExp = (int)((ua.u >> 52) & 0x7ff) - 1023;
    uint64_t frac = ua.u & 0x000fffffffffffffull;
    uint64_t m;
    int e;
    if (rawExp == -1023) {
        m = frac;
        e = -1074;
    } else {
        m = frac | (1ull << 52);
        e = rawExp - 52;
    }
    SgBn bn;
    sg_bn_from_u64(&bn, m);
    int scale;
    if (e >= 0) {
        for (int i = 0; i < e; ++i) sg_bn_mul_small(&bn, 2);
        scale = 0;
    } else {
        for (int i = 0; i < -e; ++i) sg_bn_mul_small(&bn, 5);
        scale = e;
    }
    if (bn.n == 0) {
        char* z = (char*)sg_balloc(2);
        z[0] = '0';
        z[1] = 0;
        return z;
    }
    int D = sg_bn_digits(&bn);
    /* X = round_he(bn × 10^(N-scale))；N-scale = N-e（e<0）或 N（e≥0）*/
    int shift = N - (e < 0 ? e : 0);
    SgBn q;
    q = bn;
    uint64_t X;
    if (shift >= 0) {
        if (shift > 0) sg_bn_shl10(&q, shift);
    } else {
        SgBn r, r2, div;
        sg_bn_shr10(&q, -shift, &r);
        sg_bn_mul_small(&r, 2);
        sg_bn_from_u64(&div, 1);
        sg_bn_shl10(&div, -shift);
        int cmp = sg_bn_cmp(&r, &div);
        int up = cmp > 0;
        if (cmp == 0) {
            X = 0;
            for (int i = q.n - 1; i >= 0; --i) X = X * 1000000000ull + q.d[i];
            up = (X & 1) != 0;
        }
        if (up) {
            /* 商 +1（大数 +1） */
            int i = 0;
            for (; i < q.n; ++i) {
                if (++q.d[i] < 1000000000u) break;
                q.d[i] = 0;
            }
            if (i == q.n && q.n < SG_BN_LIMBS) q.d[q.n++] = 1;
        }
    }
    X = 0;
    for (int i = q.n - 1; i >= 0; --i) X = X * 1000000000ull + q.d[i];
    /* X 数字串（可能 > u64？value×10^N ≤ 1.8e308×1e40 → 超 u64！
     * X 是大数十进制串，不能塞 u64——从 q 直接生成数字串。 */
    static char big[SG_BN_LIMBS * 9 + 2];
    int nd = 0;
    {
        char tmp[SG_BN_LIMBS * 9 + 2];
        int ti = SG_BN_LIMBS * 9 + 1;
        for (int i = 0; i < q.n; ++i) {
            unsigned int limb = q.d[i];
            for (int j = 0; j < 9; ++j) {
                tmp[--ti] = (char)('0' + limb % 10);
                limb /= 10;
            }
        }
        /* 去前导零（保留至少 1 位） */
        while (ti < SG_BN_LIMBS * 9 + 1 && tmp[ti] == '0') ++ti;
        if (ti > SG_BN_LIMBS * 9 + 1) ti = SG_BN_LIMBS * 9 + 1;
        while (ti <= SG_BN_LIMBS * 9 + 1) big[nd++] = tmp[ti++];
        big[nd] = 0;
    }
    /* 插小数点：X 有 nd 位，小数点在 nd-N 位后 */
    static char out[SG_BN_LIMBS * 9 + 8];
    int w = 0;
    if (N == 0) {
        for (int i = 0; i < nd; ++i) out[w++] = big[i];
    } else if (nd > N) {
        for (int i = 0; i < nd - N; ++i) out[w++] = big[i];
        out[w++] = '.';
        for (int i = nd - N; i < nd; ++i) out[w++] = big[i];
    } else {
        out[w++] = '0';
        out[w++] = '.';
        for (int i = 0; i < N - nd; ++i) out[w++] = '0';
        for (int i = 0; i < nd; ++i) out[w++] = big[i];
    }
    out[w] = 0;
    return out;
}

const char* sg_format_f64(double v, const char* spec) {
    SgFmtSpec f = sg_fmtspec_parse(spec);
    int prec = f.prec >= 0 ? f.prec : 6;
    char type = f.type ? f.type : 'f';
    static char body[1200];
    int neg = v < 0 || (v == 0.0 && 1.0 / v < 0);
    double a = sg_abs_f64(v);
    const char* src = body;
    if (type == '%') {
        char* t = sg_f64_fixed(a * 100.0, prec);
        char* w = body;
        while (*t) *w++ = *t++;
        *w++ = '%';
        *w = 0;
    } else if (type == 'f' || type == 'F') {
        char* t = sg_f64_fixed(a, prec);
        char* w = body;
        while (*t) *w++ = *t++;
        *w = 0;
    } else if (type == 'e' || type == 'E') {
        /* P = prec+1 位有效数字科学计数 */
        if (a == 0.0) {
            char* w = body;
            *w++ = '0';
            if (prec > 0) {
                *w++ = '.';
                for (int i = 0; i < prec; ++i) *w++ = '0';
            }
            *w++ = (char)type;
            *w++ = '+';
            *w++ = '0';
            *w++ = '0';
            *w = 0;
        } else {
            uint64_t X;
            int E;
            sg_f64_round_sig(a, prec + 1, &X, &E);
            char digits[24];
            int nd = 0;
            {
                uint64_t t = X;
                char tmp[24];
                int i = 24;
                while (t) {
                    tmp[--i] = (char)('0' + (t % 10));
                    t /= 10;
                }
                while (i < 24) digits[nd++] = tmp[i++];
            }
            char* w = body;
            *w++ = digits[0];
            if (prec > 0) {
                *w++ = '.';
                for (int i = 1; i < nd; ++i) *w++ = digits[i];
                for (int i = nd; i <= prec; ++i) *w++ = '0';
            }
            *w++ = (char)type;
            *w++ = E < 0 ? '-' : '+';
            int ae = E < 0 ? -E : E;
            *w++ = (char)('0' + (ae / 10) % 10);
            *w++ = (char)('0' + ae % 10);
            *w = 0;
        }
    } else { /* g/G：P = prec（prec=0 → 1），宿主 %g 语义（无 # 旗标） */
        int P = prec == 0 ? 1 : prec;
        if (a == 0.0) {
            body[0] = '0';
            body[1] = 0;
        } else {
            uint64_t X;
            int E;
            /* 取最小的 P' ∈ [P,17] 使 round-trip（%g 的去尾零由下方
             * 统一处理；宿主 %.Pg 用 P 位——这里直接用 P 位舍入） */
            sg_f64_round_sig(a, P, &X, &E);
            char digits[24];
            int nd = 0;
            {
                uint64_t t = X;
                char tmp[24];
                int i = 24;
                while (t) {
                    tmp[--i] = (char)('0' + (t % 10));
                    t /= 10;
                }
                while (i < 24) digits[nd++] = tmp[i++];
            }
            char* w = body;
            if (E < -4 || E >= P) {
                *w++ = digits[0];
                int last = nd;
                while (last > 1 && digits[last - 1] == '0') --last;
                if (last > 1) {
                    *w++ = '.';
                    for (int i = 1; i < last; ++i) *w++ = digits[i];
                }
                *w++ = (char)type;
                *w++ = E < 0 ? '-' : '+';
                int ae = E < 0 ? -E : E;
                *w++ = (char)('0' + (ae / 10) % 10);
                *w++ = (char)('0' + ae % 10);
            } else if (E >= 0) {
                int intLen = E + 1;
                for (int i = 0; i < intLen && i < nd; ++i) *w++ = digits[i];
                for (int i = nd; i < intLen; ++i) *w++ = '0';
                int last = nd > intLen ? nd : intLen;
                (void)last;
                /* 小数部分：nd > intLen 时存在 */
                int e2 = nd;
                while (e2 > intLen && digits[e2 - 1] == '0') --e2;
                if (e2 > intLen) {
                    *w++ = '.';
                    for (int i = intLen; i < e2; ++i) *w++ = digits[i];
                }
            } else {
                *w++ = '0';
                *w++ = '.';
                for (int i = 0; i < -E - 1; ++i) *w++ = '0';
                int e2 = nd;
                while (e2 > 1 && digits[e2 - 1] == '0') --e2;
                for (int i = 0; i < e2; ++i) *w++ = digits[i];
            }
            *w = 0;
        }
    }
    (void)src;
    return sg_fmt_signed(body + (body[0] == '-' ? 1 : 0), neg, f);
}

const char* sg_format_str(const char* v, const char* spec) {
    SgFmtSpec f = sg_fmtspec_parse(spec);
    char* body;
    if (f.prec >= 0) {
        unsigned long long nb = sg_cp_trunc_bytes(v, (unsigned long long)f.prec);
        body = sg_bdupn(v, nb);
    } else {
        body = (char*)v;
    }
    return sg_cp_pad(body, f, '<');
}

const char* sg_format_bool(int v, const char* spec) {
    return sg_format_str(v ? "True" : "False", spec);
}
)BRF";

} // namespace bare
} // namespace scourge
