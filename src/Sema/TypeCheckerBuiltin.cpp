// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 类型检查器 — 内建调用规则
// ----------------------------------------------------------------------------
//  checkBuiltinCall：内建名 → 参数/返回类型规则；@pure 纯度白名单、
//  fmt() spec 静态校验（validateFmtSpec）同置此处。
// ============================================================================
#include "scourge/Sema/TypeChecker.hpp"

#include <cctype>
#include <cmath>
#include <functional>
#include <set>

namespace scourge {

// fmt() 格式说明符静态校验。
// 文法（Python 高频子集）：[[fill]align][+][0][width][.prec][type]，
// type ∈ d/f/e/g/s/%/空。width ≤ 1000000、prec ≤ 100（与执行侧同值）。
// 返回空串 = 合法；否则为原因短语（拼入 E1007 消息）。
static std::string validateFmtSpec(const std::string& s, Type::Kind k) {
    constexpr long long kMaxFmtWidth = 1000000;
    constexpr long long kMaxFmtPrec = 100;
    bool numeric = k == Type::Kind::Int || k == Type::Kind::Float;
    size_t i = 0;
    const size_t n = s.size();
    auto isAlign = [](char c) { return c == '<' || c == '>' || c == '^'; };
    if (i < n && isAlign(s[i])) {
        ++i;
    } else if (n - i >= 2 && isAlign(s[i + 1])) {
        i += 2; // fill + align
    }
    if (i < n && s[i] == '+') ++i;
    if (i < n && s[i] == '0' && i + 1 < n && std::isdigit(
        static_cast<unsigned char>(s[i + 1]))) {
        ++i; // 零填充旗标（后随宽度）；裸 "0" 属宽度 0
    }
    long long width = 0;
    while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) {
        width = width * 10 + (s[i++] - '0');
        if (width > kMaxFmtWidth) return "宽度上限为 1000000";
    }
    bool hadPrec = false;
    if (i < n && s[i] == '.') {
        ++i;
        if (i >= n || !std::isdigit(static_cast<unsigned char>(s[i])))
            return "'.prec' 后必须有精度位数";
        long long prec = 0;
        while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) {
            prec = prec * 10 + (s[i++] - '0');
            if (prec > kMaxFmtPrec) return "精度上限为 100";
        }
        hadPrec = true;
    }
    char type = 0;
    if (i < n) {
        type = s[i];
        ++i;
    }
    if (i != n) return "无法识别的尾部字符";
    if (type && type != 'd' && type != 'f' && type != 'e' && type != 'g' &&
        type != 's' && type != '%')
        return std::string("未知类型码 '") + type + "'";
    if (!numeric) { // str/bool：fill/align + width/.prec（执行侧 cpPad 全支持）
        if (type == '%') return "str/bool 不支持百分比类型";
        if (type && type != 's') return "str/bool 类型码仅 's'";
        return "";
    }
    if (k == Type::Kind::Int) {
        if (hadPrec) return "int 类型不支持精度（.prec）";
        if (type && type != 'd') return "int 类型码仅 'd'";
        return "";
    }
    // float：f/e/g/%/空
    if (type == 'd') return "float 类型码不支持 'd'（整数化请用 int()）";
    if (type && type != 'f' && type != 'e' && type != 'g' && type != '%')
        return "float 类型码仅 f/e/g/%";
    return "";
}
TypePtr TypeChecker::checkBuiltinCall(const std::string& bn, const Call& c) {
    const Expr& e = c; // 诊断区间用整个调用表达式
    // K 专项：@pure 函数内的内建纯度门。白名单制——数学/转换/排序类
    // 内建为纯；一切涉及 I/O、时间、随机、环境、网络、进程的内建（含
    // 后续并行线新增者，未列入白名单即非纯）在 @pure 函数体内拒绝。
    if (inPureFn_) {
        static const std::unordered_set<std::string> kPureBuiltins = {
            "str",   "len",  "sum",      "range", "ok",     "err",
            "some",  "none",
            "abs",   "sqrt", "exp",      "log",   "max",    "min",
            "transpose", "reshape",
            "sin",   "cos",  "tan",      "asin",  "acos",   "atan",
            "sinh",  "cosh", "tanh",     "atan2", "floor",  "ceil",
            "round", "trunc", "sorted",
            // 0.9.4 M 专项：统计/倒序/哈希（全部为纯函数）
            "mean", "var", "std", "median", "reversed", "sha256", "crc32",
            // format()/f-string：降糖为 str()+concat，纯字符串计算
            "format",
            // 0.9.4 M 专项：JSON 解码（纯函数，不触碰外部状态）
            "json_decode",
            // 0.9.9 类型转换/码点（纯函数）
            "int", "float", "ord", "chr",
            // fmt()：f-string :spec 降糖目标，纯字符串计算（spec 字面量）
            "fmt",
            // 0.9.6 O 节：地址导出为纯读取（解引用才是非纯边界）
            "data_ptr"};
        if (!kPureBuiltins.count(bn)) {
            return fail(Errc::E1007_UnsupportedFeature, e.range,
                        "@pure 函数内禁止调用非纯内建 '" + bn +
                            "'（纯度白名单：数学/转换/排序类）");
        }
    }
    if (bn == "print") {
        for (const auto& a : c.args) checkExpr(*a); // 张量执行已进入字节码（P1）
        return tyNone();
    }
    if (bn == "str") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "str() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        // 0.9.9：容器化（List/Dict/Set repr 与 print 逐字一致）
        if (t->kind == Type::Kind::Int || t->kind == Type::Kind::Float ||
            t->kind == Type::Kind::Bool || t->kind == Type::Kind::Str ||
            t->kind == Type::Kind::Tensor || t->kind == Type::Kind::Tuple ||
            t->kind == Type::Kind::List || t->kind == Type::Kind::Dict ||
            t->kind == Type::Kind::Set || t->kind == Type::Kind::Err)
            return tyStr();
        fail(Errc::E2001_TypeMismatch, c.args[0]->range,
             "str() 不支持 " + typeToString(t));
        return tyErr();
    }
    // 0.9.9 类型转换/码点：int/float 收 str/int/float/bool 全域（str 解析
    // 失败/越界运行时 panic，消息与 VM doBuiltin / AOT sg_* 逐字一致）；
    // ord(str)→int（单码点）、chr(int)→str（0..0x10FFFF）。
    if (bn == "int" || bn == "float" || bn == "ord" || bn == "chr") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (bn == "ord") {
            if (t->kind == Type::Kind::Str) return tyInt();
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "ord() 参数必须是 str，实际为 " + typeToString(t));
        }
        if (bn == "chr") {
            if (t->kind == Type::Kind::Int) return tyStr();
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "chr() 参数必须是 int，实际为 " + typeToString(t));
        }
        if (t->kind == Type::Kind::Str || t->kind == Type::Kind::Int ||
            t->kind == Type::Kind::Float || t->kind == Type::Kind::Bool)
            return bn == "int" ? tyInt() : tyFloat();
        return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                    bn + "() 不支持 " + typeToString(t));
    }
    // format()：合法形态（字面量首参）已在 Parser 降糖，能到达这里的都是
    // 非法形态——给出定向错误而非笼统的未知内建。
    if (bn == "format") {
        if (c.args.empty())
            return fail(Errc::E1007_UnsupportedFeature, e.range,
                        "format() 首参必须是字符串字面量（编译期展开）");
        return fail(Errc::E1007_UnsupportedFeature, c.args[0]->range,
                    "format() 首参必须是字符串字面量（编译期展开）");
    }
    // fmt(expr, "spec")（0.9.9）：f-string :spec 降糖目标，亦可显式调用
    // （spec 必须字面量——静态校验是唯一事实源，执行侧不设防）。按首参
    // 类型分派四层：Int→fmt_i64、Float→fmt_f64、Str→fmt_str、Bool→fmt_bool。
    if (bn == "fmt") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "fmt() 恰好需要 2 个参数（值, 格式说明符）");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Float &&
            t->kind != Type::Kind::Str && t->kind != Type::Kind::Bool)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "fmt() 不支持 " + typeToString(t) +
                            "（仅 int/float/str/bool；容器请先逐元素格式化）");
        const Expr& se = *c.args[1];
        if (se.kind != Expr::Kind::StrLit)
            return fail(Errc::E1007_UnsupportedFeature, se.range,
                        "fmt() 格式说明符必须是字符串字面量（f-string :spec "
                        "自动生成；静态校验需编译期可见）");
        const std::string& spec =
            static_cast<const StrLit&>(se).strValue;
        std::string why = validateFmtSpec(spec, t->kind);
        if (!why.empty())
            return fail(Errc::E1007_UnsupportedFeature, se.range,
                        "格式说明符 '" + spec + "' 非法：" + why +
                            "；示例 {x:.2f} / {x:>8} / {x:08.3f}");
        return tyStr();
    }
    if (bn == "len") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "len() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::List || t->kind == Type::Kind::Dict ||
            t->kind == Type::Kind::Set || t->kind == Type::Kind::Str ||
            t->kind == Type::Kind::Tuple || t->kind == Type::Kind::Err)
            return tyInt();
        fail(Errc::E2001_TypeMismatch, c.args[0]->range,
             "len() 不支持 " + typeToString(t));
        return tyErr();
    }
    if (bn == "sum") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "sum() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Tensor)
            return t->params[0]; // 归约结果 = 元素类型
        // 0.9.9：List 归约（bool 按 0/1 计入 int；str 列表拒绝）
        if (t->kind == Type::Kind::List && !t->params.empty()) {
            TypePtr elem = t->params[0];
            if (elem->kind == Type::Kind::Int || elem->kind == Type::Kind::Bool)
                return tyInt();
            if (elem->kind == Type::Kind::Float) return tyFloat();
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "sum() 仅支持数值列表归约，实际为 " + typeToString(t));
        }
        if (t->kind == Type::Kind::Err) return tyErr();
        fail(Errc::E2001_TypeMismatch, c.args[0]->range,
             "sum() 仅支持张量或数值列表归约，不支持 " + typeToString(t));
        return tyErr();
    }
    if (bn == "now") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "now() 不需要参数");
        return tyFloat();
    }
    if (bn == "data_ptr") {
        // O 节 FFI 宽类型域：数据地址导出。仅连续原生缓冲（张量/str）——
        // List 元素为 tagged 值非连续数组，透传地址无 C 数组语义，明确拒绝。
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "data_ptr() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Tensor &&
            (t->params[0]->kind == Type::Kind::Int ||
             t->params[0]->kind == Type::Kind::Float))
            return tyInt();
        if (t->kind == Type::Kind::Str) return tyInt();
        fail(Errc::E2001_TypeMismatch, c.args[0]->range,
             "data_ptr() 仅支持 Tensor[int]/Tensor[float]/str"
             "（List 元素为 tagged 值，非连续原生数组，禁止透传指针），得到 " +
                 typeToString(t));
        return tyErr();
    }
    if (bn == "ok") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "ok() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        return makeType(Type::Kind::Result, {t, tyStr()});
    }
    if (bn == "err") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "err() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err) {
            fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                 "err() 参数必须是 str，实际为 " + typeToString(t));
            return tyErr();
        }
        // OK 类型为 Unknown 通配，由赋值目标注解特化。
        return makeType(Type::Kind::Result, {tyUnknown(), tyStr()});
    }
    // Option 显式构造（0.9.9）：some(x) ≡ ok(x)，none() ≡ 空选项（None
    // 字面量同值）。嵌套 Option 无表示，与 resolveTypeExpr 同规拒绝。
    if (bn == "some") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "some() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Option)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "Option 不支持嵌套（Option[Option[T]]）；载荷直接用 T");
        return makeType(Type::Kind::Option, {t});
    }
    if (bn == "none") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "none() 不需要参数");
        return tyNone();
    }
    // F-20：panic(msg) 可调用终止——与 assert 语句同走 lang.panic 单一钩子
    //（sg_panic，try/except 可捕，消息双模逐字一致）。恒不返回，语句位
    // tyNone（与 print 同先例）；不入 kPureBuiltins，@pure 内自动拒绝。
    if (bn == "panic") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "panic() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err) {
            fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                 "panic() 参数必须是 str，实际为 " + typeToString(t));
            return tyErr();
        }
        return tyNone();
    }
    // abs/sqrt/exp/log：逐元素数学函数（标量或数值张量）。
    if (bn == "abs" || bn == "sqrt" || bn == "exp" || bn == "log") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        const bool keepInt = (bn == "abs"); // abs 保 dtype，sqrt/exp/log 恒 float
        if (t->kind == Type::Kind::Tensor) {
            TypePtr elem = t->params[0];
            if (elem->kind != Type::Kind::Int &&
                elem->kind != Type::Kind::Float)
                return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                            bn + "() 仅支持数值张量，实际为 " +
                                typeToString(t));
            return makeTensor(keepInt ? elem : tyFloat(), t->shape);
        }
        if (t->kind == Type::Kind::Int || t->kind == Type::Kind::Float)
            return keepInt ? t : tyFloat();
        return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                    bn + "() 仅支持数值或数值张量，实际为 " + typeToString(t));
    }
    // ---- v0.9.3 标量数学：三角/双曲（int 提升 f64，恒返回 float；依 IEEE 754）。
    // 仅标量——张量版本待覆盖集扩展后统一提供（显式拒绝，拒绝即文档）。
    if (bn == "sin" || bn == "cos" || bn == "tan" || bn == "asin" ||
        bn == "acos" || bn == "atan" || bn == "sinh" || bn == "cosh" ||
        bn == "tanh") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Float)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 仅支持标量数值（张量版本暂未提供），实际为 " +
                            typeToString(t));
        return tyFloat();
    }
    if (bn == "atan2") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "atan2() 恰好需要 2 个参数");
        for (size_t k = 0; k < 2; ++k) {
            TypePtr t = checkExpr(*c.args[k]);
            if (t->kind == Type::Kind::Err) return tyErr();
            if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Float)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            "atan2() 仅支持标量数值，实际为 " +
                                typeToString(t));
        }
        return tyFloat();
    }
    // 取整族：int 恒等返回；float → int（NaN/±inf/结果超 int64 范围运行时
    // panic——显式拒绝而非静默回绕）。round = 半到偶（与 Python 3 一致）。
    if (bn == "floor" || bn == "ceil" || bn == "round" || bn == "trunc") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind == Type::Kind::Int || t->kind == Type::Kind::Float)
            return tyInt();
        return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                    bn + "() 仅支持标量数值（张量版本暂未提供），实际为 " +
                        typeToString(t));
    }
    // random/seed：确定性伪随机（splitmix64；双模实现逐位一致，默认状态 0）。
    if (bn == "random") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "random() 不需要参数");
        return tyFloat();
    }
    if (bn == "seed") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "seed() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "seed() 参数必须是 int，实际为 " + typeToString(t));
        return tyNone();
    }
    // sorted(List[T]) → List[T]：稳定升序，返回新列表（列表无变更操作，
    // 就地排序不可表达——One Obvious Way）。
    if (bn == "sorted") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "sorted() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind != Type::Kind::List)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "sorted() 仅支持列表，实际为 " + typeToString(t));
        return t;
    }
    // ---- IO 内建（0.9.4）：函数式文件/目录/标准输入。路径一律 str（窄字节
    // 串）；失败即 panic（可被 try/except 捕获），无 Result 化——调用方取舍。
    if (bn == "input") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "input() 不需要参数");
        return tyStr();
    }
    if (bn == "read_text" || bn == "exists" || bn == "list_dir") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind != Type::Kind::Str)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 路径参数必须是 str，实际为 " +
                            typeToString(t));
        if (bn == "exists") return tyBool();
        if (bn == "list_dir") return makeType(Type::Kind::List, {tyStr()});
        return tyStr(); // read_text
    }
    if (bn == "write_text" || bn == "append_text") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 2 个参数");
        for (size_t k = 0; k < 2; ++k) {
            TypePtr t = checkExpr(*c.args[k]);
            if (t->kind == Type::Kind::Err) return tyErr();
            if (t->kind != Type::Kind::Str)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            bn + "() 参数必须是 str，实际为 " +
                                typeToString(t));
        }
        return tyInt(); // 写入字节数
    }
    // ---- 网络/睡眠内建（0.9.4 F/G 专项）：客户端最小闭环。句柄是不透明
    // int（1 起始表下标）；失败即 panic（与 IO 同哲学），无 Result 化。
    if (bn == "net_connect") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_connect() 恰好需要 2 个参数");
        TypePtr h = checkExpr(*c.args[0]);
        TypePtr p = checkExpr(*c.args[1]);
        if (h->kind != Type::Kind::Str && h->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "net_connect() 主机参数必须是 str，实际为 " +
                            typeToString(h));
        if (p->kind != Type::Kind::Int && p->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[1]->range,
                        "net_connect() 端口参数必须是 int，实际为 " +
                            typeToString(p));
        return tyInt();
    }
    if (bn == "net_send" || bn == "net_recv") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 2 个参数");
        TypePtr h = checkExpr(*c.args[0]);
        TypePtr x = checkExpr(*c.args[1]);
        if (h->kind != Type::Kind::Int && h->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 句柄参数必须是 int，实际为 " +
                            typeToString(h));
        const char* what = bn == "net_send" ? "数据参数" : "上限参数";
        Type::Kind want =
            bn == "net_send" ? Type::Kind::Str : Type::Kind::Int;
        if (x->kind != want && x->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[1]->range,
                        bn + "() " + what + "类型不符，实际为 " +
                            typeToString(x));
        return bn == "net_send" ? tyInt() : tyStr();
    }
    if (bn == "net_close" || bn == "sleep") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 参数必须是 int，实际为 " +
                            typeToString(t));
        return tyNone();
    }
    if (bn == "net_udp_send") {
        if (c.args.size() != 3)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_udp_send() 恰好需要 3 个参数");
        const char* whats[3] = {"主机参数", "端口参数", "数据参数"};
        Type::Kind wants[3] = {Type::Kind::Str, Type::Kind::Int,
                               Type::Kind::Str};
        for (size_t k = 0; k < 3; ++k) {
            TypePtr t = checkExpr(*c.args[k]);
            if (t->kind != wants[k] && t->kind != Type::Kind::Err)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            "net_udp_send() " + std::string(whats[k]) +
                                "类型不符，实际为 " + typeToString(t));
        }
        return tyInt();
    }
    if (bn == "net_http_get") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_http_get() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "net_http_get() URL参数必须是 str，实际为 " +
                            typeToString(t));
        return tyStr();
    }
    // 0.9.10：HTTP GET → Json（status/body 两键；分帧解析双模镜像）
    if (bn == "net_http_get_json") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_http_get_json() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "net_http_get_json() URL参数必须是 str，实际为 " +
                            typeToString(t));
        return makeType(Type::Kind::Json);
    }
    // TCP 服务端：bind（bind+listen 一步）→ accept（阻塞，返回连接句柄）。
    // 超时：set_timeout 同时作用于收/发（ms<=0 显式拒绝——0 语义平台分歧，
    // 拒绝即文档）。
    if (bn == "net_bind") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_bind() 恰好需要 2 个参数");
        TypePtr h = checkExpr(*c.args[0]);
        TypePtr p = checkExpr(*c.args[1]);
        if (h->kind != Type::Kind::Str && h->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "net_bind() 主机参数必须是 str，实际为 " +
                            typeToString(h));
        if (p->kind != Type::Kind::Int && p->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[1]->range,
                        "net_bind() 端口参数必须是 int，实际为 " +
                            typeToString(p));
        return tyInt();
    }
    if (bn == "net_accept") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_accept() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "net_accept() 句柄参数必须是 int，实际为 " +
                            typeToString(t));
        return tyInt();
    }
    if (bn == "net_set_timeout") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_set_timeout() 恰好需要 2 个参数");
        for (size_t k = 0; k < 2; ++k) {
            TypePtr t = checkExpr(*c.args[k]);
            if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            "net_set_timeout() 参数必须是 int，实际为 " +
                                typeToString(t));
        }
        return tyNone();
    }
    // ---- M 专项：标准库广度（统计/倒序/哈希） ----
    // mean/var/std：张量与数值列表（int/float 元素），恒返回 f64（总体方差）。
    // median：同输入域，排序取中（偶数取中间两值平均），恒返回 f64。
    if (bn == "mean" || bn == "var" || bn == "std" || bn == "median") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind == Type::Kind::Tensor) return tyFloat();
        if (t->kind == Type::Kind::List && !t->params.empty()) {
            TypePtr el = t->params[0];
            if (el->kind == Type::Kind::Int || el->kind == Type::Kind::Float)
                return tyFloat();
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 仅支持数值列表（int/float 元素），实际 List[" +
                            typeToString(el) + "]");
        }
        return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                    bn + "() 仅支持张量或数值列表，实际为 " +
                        typeToString(t));
    }
    // reversed：List[T] → List[T]（倒序新列表）；str → str（字节倒序）。
    if (bn == "reversed") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "reversed() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind == Type::Kind::List || t->kind == Type::Kind::Str)
            return t;
        return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                    "reversed() 仅支持列表或字符串，实际为 " +
                        typeToString(t));
    }
    // sha256：str → 64 位小写 hex；crc32：str → 非负 int（CRC-32/ISO-HDLC）。
    if (bn == "sha256" || bn == "crc32") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 参数必须是 str，实际为 " +
                            typeToString(t));
        return bn == "sha256" ? tyStr() : tyInt();
    }
    // json_decode：str → Json（动态 JSON 值；解析失败运行时 panic，
    // 与 BuilderRTJson.hpp 的 sg_json_decode 逐字一致）。
    if (bn == "json_decode") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "json_decode() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "json_decode() 参数必须是 str，实际为 " +
                            typeToString(t));
        return makeType(Type::Kind::Json);
    }
    // ---- HTTP 服务端内建（0.9.8）：读请求 → Json / 写响应 ----
    if (bn == "net_http_read_request") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_http_read_request() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "net_http_read_request() 句柄参数必须是 int，实际为 " +
                            typeToString(t));
        return makeType(Type::Kind::Json);
    }
    if (bn == "net_http_write_response") {
        if (c.args.size() != 4)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "net_http_write_response() 恰好需要 4 个参数");
        const char* const names[4] = {"句柄", "状态码", "内容类型", "响应体"};
        const bool wantInt[4] = {true, true, false, false};
        for (int k = 0; k < 4; ++k) {
            TypePtr t = checkExpr(*c.args[k]);
            bool ok = wantInt[k] ? (t->kind == Type::Kind::Int ||
                                    t->kind == Type::Kind::Err)
                                 : (t->kind == Type::Kind::Str ||
                                    t->kind == Type::Kind::Err);
            if (!ok)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            std::string("net_http_write_response() ") +
                                names[k] + "参数必须是 " +
                                (wantInt[k] ? "int" : "str") + "，实际为 " +
                                typeToString(t));
        }
        return tyInt();
    }
    // ---- 系统编程内建（0.9.4 H 专项）：args/env/时间日期/进程/信号 ----
    if (bn == "args") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "args() 不需要参数");
        return makeType(Type::Kind::List, {tyStr()});
    }
    if (bn == "env" || bn == "has_env" || bn == "unset_env") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 名称参数必须是 str，实际为 " +
                            typeToString(t));
        if (bn == "env") return tyStr();
        if (bn == "has_env") return tyBool();
        return tyNone();
    }
    if (bn == "set_env") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "set_env() 恰好需要 2 个参数");
        for (size_t k = 0; k < 2; ++k) {
            TypePtr t = checkExpr(*c.args[k]);
            if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            std::string(k == 0 ? "set_env() 名称参数"
                                               : "set_env() 值参数") +
                                "必须是 str，实际为 " + typeToString(t));
        }
        return tyBool();
    }
    if (bn == "epoch" || bn == "epoch_ms") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 不需要参数");
        return tyInt();
    }
    if (bn == "localtime" || bn == "gmtime") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 时间参数必须是 int，实际为 " +
                            typeToString(t));
        return makeType(Type::Kind::List, {tyInt()});
    }
    if (bn == "system") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "system() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "system() 命令参数必须是 str，实际为 " +
                            typeToString(t));
        return tyInt();
    }
    if (bn == "spawn_wait") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "spawn_wait() 恰好需要 2 个参数");
        TypePtr p = checkExpr(*c.args[0]);
        TypePtr l = checkExpr(*c.args[1]);
        if (p->kind != Type::Kind::Str && p->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "spawn_wait() 路径参数必须是 str，实际为 " +
                            typeToString(p));
        bool isStrList = l->kind == Type::Kind::List && !l->params.empty() &&
                         l->params[0]->kind == Type::Kind::Str;
        if (!isStrList && l->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[1]->range,
                        "spawn_wait() 参数列表必须是 List[str]，实际为 " +
                            typeToString(l));
        return tyInt();
    }
    if (bn == "signal_caught") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "signal_caught() 不需要参数");
        return tyBool();
    }
    if (bn == "signal_reset") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "signal_reset() 不需要参数");
        return tyNone();
    }
    // ---- 真并发 M1（thread 域）：线程/锁/通道。spawn 实参必须是字符串
    // 字面量且指向当前模块 0 参函数（M1 处方——函数不可作头等值传递，
    // 字面量即符号；跨模块/带参目标一律 E1007 拒绝）。
    if (bn == "thread_spawn") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "thread_spawn() 恰好需要 1 个参数");
        if (c.args[0]->kind != Expr::Kind::StrLit)
            return fail(Errc::E1007_UnsupportedFeature, c.args[0]->range,
                        "thread_spawn() 实参必须是字符串字面量"
                        "（M1 仅支持当前模块函数名）");
        const std::string& fnName =
            static_cast<const StrLit&>(*c.args[0]).strValue;
        auto f = localFuncs_.find(fnName);
        if (f == localFuncs_.end())
            return fail(Errc::E1007_UnsupportedFeature, c.args[0]->range,
                        "thread_spawn() 未找到当前模块函数 '" + fnName + "'");
        if (!f->second.params.empty())
            return fail(Errc::E1007_UnsupportedFeature, c.args[0]->range,
                        "thread_spawn() 目标函数 '" + fnName +
                            "' 必须无参数");
        return tyInt();
    }
    if (bn == "thread_join" || bn == "lock_acquire" || bn == "lock_release" ||
        bn == "chan_recv" || bn == "chan_close") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 句柄参数必须是 int，实际为 " +
                            typeToString(t));
        if (bn == "chan_recv") return tyStr();
        return tyNone();
    }
    if (bn == "lock_new" || bn == "channel_new") {
        if (!c.args.empty())
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 不需要参数");
        return tyInt();
    }
    if (bn == "chan_send") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "chan_send() 恰好需要 2 个参数");
        TypePtr h = checkExpr(*c.args[0]);
        TypePtr m = checkExpr(*c.args[1]);
        if (h->kind != Type::Kind::Int && h->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "chan_send() 句柄参数必须是 int，实际为 " +
                            typeToString(h));
        if (m->kind != Type::Kind::Str && m->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[1]->range,
                        "chan_send() 消息参数必须是 str，实际为 " +
                            typeToString(m));
        return tyNone();
    }
    // ---- GUI M1+M2（gui 域）：仅 AOT/Windows 的轮询式窗口内建。句柄为
    // 不透明 int（1 起始表下标）；gui_poll 返回 Dict[str,str] 事件（恒含
    // tag/x/y/key/text 五键，空 Dict = 无事件——约定单一事实源见
    // BuilderRTGui.hpp 头注释与 LangOps.td）。VM 侧运行到即固定 panic
    // （"gui_* 仅 build 模式可用"，见 VMBuiltin.cpp），静态检查两模一致。
    if (bn == "gui_window") {
        if (c.args.size() != 3)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "gui_window() 恰好需要 3 个参数（标题, 宽, 高）");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "gui_window() 标题参数必须是 str，实际为 " +
                            typeToString(t));
        for (size_t k = 1; k < 3; ++k) {
            TypePtr d = checkExpr(*c.args[k]);
            if (d->kind != Type::Kind::Int && d->kind != Type::Kind::Err)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            "gui_window() 尺寸参数必须是 int，实际为 " +
                                typeToString(d));
        }
        return tyInt();
    }
    if (bn == "gui_label" || bn == "gui_button") {
        if (c.args.size() != 6)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 6 个参数（窗口, 文本, x, y, 宽, 高）");
        TypePtr w = checkExpr(*c.args[0]);
        if (w->kind != Type::Kind::Int && w->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 窗口句柄参数必须是 int，实际为 " +
                            typeToString(w));
        TypePtr t = checkExpr(*c.args[1]);
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[1]->range,
                        bn + "() 文本参数必须是 str，实际为 " +
                            typeToString(t));
        for (size_t k = 2; k < 6; ++k) {
            TypePtr d = checkExpr(*c.args[k]);
            if (d->kind != Type::Kind::Int && d->kind != Type::Kind::Err)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            bn + "() 位置/尺寸参数必须是 int，实际为 " +
                                typeToString(d));
        }
        return tyInt();
    }
    if (bn == "gui_input") {
        if (c.args.size() != 5)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "gui_input() 恰好需要 5 个参数（窗口, x, y, 宽, 高）");
        for (size_t k = 0; k < 5; ++k) {
            TypePtr d = checkExpr(*c.args[k]);
            if (d->kind != Type::Kind::Int && d->kind != Type::Kind::Err)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            std::string(k == 0 ? "gui_input() 窗口句柄参数"
                                               : "gui_input() 位置/尺寸参数") +
                                "必须是 int，实际为 " + typeToString(d));
        }
        return tyInt();
    }
    if (bn == "gui_poll") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "gui_poll() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "gui_poll() 窗口句柄参数必须是 int，实际为 " +
                            typeToString(t));
        return makeType(Type::Kind::Dict, {tyStr(), tyStr()});
    }
    if (bn == "gui_set_text" || bn == "gui_title") {
        if (c.args.size() != 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 2 个参数");
        TypePtr h = checkExpr(*c.args[0]);
        TypePtr t = checkExpr(*c.args[1]);
        if (h->kind != Type::Kind::Int && h->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 句柄参数必须是 int，实际为 " +
                            typeToString(h));
        if (t->kind != Type::Kind::Str && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[1]->range,
                        bn + "() 文本参数必须是 str，实际为 " +
                            typeToString(t));
        return tyNone();
    }
    if (bn == "gui_get_text") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "gui_get_text() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "gui_get_text() 句柄参数必须是 int，实际为 " +
                            typeToString(t));
        return tyStr();
    }
    if (bn == "gui_show" || bn == "gui_hide" || bn == "gui_close") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 句柄参数必须是 int，实际为 " +
                            typeToString(t));
        return tyNone();
    }
    // max/min：张量/列表归约 → 元素类型标量（0.9.9 接 List：bool 按 0/1
    // 计入 int；str 列表拒绝——数值序语义，双模一致）。
    if (bn == "max" || bn == "min") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        bn + "() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind == Type::Kind::List && !t->params.empty()) {
            TypePtr elem = t->params[0];
            if (elem->kind == Type::Kind::Int || elem->kind == Type::Kind::Bool)
                return tyInt();
            if (elem->kind == Type::Kind::Float) return tyFloat();
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        bn + "() 仅支持数值列表归约，实际为 " +
                            typeToString(t));
        }
        if (t->kind == Type::Kind::Tensor) {
            TypePtr elem = t->params[0];
            if (elem->kind != Type::Kind::Int &&
                elem->kind != Type::Kind::Float)
                return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                            bn + "() 仅支持数值张量，实际为 " +
                                typeToString(t));
            return elem;
        }
        return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                    bn + "() 仅支持张量或数值列表归约，不支持 " +
                        typeToString(t));
    }
    // transpose：二维转置 → (m, n) 变 (n, m)。
    if (bn == "transpose") {
        if (c.args.size() != 1)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "transpose() 恰好需要 1 个参数");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind != Type::Kind::Tensor || t->shape.size() != 2)
            return fail(Errc::E2004_ShapeMismatch, c.args[0]->range,
                        "transpose() 要求二维张量，实际为 " + typeToString(t));
        return makeTensor(t->params[0], {t->shape[1], t->shape[0]});
    }
    // reshape：目标形状由正 int 字面量给出（静态可校验）。
    if (bn == "reshape") {
        if (c.args.size() < 2)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "reshape() 需要 1 个张量 + 至少 1 个维度");
        TypePtr t = checkExpr(*c.args[0]);
        if (t->kind == Type::Kind::Err) return tyErr();
        if (t->kind != Type::Kind::Tensor)
            return fail(Errc::E2001_TypeMismatch, c.args[0]->range,
                        "reshape() 首参必须是张量，实际为 " + typeToString(t));
        std::vector<int64_t> ns;
        int64_t prod = 1;
        for (size_t k = 1; k < c.args.size(); ++k) {
            const auto* il = dynamic_cast<const IntLit*>(c.args[k].get());
            if (!il || il->value <= 0)
                return fail(Errc::E2001_TypeMismatch, c.args[k]->range,
                            "reshape() 维度必须是正的 int 字面量");
            ns.push_back(il->value);
            prod *= il->value;
        }
        int64_t src = 1;
        bool srcStatic = true;
        for (int64_t d : t->shape) {
            if (d == kDyn) {
                srcStatic = false;
                break;
            }
            src *= d;
        }
        if (srcStatic && src != prod)
            return fail(Errc::E2004_ShapeMismatch, e.range,
                        "reshape() 元素总数不一致：" + typeToString(t) + " 共 " +
                            std::to_string(src) + " 个，目标形状共 " +
                            std::to_string(prod) + " 个");
        return makeTensor(t->params[0], ns);
    }
    // range
    if (c.args.size() != 1)
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "range() 恰好需要 1 个参数");
    TypePtr t = checkExpr(*c.args[0]);
    if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Err)
        fail(Errc::E2001_TypeMismatch, c.args[0]->range,
             "range() 参数必须是 int，实际为 " + typeToString(t));
    return makeType(Type::Kind::Range);
}

} // namespace scourge
