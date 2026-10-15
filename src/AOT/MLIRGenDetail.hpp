// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT MLIR 生成器内部共享头（MLIRGen*.cpp 分块专用）
// ----------------------------------------------------------------------------
//  仅供 src/AOT/MLIRGen*.cpp 引用，非对外 API（不置于 include/）。
//  按功能模块与生命周期拆分（拆分纪律见 agents.md）：
//    MLIRGen.cpp     驱动与模块装配（generateMLIR 入口、run、模块级、assemble）
//    MLIRGenFunc.cpp 函数生命周期：骨架 + 槽位存取 + 调用发射
//    MLIRGenExpr.cpp 表达式求值：inferVK / genExpr / genBinary / genTernary
//    MLIRGenLit.cpp  字面量与容器构造：str / tensor / list / dict / set / tuple
//    MLIRGenCall.cpp 调用与内建：genCall / genBuiltinCall
//    MLIRGenStmt.cpp 语句与控制流：赋值 / 分支 / 循环 / 断言 / try
//  值模型（VK/Value）、算子注册表、工具函数与 Gen 类声明集中于此；
//  头内自由函数一律 inline，注册表 inline constexpr（跨 TU 单一实体）。
// ============================================================================
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "scourge/Sema/Type.hpp"
#include "scourge/Sema/TypeChecker.hpp"
#include "scourge/Syntax/AST.hpp"
#include "scourge/Tool/Session.hpp"

namespace scourge {
namespace aot {
namespace mlirgen {


// ============================================================================
// 值模型
// ============================================================================

/// AOT 静态值类别。Range 以 i64 存上限 n（与 VM Value::makeRange 一致）；
/// Tensor 以 !llvm.ptr 存 C 运行时 SgTensor 句柄（tInt 记录元素 dtype）；
/// Result/List/Dict/Set/Tuple 以 !llvm.ptr 存 C 运行时堆句柄（lElem 记录
/// 元素/载荷 VK；Tuple 元素类型由检查器类型表按位查取，不落句柄内）。
enum class VK {
    Void, Int, Float, Bool, Str, None, Range, Tensor, Result, List, Dict, Set,
    Tuple, Json, Bad
};

struct Value {
    VK vk = VK::Void;
    std::string ssa;     ///< MLIR SSA 名；None/Void 为空串
    bool tInt = false;   ///< VK::Tensor 时有效：true = i64 元素，false = f64
    uint8_t tRank = 0;   ///< VK::Tensor 时有效：秩（get 标量 / slice 判定用）
    uint8_t lElem = 0;   ///< VK::List 时有效：元素 VK（载荷 VK 语义）
};

inline const char* mlirTy(VK vk) {
    switch (vk) {
    case VK::Int: return "i64";
    case VK::Float: return "f64";
    case VK::Bool: return "i1";
    case VK::Str: return "!llvm.ptr";
    case VK::Range: return "i64"; // 存上限 n
    case VK::Tensor: return "!llvm.ptr";
    case VK::Result: return "!llvm.ptr";
    case VK::List: return "!llvm.ptr";
    case VK::Dict: return "!llvm.ptr";
    case VK::Set: return "!llvm.ptr";
    case VK::Tuple: return "!llvm.ptr";
    case VK::Json: return "!llvm.ptr";
    default: return "";           // None/Void 无 SSA 表示
    }
}

inline bool isNumVK(VK vk) { return vk == VK::Int || vk == VK::Float || vk == VK::Bool; }

/// Type::Kind → VK。Bad 表示 AOT 暂不支持（E1007）。
inline VK vkOfType(const TypePtr& t) {
    if (!t) return VK::Bad;
    switch (t->kind) {
    case Type::Kind::Int: return VK::Int;
    case Type::Kind::Float: return VK::Float;
    case Type::Kind::Bool: return VK::Bool;
    case Type::Kind::Str: return VK::Str;
    case Type::Kind::None: return VK::None;
    case Type::Kind::Range: return VK::Range;
    case Type::Kind::Tensor: return VK::Tensor;
    case Type::Kind::Result: return VK::Result;
    case Type::Kind::List: return VK::List;
    case Type::Kind::Dict: return VK::Dict;
    case Type::Kind::Set: return VK::Set;
    case Type::Kind::Tuple: return VK::Tuple;
    case Type::Kind::Json: return VK::Json;
    default: return VK::Bad;
    }
}

/// 注解名 → VK（与 resolveTypeExpr 可解析的标量名对应）。
inline VK vkOfAnnName(const std::string& n) {
    if (n == "Json") return VK::Json; // 动态 JSON 值（0.9.4 M 专项）
    if (n == "int") return VK::Int;
    if (n == "float") return VK::Float;
    if (n == "bool") return VK::Bool;
    if (n == "str") return VK::Str;
    if (n == "None") return VK::None;
    if (n == "List") return VK::List; // 句柄 VK，元素 VK 由注解参数/首次 store 回填
    if (n == "Result") return VK::Result; // 句柄 VK，载荷 VK 由类型表查取
    if (n == "Dict") return VK::Dict; // 句柄 VK，键/值 VK 由类型表查取
    if (n == "Set") return VK::Set; // 句柄 VK，元素 VK 由类型表查取
    return VK::Bad;
}

// ============================================================================
// 算子注册表：运算符 → 各降级路径元数据（单一事实源）
// ----------------------------------------------------------------------------
//  三张表取代散在 genBinary / genBuiltinCall 里的三元选择链：
//  * kArithOps：算术运算符 → 张量广播编码 tCode（与 C 运行时 sg_tiop/sg_tfop
//    一致）、纯整数路径 lang op 与返回类型、浮点路径（fSym = lang op 走
//    panic 兜底；nullptr = arith 直降，无 panic 上下文故无 opLoc）。
//  * kCmpOps：比较运算符 → 三族 arith 谓词（s = 有符号整型，含 str_cmp 结果
//    与混合提升路径；u = i1 无符号（bool 按数值比较）；f = 浮点 ordered）。
//  * kUnOps：一元数学内建 → 张量逐元素编码 fnid（与 C 运行时 sg_unop 一致，
//    fnid 0 = abs 保 dtype）与标量路径 lang op（iSym 仅 abs 注册——abs 双
//    路径保 dtype，不提升不挂 opLoc；其余提升 f64 且挂 opLoc）。
// ============================================================================
struct OpInfo {
    const char* op;      ///< 源码运算符
    int tCode;           ///< 张量广播 op 编码
    const char* intSym;  ///< 纯整数路径 lang op
    const char* intRet;  ///< 该路径返回 MLIR 类型（i64/f64）
    const char* fSym;    ///< 浮点路径 lang op；nullptr = arith 直降
    const char* fArith;  ///< arith 直降助记符（fSym 为空时有效）
};

struct CmpInfo {
    const char* op; ///< 源码运算符
    const char* s;  ///< 有符号整型谓词
    const char* u;  ///< i1 无符号谓词
    const char* f;  ///< 浮点 ordered 谓词
};

struct UnInfo {
    const char* name; ///< 内建名
    int fnid;         ///< 张量逐元素 fn 编码
    const char* uSym; ///< 标量浮点路径 lang op
    const char* iSym; ///< 标量整数路径 lang op（仅 abs）
};

inline constexpr OpInfo kArithOps[] = {
    {"+", 0, "lang.addi", "i64", nullptr, "arith.addf"},
    {"-", 1, "lang.subi", "i64", nullptr, "arith.subf"},
    {"*", 2, "lang.muli", "i64", nullptr, "arith.mulf"},
    {"/", 3, "lang.divi", "f64", "lang.divf", nullptr},
    {"//", 4, "lang.floordivi", "i64", "lang.floordivf", nullptr},
    {"%", 5, "lang.modi", "i64", "lang.modf", nullptr},
    {"**", 6, "lang.powi", "i64", "lang.powf", nullptr},
};

inline constexpr CmpInfo kCmpOps[] = {
    {"==", "eq", "eq", "oeq"},
    {"!=", "ne", "ne", "une"},
    {"<", "slt", "ult", "olt"},
    {"<=", "sle", "ule", "ole"},
    {">", "sgt", "ugt", "ogt"},
    {">=", "sge", "uge", "oge"},
};

inline constexpr UnInfo kUnOps[] = {
    {"abs", 0, "lang.abs_f", "lang.abs_i"},
    {"sqrt", 1, "lang.sqrt", nullptr},
    {"exp", 2, "lang.exp", nullptr},
    {"log", 3, "lang.log", nullptr},
};

inline const OpInfo* findArithOp(const std::string& op) {
    for (const OpInfo& o : kArithOps)
        if (op == o.op) return &o;
    return nullptr;
}

inline const CmpInfo* findCmpOp(const std::string& op) {
    for (const CmpInfo& c : kCmpOps)
        if (op == c.op) return &c;
    return nullptr;
}

inline const UnInfo* findUnOp(const std::string& name) {
    for (const UnInfo& u : kUnOps)
        if (name == u.name) return &u;
    return nullptr;
}

// ============================================================================
// 基础设施
// ============================================================================

/// 生成中途放弃（诊断已登记）——用异常退栈，generateMLIR 捕获后报 false。
struct GenAbort {};

/// 符号净化：非 [A-Za-z0-9_] → '_'；发生过替换时追加确定性短哈希防碰撞。
inline std::string san(const std::string& name) {
    bool clean = true;
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            clean = false;
            break;
        }
    if (clean) return name;
    uint32_t h = 2166136261u;
    for (char raw : name) {
        h ^= static_cast<unsigned char>(raw);
        h *= 16777619u;
    }
    std::string out;
    for (char c : name)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    char buf[16];
    std::snprintf(buf, sizeof(buf), "_h%08x", h);
    return out + buf;
}

/// f64 → MLIR 字面量（%.17g；补 '.' 保证 MLIR 可解析——MLIR 浮点字面量
/// 要求尾数含小数点：整数补 ".0"，指数形式（如 "1e+100"）在 'e' 前插 '.'；
/// nan/inf 走十六进制位型）。
inline std::string fmtF64(double d) {
    if (std::isnan(d)) return "0x7FF8000000000000";
    if (std::isinf(d)) return d > 0 ? "0x7FF0000000000000" : "0xFFF0000000000000";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", d);
    std::string s = buf;
    if (s.find('.') == std::string::npos) {
        size_t e = s.find_first_of("eE");
        if (e == std::string::npos)
            s += ".0";
        else
            s.insert(e, 1, '.');
    }
    return s;
}

/// MLIR 字符串字面量转义：不可打印/非 ASCII 字节 → \XX 十六进制。
inline std::string escapeMlirStr(const std::string& text) {
    std::string out;
    char buf[8];
    for (char raw : text) {
        unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"': out += "\\\""; continue;
        case '\\': out += "\\\\"; continue;
        case '\n': out += "\\0A"; continue;
        case '\t': out += "\\09"; continue;
        case '\r': out += "\\0D"; continue;
        default: break;
        }
        if (c < 0x20 || c >= 0x7F) {
            std::snprintf(buf, sizeof(buf), "\\%02X", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

/// 内建函数名（与 Compiler::builtinNames 同表）。
inline const char* builtinNames[] = {
    "print",    "str",  "len", "sum",       "now",   "range", "ok", "err",
    "abs",      "sqrt", "exp", "log",       "max",   "min",
    "transpose", "reshape",
    // 0.9.6 O 节 FFI 宽类型域（与 BC::Compiler builtinNames 同表）
    "data_ptr",
    // v0.9.3 标量数学/随机/排序（与 BC::Compiler builtinNames 同表）
    "sin",  "cos",  "tan",   "asin", "acos", "atan", "sinh", "cosh",
    "tanh", "atan2", "floor", "ceil", "round", "trunc",
    "random", "seed", "sorted",
    // 0.9.4 IO 子系统（与 BC::Compiler builtinNames 同表）
    "input", "read_text", "write_text", "append_text", "exists",
    "list_dir",
    // 0.9.4 F/G 专项：网络客户端与睡眠（与 BC::Compiler builtinNames 同表）
    "net_connect", "net_send", "net_recv", "net_close", "net_udp_send",
    "net_http_get", "sleep",
    "net_bind", "net_accept", "net_set_timeout",
    // 0.9.4 H 专项：系统编程（args/env/时间/进程/信号；同表）
    "args", "env", "has_env", "set_env", "unset_env", "epoch", "epoch_ms",
    "localtime", "gmtime", "system", "spawn_wait", "signal_caught",
    "signal_reset",
    // 0.9.4 M 专项：标准库广度（统计/倒序/哈希；与 BC::Compiler 同表）
    "mean", "var", "std", "reversed", "sha256", "crc32",
    // 0.9.4 M 专项：JSON（动态 Json 值；与 BC::Compiler 同表）
    "json_decode"};

inline bool isBuiltinName(const std::string& n) {
    for (const char* b : builtinNames)
        if (n == b) return true;
    return false;
}

// ============================================================================
// 生成器
// ============================================================================

struct Block {
    std::string label;              ///< "^bbN"；入口块为空串
    std::vector<std::string> lines;
    bool terminated = false;
    bool reachable = true; ///< 可达性剪枝标记（方言转换会孤立不可达块）
};

struct Slot {
    VK vk = VK::Void;
    std::string ptr;   ///< 指向槽的 !llvm.ptr SSA 名（None 槽为空）
    bool tInt = false; ///< VK::Tensor 槽：元素 dtype（storeTo 时更新）
    uint8_t tRank = 0; ///< VK::Tensor 槽：秩（get/slice 静态分派用）
    uint8_t lElem = 0; ///< VK::List 槽：元素 VK（storeTo 时更新）
};

struct GlobalSlot {
    VK vk = VK::Void;
    std::string sym; ///< "@sg_g_M__g"
};

// lang op 的 panic 上下文不再显式传参：file/line 编码进 op 的 Location，
// fn 由父 func 符号在 LangToLLVM 降级时反解（AOT 步骤 2 分层纪律）。

class Gen {
public:
    Gen(Session& session, const std::string& entry,
        const ExprTypeMap& exprTypes)
        : s_(session), entry_(entry), types_(exprTypes) {}

    bool run(std::string& out);

    [[noreturn]] void unsupported(const SourceRange& r, const std::string& what) {
        diag().error(Errc::E1007_UnsupportedFeature, curFile_, r,
                     "AOT 后端暂不支持：" + what);
        throw GenAbort{};
    }

private:
    DiagEngine& diag() { return s_.diag(); }

    // ---- 块与 SSA ------------------------------------------------------------
    void emit(const std::string& line) {
        if (!blocks_[static_cast<size_t>(cur_)].terminated)
            blocks_[static_cast<size_t>(cur_)].lines.push_back(line);
    }
    void emitEntry(const std::string& line) { blocks_[0].lines.push_back(line); }
    /// 块下标访问（int 索引的显式符号转换收敛点）。
    Block& blk(int i) { return blocks_[static_cast<size_t>(i)]; }
    void terminate(const std::string& term) {
        Block& b = blocks_[static_cast<size_t>(cur_)];
        if (b.terminated) return;
        b.lines.push_back(term);
        b.terminated = true;
    }
    int newBlock() {
        Block b;
        b.label = "^bb" + std::to_string(++blockCounter_);
        blocks_.push_back(std::move(b));
        return static_cast<int>(blocks_.size()) - 1;
    }
    std::string fresh() { return "%v" + std::to_string(tmp_++); }

    // ---- 入口块常量（惰性；入口块支配全部 body 块） ---------------------------
    std::string oneReg() {
        if (oneSsa_.empty()) {
            oneSsa_ = fresh();
            emitEntry(oneSsa_ + " = arith.constant 1 : i64");
        }
        return oneSsa_;
    }
    std::string zeroI64() {
        if (zeroI64Ssa_.empty()) {
            zeroI64Ssa_ = fresh();
            emitEntry(zeroI64Ssa_ + " = arith.constant 0 : i64");
        }
        return zeroI64Ssa_;
    }
    std::string zeroF64() {
        if (zeroF64Ssa_.empty()) {
            zeroF64Ssa_ = fresh();
            emitEntry(zeroF64Ssa_ + " = arith.constant 0.0 : f64");
        }
        return zeroF64Ssa_;
    }
    std::string boolReg(bool v) {
        std::string& ssa = v ? btrueSsa_ : bfalseSsa_;
        if (ssa.empty()) {
            ssa = fresh();
            emitEntry(ssa + " = arith.constant " + (v ? "true" : "false"));
        }
        return ssa;
    }
    std::string intConst(int64_t v) {
        auto it = intConsts_.find(v);
        if (it != intConsts_.end()) return it->second;
        std::string ssa = fresh();
        emitEntry(ssa + " = arith.constant " + std::to_string(v) + " : i64");
        intConsts_.emplace(v, ssa);
        return ssa;
    }
    std::string i32Const(int32_t v) {
        auto it = i32Consts_.find(v);
        if (it != i32Consts_.end()) return it->second;
        std::string ssa = fresh();
        emitEntry(ssa + " = arith.constant " + std::to_string(v) + " : i32");
        i32Consts_.emplace(v, ssa);
        return ssa;
    }

    // ---- 字符串常量池（跨模块共享、去重） --------------------------------------
    std::string internStr(const std::string& text) {
        if (text.empty()) {
            if (emptyStrDef_.empty())
                emptyStrDef_ =
                    "llvm.mlir.global internal constant @sg_str_empty(\"\\00\") : "
                    "!llvm.array<1 x i8>";
            return "@sg_str_empty";
        }
        auto it = strPool_.find(text);
        if (it != strPool_.end()) return it->second;
        std::string name = "@sg_str_" + std::to_string(strCounter_++);
        size_t len = text.size() + 1;
        strDefs_.push_back("llvm.mlir.global internal constant " + name + "(\"" +
                           escapeMlirStr(text) + "\\00\") : !llvm.array<" +
                           std::to_string(len) + " x i8>");
        strPool_.emplace(text, name);
        return name;
    }

    // ---- 全局符号 ---------------------------------------------------------------
    std::string funcSym(const std::string& m, const std::string& f) {
        return "@sg_" + san(m) + "__" + san(f);
    }
    std::string topSym(const std::string& m) { return "@sg_top_" + san(m); }
    std::string globalSym(const std::string& m, const std::string& g) {
        return "@sg_g_" + san(m) + "__" + san(g);
    }

    // ---- 函数骨架 ---------------------------------------------------------------
    /// symLine：完整签名（不含缩进与大括号），如 "func.func @sg_m__f(%a0: i64) -> i64"。
    void startFunction(const std::string& symLine, const std::string& dispName,
                       VK retVK, bool isTop,
                       const std::vector<std::pair<std::string, VK>>& params);
    void finishFunction();
    void pruneUnreachable();
    std::string renderFunction() const;

    // ---- 存取 -------------------------------------------------------------------
    Slot allocSlot(VK vk);
    Value loadSlot(const Slot& slot);
    void storeTo(Slot& slot, Value v);
    Value loadGlobal(const std::string& module, const std::string& name);
    void storeGlobal(const GlobalSlot& g, Value v);

    // ---- lang op 位置后缀 ---------------------------------------------------------
    /// lang op 的 loc 后缀：panic 上下文（file/line）由 LangToLLVM 从该
    /// FileLineColLoc 恢复（fn 由父 func 符号反解）。panic-ABI op 与能力行
    /// （sg_cap_check 拒绝时需同一上下文，0.9.7）一律携带。
    std::string opLoc() const {
        return "loc(\"" + escapeMlirStr(curFilePath_) + "\":" +
               std::to_string(line_) + ":1)";
    }

    // ---- 调用 -------------------------------------------------------------------
    Value emitCallRet(const std::string& sym, const std::vector<Value>& args,
                      const std::vector<VK>& paramVKs, VK retVK,
                      const TypePtr& retType = nullptr);

    // ---- 表达式 -----------------------------------------------------------------
    VK inferVK(const Expr& e) const;
    Value genExpr(const Expr& e);
    Value genName(const Name& n);
    Value genUnary(const Unary& u);
    Value genBinary(const std::string& op, Value l, Value r,
                    const SourceRange& range, const Expr* rExpr,
                    const Expr* lExpr = nullptr);
    /// 表达式张量类型的全静态形状（无 kDyn）且元素为 int/float → 返回形状，
    /// 否则空（张量降级覆盖集判定：形状属性只对可安全降级的形态写出；
    /// str 张量与 0-d 空形状一律兜底 sg_*）。
    std::optional<std::vector<int64_t>> staticShapeOf(const Expr* e) const;
    /// staticShapeOf 且元素为 f64（linalg/affine f64 覆盖集判定）。
    std::optional<std::vector<int64_t>> staticF64Shape(const Expr* e) const;
    /// 形状属性文本 "array<i64: 24, 2>"（DenseI64ArrayAttr 通用形式）。
    static std::string shapeArrAttr(const std::vector<int64_t>& s);
    Value promoteF64(Value v);
    Value genTernary(const Ternary& t);
    Value genCall(const Call& c);
    /// 跨模块调用统一发射（Attr 链折叠键 / from-import 函数别名共用）。
    Value genCrossCall(const std::string& module, const std::string& fname,
                       const Call& c);
    /// 属性链折叠为规范模块全名（0.9.6，与 Sema qualifiedModuleName /
    /// bc::Compiler::foldModuleKey 三方同构）；非模块链返回空串。
    std::string foldModuleKey(const Expr& base) const;
    Value genBuiltinCall(const std::string& name, const Call& c);
    Value genExternCall(const FuncSig& sig, const std::vector<Value>& args);
    Value strConst(const std::string& text);
    /// 元组 repr（与 VM toString(Tuple, repr) 逐字一致）：括号包裹、", "
    /// 连接、单元素尾逗号、str 元素带引号转义（lang.str_escape）。
    Value genTupleRepr(Value tuple, const TypePtr& ty, const SourceRange& r);
    /// 元组 == 的编译期逐元素展开（检查器已限同构可比）；返回 i1。
    /// genBinary 的 != 对结果取反。嵌套元组递归。
    Value genTupleEq(Value l, Value r, const TypePtr& lt, const TypePtr& rt,
                     const SourceRange& loc);

    // ---- 张量 -----------------------------------------------------------------
    /// 8 字节元素数组第 k 元素地址（手动 *8 缩放，规避 GEP elem_type 版本差异）。
    std::string elemAddr8(const std::string& arr, const std::string& idxSsa);
    /// i64 值数组落盘：入口块 alloca N×i64 + 逐个 store；返回数组指针 SSA。
    std::string buildI64Array(const std::vector<Value>& items);
    /// 全常量张量数据入模块级常量池（Q7/Q8）；返回全局符号。
    std::string internTensorConst(bool isF, int64_t n, const std::string& dataText);
    /// 张量字面量：全标量 → 一维；全张量 → 行拼接（与 VM tensorNew 一致）。
    Value genTensorLit(const ListLit& ls);

    // ---- 列表 / try -------------------------------------------------------------
    /// 指针句柄数组落盘（str 列表字面量）：入口块 alloca N×ptr + 逐个 store。
    std::string buildPtrArray(const std::vector<Value>& items);
    /// f64 值数组落盘（float 列表字面量）：入口块 alloca N×f64 + 逐个 store。
    std::string buildF64Array(const std::vector<Value>& items);
    /// 列表字面量：元素统一后经 lang.list_new_* 构造句柄（与 VM NewList 一致）。
    Value genListLit(const ListLit& ls);
    /// 元组字面量：元素按 dict 位槽 ABI 编码 → lang.list_new_i 构造句柄
    /// （与 i64 List 同构；仅元组专属路径按类型表消费，0.9.6）。
    Value genTupleLit(const TupleLit& tl);
    /// 元素位槽读取：list_get_i（下标编译期常量、检查器静态越界，永不
    /// panic）→ dictFromBits 按元素类型还原。genIndex/相等展开/repr 共用。
    Value genTupleElem(Value tuple, int64_t i, const TypePtr& elemTy,
                       const SourceRange& r);
    /// 字典字面量：键/值统一后经 lang.dict_new 构造句柄（与 VM NewDict 一致）。
    Value genDictLit(const DictLit& dl);
    /// 集合字面量：元素统一后经 lang.set_new 构造句柄（与 VM NewSet 一致）。
    Value genSetLit(const SetLit& sl);
    /// 空集合（Set[T] 注解上下文的空 {}）：按注解槽位 tag 构造 0 元素句柄。
    Value genSetLitEmpty(const DictLit& dl, const TypePtr& ty);
    /// dict ABI：标量/句柄 → i64 位槽（int 直通；float bitcast；bool zext；
    /// str/容器句柄 ptrtoint；None 恒 0）。
    std::string dictToBits(const Value& v);
    /// dict ABI：i64 位槽 → 静态 VK 值（与 dictToBits 互逆；None 无 SSA）。
    Value dictFromBits(VK vk, const std::string& bits);
    /// 弹出 tryStack_ 中 depth 之上的全部 handler（Break/Continue/Return 穿越
    /// try 边界时对齐 VM Compiler 的显式 ExitTry）。
    void popTriesAbove(size_t depth);
    /// try / except：sg_try_push → sg_setjmp 双分支 → body / handler → join。
    void genTry(const Try& t);

    // ---- 语句 -----------------------------------------------------------------
    void genStmtList(const StmtList& list);
    void genStmt(const Stmt& s);
    void emitDefers(); ///< return 点 LIFO 内联展开 defer 块（与 VM Compiler 同构）
    void genAssign(const Assign& a);
    void genAugAssign(const AugAssign& a);
    void genIf(const Expr& cond, const StmtList& thenBody,
               const std::vector<If::Elif>* elifs, const StmtList* elseBody);
    void genWhile(const While& w);
    void genFor(const For& f);
    void genAssert(const Assert& a);

    // ---- 模块级 ----------------------------------------------------------------
    void precollectGlobals();
    void genModule(const std::string& name);
    void genTopAssigns(const Module& m);
    void genFunction(const Module& m, const FuncDef& f);
    void genMain();
    void assemble(std::string& out);
    int lineOf(const SourceRange& r) const {
        return static_cast<int>(s_.src().lineCol(curFile_, r.offset).line);
    }

    // ---- 状态 -----------------------------------------------------------------
    Session& s_;
    std::string entry_;
    /// 检查器产出的表达式类型注解表（单一事实源；emit 处只查不重算）。
    const ExprTypeMap& types_;

    // 模块级产物
    std::vector<std::string> strDefs_;
    std::string emptyStrDef_;
    std::map<std::string, std::string> strPool_;
    int strCounter_ = 0;
    std::vector<std::string> globalDefs_;
    /// 张量字面量常量池（Q7/Q8）：全常量标量数据 → 模块级 llvm.global
    /// 常量（.rdata），替代逐元素 alloca+store 的栈上物化——大字面量
    /// 不再受线程栈上限约束，构建时间从 O(n) 个 op 降为单 op。
    std::vector<std::string> tensorConstDefs_;
    std::map<std::string, std::string> tensorConstPool_;
    int tensorConstCounter_ = 0;
    std::vector<std::string> funcDefs_;
    /// 全部模块的顶级全局：module → name → {VK, 符号}
    std::map<std::string, std::map<std::string, GlobalSlot>> globals_;

    // 当前模块
    std::string curModule_;
    FileId curFile_ = kInvalidFile;
    std::string curFilePath_;
    std::map<std::string, GlobalSlot>* curGlobals_ = nullptr;
    /// 绑定键 → 规范模块全名（0.9.6 L 节，与 bc::Compiler::modAlias_
    /// 同构：全限定名/首段/末段宽容/别名）。
    std::map<std::string, std::string> modAlias_;
    /// from-import 全局别名 → (源模块全名, 源名)——genName 跨模块重定向。
    std::map<std::string, std::pair<std::string, std::string>>
        curFromGlobals_;
    /// from-import 函数别名 → (源模块全名, 源函数名)——genCall Name 分派。
    std::map<std::string, std::pair<std::string, std::string>> curFromFuncs_;

    // 当前函数
    std::string curSymLine_;
    std::string curDispName_;
    VK curRetVK_ = VK::Void;
    bool curIsTop_ = false;
    std::vector<Block> blocks_;
    int cur_ = 0;
    int tmp_ = 0;
    int blockCounter_ = 0;
    int line_ = 0;
    std::string oneSsa_, zeroI64Ssa_, zeroF64Ssa_, btrueSsa_, bfalseSsa_;
    std::map<int64_t, std::string> intConsts_;
    std::map<int32_t, std::string> i32Consts_;
    std::map<std::string, Slot> locals_;
    /// 循环上下文：break / continue 目标块 + 进入循环时的 tryStack_ 深度
    /// （break/continue 穿越循环内 try 边界时需弹到该深度，对齐 VM Compiler
    /// 的 loopEntryTries_ / 显式 ExitTry）。
    struct LoopCtx {
        std::string brk, cont;
        size_t tryDepth = 0;
    };
    std::vector<LoopCtx> loopStack_;
    /// 当前函数已声明的 defer 块（词法序）——return 点按 LIFO 内联展开
    std::vector<const StmtList*> deferStack_;
    /// 激活中的 try handler 句柄栈（与 VM catches_ / C 运行时 sg_try_top
    /// 同构；CFG 语义以块入口快照恢复）。
    std::vector<std::string> tryStack_;
    bool tryDeclsEmitted_ = false; ///< assemble 时 try 蹦床声明只输出一次
    /// 当前函数是否发射过 setjmp（genTry 置位）。置位后本函数槽位读写
    /// 全部走 volatile——含 setjmp 的函数中，非 volatile 槽跨 longjmp 的
    /// 值是 C11 7.13.2.1 意义上的 indeterminate，clang -O2 会据此折叠
    /// catch 块的 load / 删除 handler 的 store（p_try_nested 实证）。
    /// volatile 读写保住 VM「槽值跨 panic 保持」语义。
    bool frameHasSetjmp_ = false;
    /// volatile 后缀：frameHasSetjmp_ 时返回 " volatile"（MLIR LLVM
    /// load/store 的单元属性），否则空串。
    const char* vol() const { return frameHasSetjmp_ ? " volatile" : ""; }
};

} // namespace mlirgen
} // namespace aot
} // namespace scourge
