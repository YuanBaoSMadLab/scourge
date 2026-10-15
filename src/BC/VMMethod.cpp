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
        if (self.kind != Value::Kind::List || !self.list)
            panicHere("count() 只能用于列表");
        if (args.size() != 1) panicHere("count() 恰好需要 1 个参数");
        int64_t n = 0;
        for (const Value& e : *self.list)
            if (listElemEq(e, args[0])) ++n;
        push(Value::makeInt(n));
        return;
    }
    default:
        panicHere("VM 内部错误：未知方法");
    }
}

} // namespace bc
} // namespace scourge
