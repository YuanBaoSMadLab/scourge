// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 字节码 VM — str / List 成员方法执行（CallMethod 分派尾段）
// ----------------------------------------------------------------------------
//  职责边界：只实现 Method 枚举中 0.9.4 追加的 str/List 成员方法；Result
//  三方法仍在 VM.cpp 的 CallMethod 直接分支（历史路径不动）。
//  语义契约（与 src/Tool/BuilderRTStr.hpp 的 sg_str_* / sg_list_* 逐字一致）：
//    * panic 消息逐字相同，双模差分由 verify 探针锁定；
//    * upper/lower 仅 ASCII 字母映射，其余字节原样；
//    * trim 家族空白集 = 空格 \t \r \n \f \v（与 C isspace 默认一致）；
//    * find 未找到返回 -1；index 未找到 panic；split/join/replace 见各分支；
//    * List 成员比较规则 = 'in' 运算符（valuesEqual）：同类精确比较，
//      int 键遇 float 元素提升 f64（唯一放宽），其余不匹配（检查器拦截）。
// ============================================================================
#include "scourge/BC/VM.hpp"
#include "scourge/BC/VMDetail.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace scourge {
namespace bc {

namespace {

inline bool isSpaceByte(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

/// List 成员相等（与 Bin::In 的 valuesEqual 同规则；见文件头契约）。
inline bool listElemEq(const Value& e, const Value& key) {
    if (e.kind == Value::Kind::Int && key.kind == Value::Kind::Int)
        return e.i == key.i;
    if (e.kind == Value::Kind::Float && key.kind == Value::Kind::Float)
        return e.f == key.f;
    if (e.kind == Value::Kind::Float && key.kind == Value::Kind::Int)
        return e.f == static_cast<double>(key.i);
    if (e.kind == Value::Kind::Bool && key.kind == Value::Kind::Bool)
        return e.b == key.b;
    if (e.kind == Value::Kind::Str && key.kind == Value::Kind::Str)
        return e.s == key.s;
    return false;
}

/// trim 家族共用：left/right 分别控制两端。
std::string trimImpl(const std::string& s, bool left, bool right) {
    size_t b = 0, e = s.size();
    if (left)
        while (b < e && isSpaceByte(s[b])) ++b;
    if (right)
        while (e > b && isSpaceByte(s[e - 1])) --e;
    return s.substr(b, e - b);
}

/// Python 语义 split：无匹配 → 单元素整串；分隔符邻接 → 空串元素。
std::vector<std::string> splitImpl(const std::string& s,
                                   const std::string& sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t p = s.find(sep); p != std::string::npos; p = s.find(sep, start)) {
        out.push_back(s.substr(start, p - start));
        start = p + sep.size();
    }
    out.push_back(s.substr(start));
    return out;
}

/// 全部替换（调用方保证 old 非空，替换次数有界）。
std::string replaceImpl(const std::string& s, const std::string& oldS,
                        const std::string& newS) {
    std::string out;
    size_t start = 0;
    for (size_t p = s.find(oldS); p != std::string::npos;
         p = s.find(oldS, start)) {
        out.append(s, start, p - start);
        out += newS;
        start = p + oldS.size();
    }
    out.append(s, start, std::string::npos);
    return out;
}

std::string asciiMap(const std::string& s, bool toUpper) {
    std::string out = s;
    for (char& c : out) {
        if (toUpper && c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        if (!toUpper && c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return out;
}

} // namespace

// ----------------------------------------------------------------------------
// 方法分派：self 类型运行时守卫 + 纯函数实现
// ----------------------------------------------------------------------------
void VM::callValueMethod(Method m, const Value& self,
                         const std::vector<Value>& args) {
    // ---- Json 家族（M 专项）：路由到 VMJson.cpp 分块 ----
    if (self.kind == Value::Kind::Json) {
        jsonMethod(m, self, args);
        return;
    }
    // ---- str 家族：self 必须是字符串（检查器静态保证，此处兜底） ----
    auto needStr = [&](const char* what) {
        if (self.kind != Value::Kind::Str)
            panicHere(std::string(what) + "() 只能用于 str");
    };

    switch (m) {
    case Method::StrUpper:
        needStr("upper");
        push(Value::makeStr(asciiMap(self.s, true)));
        return;
    case Method::StrLower:
        needStr("lower");
        push(Value::makeStr(asciiMap(self.s, false)));
        return;
    case Method::StrTrim:
        needStr("trim");
        push(Value::makeStr(trimImpl(self.s, true, true)));
        return;
    case Method::StrTrimStart:
        needStr("trim_start");
        push(Value::makeStr(trimImpl(self.s, true, false)));
        return;
    case Method::StrTrimEnd:
        needStr("trim_end");
        push(Value::makeStr(trimImpl(self.s, false, true)));
        return;
    case Method::StrSplit: {
        needStr("split");
        if (args.size() != 1 || args[0].kind != Value::Kind::Str)
            panicHere("split() 需要 1 个 str 参数");
        if (args[0].s.empty()) panicHere("split() 分隔符不能为空串");
        auto vec = std::make_shared<std::vector<Value>>();
        for (std::string& part : splitImpl(self.s, args[0].s))
            vec->push_back(Value::makeStr(std::move(part)));
        push(Value::makeList(std::move(vec)));
        return;
    }
    case Method::StrJoin: {
        needStr("join");
        if (args.size() != 1 || args[0].kind != Value::Kind::List || !args[0].list)
            panicHere("join() 需要 1 个 List[str] 参数");
        std::string out;
        for (size_t k = 0; k < args[0].list->size(); ++k) {
            const Value& e = (*args[0].list)[k];
            if (e.kind != Value::Kind::Str) panicHere("join() 列表元素必须是 str");
            if (k) out += self.s;
            out += e.s;
        }
        push(Value::makeStr(std::move(out)));
        return;
    }
    case Method::StrReplace: {
        needStr("replace");
        if (args.size() != 2 || args[0].kind != Value::Kind::Str ||
            args[1].kind != Value::Kind::Str)
            panicHere("replace() 需要 2 个 str 参数");
        if (args[0].s.empty()) panicHere("replace() 旧子串不能为空串");
        push(Value::makeStr(replaceImpl(self.s, args[0].s, args[1].s)));
        return;
    }
    case Method::StrFind: {
        needStr("find");
        if (args.size() != 1 || args[0].kind != Value::Kind::Str)
            panicHere("find() 需要 1 个 str 参数");
        size_t p = self.s.find(args[0].s);
        push(Value::makeInt(p == std::string::npos ? -1
                                                   : static_cast<int64_t>(p)));
        return;
    }
    case Method::StrSlice: {
        needStr("slice");
        if (args.size() != 2 || args[0].kind != Value::Kind::Int ||
            args[1].kind != Value::Kind::Int)
            panicHere("slice() 需要 2 个 int 参数");
        int64_t len = static_cast<int64_t>(self.s.size());
        int64_t a = args[0].i < 0 ? 0 : args[0].i;
        int64_t b = args[1].i > len ? len : args[1].i;
        if (a > b) a = b;
        push(Value::makeStr(self.s.substr(static_cast<size_t>(a),
                                          static_cast<size_t>(b - a))));
        return;
    }
    case Method::StrStartsWith: {
        needStr("startswith");
        if (args.size() != 1 || args[0].kind != Value::Kind::Str)
            panicHere("startswith() 需要 1 个 str 参数");
        push(Value::makeBool(self.s.rfind(args[0].s, 0) == 0));
        return;
    }
    case Method::StrEndsWith: {
        needStr("endswith");
        if (args.size() != 1 || args[0].kind != Value::Kind::Str)
            panicHere("endswith() 需要 1 个 str 参数");
        const std::string& suf = args[0].s;
        push(Value::makeBool(self.s.size() >= suf.size() &&
                             self.s.compare(self.s.size() - suf.size(),
                                            suf.size(), suf) == 0));
        return;
    }
    // ---- contains / index：str 与 List 同名，按 self.kind 分派 ----
    case Method::Contains: {
        if (self.kind == Value::Kind::Str) {
            if (args.size() != 1 || args[0].kind != Value::Kind::Str)
                panicHere("contains() 需要 1 个 str 参数");
            push(Value::makeBool(self.s.find(args[0].s) != std::string::npos));
            return;
        }
        if (self.kind == Value::Kind::List && self.list) {
            if (args.size() != 1) panicHere("contains() 恰好需要 1 个参数");
            for (const Value& e : *self.list)
                if (listElemEq(e, args[0])) {
                    push(Value::makeBool(true));
                    return;
                }
            push(Value::makeBool(false));
            return;
        }
        panicHere("contains() 只能用于 str 或列表");
    }
    case Method::Index: {
        if (self.kind == Value::Kind::Str) {
            if (args.size() != 1 || args[0].kind != Value::Kind::Str)
                panicHere("index() 需要 1 个 str 参数");
            size_t p = self.s.find(args[0].s);
            if (p == std::string::npos) panicHere("index() 未找到子串");
            push(Value::makeInt(static_cast<int64_t>(p)));
            return;
        }
        if (self.kind == Value::Kind::List && self.list) {
            if (args.size() != 1) panicHere("index() 恰好需要 1 个参数");
            for (size_t k = 0; k < self.list->size(); ++k)
                if (listElemEq((*self.list)[k], args[0])) {
                    push(Value::makeInt(static_cast<int64_t>(k)));
                    return;
                }
            panicHere("index() 未找到元素");
        }
        panicHere("index() 只能用于 str 或列表");
    }
    case Method::ListCount: {
        // 0.9.9：str 分支——子串非重叠计数（空子串 = len+1，与 Python
        // 一致）；消息与 AOT sg_str_count 逐字一致。
        if (self.kind == Value::Kind::Str) {
            if (args.size() != 1 || args[0].kind != Value::Kind::Str)
                panicHere("count() 需要 1 个 str 参数");
            if (args[0].s.empty()) {
                push(Value::makeInt(static_cast<int64_t>(self.s.size()) + 1));
                return;
            }
            int64_t n = 0;
            for (size_t p = self.s.find(args[0].s); p != std::string::npos;
                 p = self.s.find(args[0].s, p + args[0].s.size()))
                ++n;
            push(Value::makeInt(n));
            return;
        }
        if (self.kind != Value::Kind::List || !self.list)
            panicHere("count() 只能用于列表或字符串");
        if (args.size() != 1) panicHere("count() 恰好需要 1 个参数");
        int64_t n = 0;
        for (const Value& e : *self.list)
            if (listElemEq(e, args[0])) ++n;
        push(Value::makeInt(n));
        return;
    }
    case Method::ListAppend: {
        // 元素域 str/int/float/bool（0.9.8 扩展标量全域）：VM 列表为
        // vector<Value>，值直接入列；类型一致性与参数个数已由检查器
        // 静态保证，此处仅防御非法字节码。
        if (self.kind != Value::Kind::List || !self.list)
            panicHere("append() 只能用于列表");
        if (args.size() != 1)
            panicHere("append() 恰好需要 1 个参数");
        self.list->push_back(args[0]);
        push(Value::makeNone());
        return;
    }
    // ---- Dict 视图方法（0.9.9）：插入序，键值标量域（检查器限域） ----
    case Method::DictKeys: {
        if (self.kind != Value::Kind::Dict || !self.dict)
            panicHere("keys() 只能用于字典");
        auto out = std::make_shared<std::vector<Value>>();
        for (const auto& e : self.dict->entries) out->push_back(e.key);
        push(Value::makeList(std::move(out)));
        return;
    }
    case Method::DictValues: {
        if (self.kind != Value::Kind::Dict || !self.dict)
            panicHere("values() 只能用于字典");
        auto out = std::make_shared<std::vector<Value>>();
        for (const auto& e : self.dict->entries) out->push_back(e.val);
        push(Value::makeList(std::move(out)));
        return;
    }
    case Method::DictItems: {
        if (self.kind != Value::Kind::Dict || !self.dict)
            panicHere("items() 只能用于字典");
        auto out = std::make_shared<std::vector<Value>>();
        for (const auto& e : self.dict->entries) {
            auto pair = std::make_shared<std::vector<Value>>();
            pair->push_back(e.key);
            pair->push_back(e.val);
            out->push_back(Value::makeTuple(std::move(pair)));
        }
        push(Value::makeList(std::move(out)));
        return;
    }
    case Method::DictGet: {
        if (self.kind != Value::Kind::Dict || !self.dict)
            panicHere("get() 只能用于字典");
        if (args.size() != 2) panicHere("get() 恰好需要 2 个参数");
        // 缺键取缺省值不 panic（Result.unwrap_or 同哲学）；键相等 =
        // valuesEqual（数值跨类型同键，与 d[k] 同一事实源）
        for (const auto& e : self.dict->entries)
            if (valuesEqual(e.key, args[0])) {
                push(e.val);
                return;
            }
        push(args[1]);
        return;
    }
    case Method::SetAdd: {
        if (self.kind != Value::Kind::Set || !self.set)
            panicHere("add() 只能用于集合");
        if (args.size() != 1) panicHere("add() 恰好需要 1 个参数");
        // NaN 先查（与 NewSet/构造消息逐字一致；SetData::add 内不设防）
        if (args[0].kind == Value::Kind::Float && std::isnan(args[0].f))
            panicHere("集合元素不能为 NaN");
        self.set->add(args[0]);
        push(Value::makeNone());
        return;
    }
    case Method::Slice2: {
        // 切片 s[a:b]（0.9.9）：4 参 [start,end,hasA,hasB] 全 int（编译器
        // 保证；此处防御非法字节码）。归一公式与 AOT sg_normslice 逐字
        // 一致：负下标加 len 后钳 0，越界钳 len，b<a 收敛为空切片。
        if (args.size() != 4 || args[0].kind != Value::Kind::Int ||
            args[1].kind != Value::Kind::Int || args[2].kind != Value::Kind::Int ||
            args[3].kind != Value::Kind::Int)
            panicHere("slice 边界必须是 int");
        auto norm = [](int64_t a, int64_t b, int64_t hasA, int64_t hasB,
                       int64_t len) {
            if (!hasA)
                a = 0;
            else if (a < 0) {
                a += len;
                if (a < 0) a = 0;
            }
            if (!hasB)
                b = len;
            else if (b < 0) {
                b += len;
                if (b < 0) b = 0;
            }
            if (a > len) a = len;
            if (b > len) b = len;
            if (b < a) b = a;
            return std::pair<int64_t, int64_t>{a, b};
        };
        if (self.kind == Value::Kind::Str) {
            auto [a, b] = norm(args[0].i, args[1].i, args[2].i, args[3].i,
                               static_cast<int64_t>(self.s.size()));
            push(Value::makeStr(self.s.substr(static_cast<size_t>(a),
                                              static_cast<size_t>(b - a))));
            return;
        }
        if (self.kind == Value::Kind::List && self.list) {
            auto [a, b] = norm(args[0].i, args[1].i, args[2].i, args[3].i,
                               static_cast<int64_t>(self.list->size()));
            // 浅拷贝子列表：元素句柄共享语义与 VM 列表别名一致。
            auto out = std::make_shared<std::vector<Value>>(
                self.list->begin() + static_cast<std::ptrdiff_t>(a),
                self.list->begin() + static_cast<std::ptrdiff_t>(b));
            push(Value::makeList(std::move(out)));
            return;
        }
        panicHere("切片只能用于 str 或列表");
    }
    default:
        panicHere("VM 内部错误：未知方法");
    }
}

} // namespace bc
} // namespace scourge
