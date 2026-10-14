// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT MLIR 生成器实现（SG-AST → 文本 MLIR）
// ----------------------------------------------------------------------------
//  生成模型：
//  * 单一 MLIR 模块容纳全部依赖层序模块；MLIR 符号解析与顺序无关。
//  * 每个函数一份入口块（空 label）：常量、llvm.alloca、参数 store 全部
//    提升到入口块，随后的 body 块自 ^bb1 起——支配关系天然成立。
//  * 函数内变量复刻 Compiler 的渐进式局部槽：赋值时建槽、读取先局部后
//    全局，语义与字节码后端零分叉。
//  * panic 语义（溢出/除零/断言失败等）全部下沉到内嵌 C 运行时 sg_* 函数，
//    消息文本与 VM::doBinOp / doBuiltin 逐字一致。
//  * Result / List 与 Tensor 同构的堆句柄模型：lang.result_* / lang.list_*
//    op（LangToLLVM 的 kLowerings 降级）+ Value::lElem 记录元素/载荷 VK。
//  * try/except 是唯一的 func.call 直连运行时例外（控制流设施，与
//    llvm.alloca 同类）：sg_try_push/sg_try_pop 蹦床 + 生成代码直调 CRT
//    @_setjmp（SgTry.jb 首成员 → 句柄即 jmp_buf*；setjmp 必须在生成函数
//    自身活跃帧调用，包装帧方案会被同深度兄弟调用覆盖致死帧），处理器栈
//    簿记与 VM catches_ 逐一对应（EnterTry/ExitTry/recover
//    /doReturn 清理/break-continue 显式 ExitTry 穿越）。
//  * 不进入 AOT 的构造（Dict/Option、print/str 的句柄值、List 下标赋值、
//    Result/List 全局变量等）显式 E1007，无静默回退。
// ============================================================================
#include "scourge/AOT/MLIRGen.hpp"

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

namespace {

// ============================================================================
// 值模型
// ============================================================================

/// AOT 静态值类别。Range 以 i64 存上限 n（与 VM Value::makeRange 一致）；
/// Tensor 以 !llvm.ptr 存 C 运行时 SgTensor 句柄（tInt 记录元素 dtype）；
/// Result/List 以 !llvm.ptr 存 C 运行时堆句柄（lElem 记录元素/载荷 VK）。
enum class VK {
    Void, Int, Float, Bool, Str, None, Range, Tensor, Result, List, Bad
};

struct Value {
    VK vk = VK::Void;
    std::string ssa;     ///< MLIR SSA 名；None/Void 为空串
    bool tInt = false;   ///< VK::Tensor 时有效：true = i64 元素，false = f64
    uint8_t tRank = 0;   ///< VK::Tensor 时有效：秩（get 标量 / slice 判定用）
    uint8_t lElem = 0;   ///< VK::List 时有效：元素 VK（载荷 VK 语义）
};

const char* mlirTy(VK vk) {
    switch (vk) {
    case VK::Int: return "i64";
    case VK::Float: return "f64";
    case VK::Bool: return "i1";
    case VK::Str: return "!llvm.ptr";
    case VK::Range: return "i64"; // 存上限 n
    case VK::Tensor: return "!llvm.ptr";
    case VK::Result: return "!llvm.ptr";
    case VK::List: return "!llvm.ptr";
    default: return "";           // None/Void 无 SSA 表示
    }
}

bool isNumVK(VK vk) { return vk == VK::Int || vk == VK::Float || vk == VK::Bool; }

/// Type::Kind → VK。Bad 表示 AOT 暂不支持（E1007）。
VK vkOfType(const TypePtr& t) {
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
    default: return VK::Bad;
    }
}

/// 注解名 → VK（与 resolveTypeExpr 可解析的标量名对应）。
VK vkOfAnnName(const std::string& n) {
    if (n == "int") return VK::Int;
    if (n == "float") return VK::Float;
    if (n == "bool") return VK::Bool;
    if (n == "str") return VK::Str;
    if (n == "None") return VK::None;
    if (n == "List") return VK::List; // 句柄 VK，元素 VK 由注解参数/首次 store 回填
    if (n == "Result") return VK::Result; // 句柄 VK，载荷 VK 由类型表查取
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

constexpr OpInfo kArithOps[] = {
    {"+", 0, "lang.addi", "i64", nullptr, "arith.addf"},
    {"-", 1, "lang.subi", "i64", nullptr, "arith.subf"},
    {"*", 2, "lang.muli", "i64", nullptr, "arith.mulf"},
    {"/", 3, "lang.divi", "f64", "lang.divf", nullptr},
    {"//", 4, "lang.floordivi", "i64", "lang.floordivf", nullptr},
    {"%", 5, "lang.modi", "i64", "lang.modf", nullptr},
    {"**", 6, "lang.powi", "i64", "lang.powf", nullptr},
};

constexpr CmpInfo kCmpOps[] = {
    {"==", "eq", "eq", "oeq"},
    {"!=", "ne", "ne", "une"},
    {"<", "slt", "ult", "olt"},
    {"<=", "sle", "ule", "ole"},
    {">", "sgt", "ugt", "ogt"},
    {">=", "sge", "uge", "oge"},
};

constexpr UnInfo kUnOps[] = {
    {"abs", 0, "lang.abs_f", "lang.abs_i"},
    {"sqrt", 1, "lang.sqrt", nullptr},
    {"exp", 2, "lang.exp", nullptr},
    {"log", 3, "lang.log", nullptr},
};

const OpInfo* findArithOp(const std::string& op) {
    for (const OpInfo& o : kArithOps)
        if (op == o.op) return &o;
    return nullptr;
}

const CmpInfo* findCmpOp(const std::string& op) {
    for (const CmpInfo& c : kCmpOps)
        if (op == c.op) return &c;
    return nullptr;
}

const UnInfo* findUnOp(const std::string& name) {
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
std::string san(const std::string& name) {
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

/// f64 → MLIR 字面量（%.17g；无 '.'/e/E 补 ".0"；nan/inf 走十六进制位型）。
std::string fmtF64(double d) {
    if (std::isnan(d)) return "0x7FF8000000000000";
    if (std::isinf(d)) return d > 0 ? "0x7FF0000000000000" : "0xFFF0000000000000";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", d);
    std::string s = buf;
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos)
        s += ".0";
    return s;
}

/// MLIR 字符串字面量转义：不可打印/非 ASCII 字节 → \XX 十六进制。
std::string escapeMlirStr(const std::string& text) {
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
const char* builtinNames[] = {
    "print",    "str",  "len", "sum",       "now",   "range", "ok", "err",
    "abs",      "sqrt", "exp", "log",       "max",   "min",
    "transpose", "reshape"};

bool isBuiltinName(const std::string& n) {
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
    /// FileLineColLoc 恢复（fn 由父 func 符号反解）。仅 panic-ABI op 携带。
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
    Value genBuiltinCall(const std::string& name, const Call& c);
    Value strConst(const std::string& text);

    // ---- 张量 -----------------------------------------------------------------
    /// 8 字节元素数组第 k 元素地址（手动 *8 缩放，规避 GEP elem_type 版本差异）。
    std::string elemAddr8(const std::string& arr, const std::string& idxSsa);
    /// i64 值数组落盘：入口块 alloca N×i64 + 逐个 store；返回数组指针 SSA。
    std::string buildI64Array(const std::vector<Value>& items);
    /// 张量字面量：全标量 → 一维；全张量 → 行拼接（与 VM tensorNew 一致）。
    Value genTensorLit(const ListLit& ls);

    // ---- 列表 / try -------------------------------------------------------------
    /// 指针句柄数组落盘（str 列表字面量）：入口块 alloca N×ptr + 逐个 store。
    std::string buildPtrArray(const std::vector<Value>& items);
    /// f64 值数组落盘（float 列表字面量）：入口块 alloca N×f64 + 逐个 store。
    std::string buildF64Array(const std::vector<Value>& items);
    /// 列表字面量：元素统一后经 lang.list_new_* 构造句柄（与 VM NewList 一致）。
    Value genListLit(const ListLit& ls);
    /// 弹出 tryStack_ 中 depth 之上的全部 handler（Break/Continue/Return 穿越
    /// try 边界时对齐 VM Compiler 的显式 ExitTry）。
    void popTriesAbove(size_t depth);
    /// try / except：sg_try_push → sg_setjmp 双分支 → body / handler → join。
    void genTry(const Try& t);

    // ---- 语句 -----------------------------------------------------------------
    void genStmtList(const StmtList& list);
    void genStmt(const Stmt& s);
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
    std::vector<std::string> funcDefs_;
    /// 全部模块的顶级全局：module → name → {VK, 符号}
    std::map<std::string, std::map<std::string, GlobalSlot>> globals_;

    // 当前模块
    std::string curModule_;
    FileId curFile_ = kInvalidFile;
    std::string curFilePath_;
    std::map<std::string, GlobalSlot>* curGlobals_ = nullptr;
    std::vector<std::string> curImports_;

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

// ============================================================================
// 函数骨架
// ============================================================================

void Gen::startFunction(const std::string& symLine, const std::string& dispName,
                        VK retVK, bool isTop,
                        const std::vector<std::pair<std::string, VK>>& params) {
    curSymLine_ = symLine;
    curDispName_ = dispName;
    curRetVK_ = retVK;
    curIsTop_ = isTop;
    blocks_.clear();
    blocks_.emplace_back(); // 入口块（label 空）
    cur_ = 0;
    tmp_ = 0;
    blockCounter_ = 0;
    oneSsa_.clear();
    zeroI64Ssa_.clear();
    zeroF64Ssa_.clear();
    btrueSsa_.clear();
    bfalseSsa_.clear();
    intConsts_.clear();
    i32Consts_.clear();
    locals_.clear();
    loopStack_.clear();
    tryStack_.clear();
    frameHasSetjmp_ = false;

    // 参数槽 + store（None 参数无 SSA，跳过 store）
    for (size_t k = 0; k < params.size(); ++k) {
        Slot slot = allocSlot(params[k].second);
        locals_.emplace(params[k].first, slot);
        if (params[k].second != VK::None) {
            std::string arg = "%a" + std::to_string(k);
            const char* ty = mlirTy(params[k].second);
            // 一律 volatile：try 可能出现在函数体任何位置（allocSlot 先于
            // genTry 发生），入口块一次性 store 的 volatile 开销可忽略。
            emit("llvm.store volatile " + arg + ", " + slot.ptr + " : " + ty +
                 ", !llvm.ptr");
        }
    }

    cur_ = newBlock(); // body 块自 ^bb1 起
}

void Gen::finishFunction() {
    // 入口块补跳转（必须先于剪枝：入口终结符是可达性 BFS 的起点边）。
    Block& entry = blocks_[0];
    if (!entry.terminated) {
        entry.lines.push_back("cf.br ^bb1");
        entry.terminated = true;
    }
    // 可达性剪枝：方言转换（--convert-to-llvm）会把不可达块从函数中
    // 孤立出来，其 func.return 将失去 func.func 父节点而违反合法性。
    pruneUnreachable();
    // 其余可达块补终止（死代码块也必须合法终止；入口块已带终止符）。
    for (size_t i = 1; i < blocks_.size(); ++i) {
        if (blocks_[i].terminated || !blocks_[i].reachable) continue;
        int saved = cur_;
        cur_ = static_cast<int>(i);
        switch (curRetVK_) {
        case VK::Int:
        case VK::Range:
            emit("func.return " + zeroI64() + " : i64");
            break;
        case VK::Float:
            emit("func.return " + zeroF64() + " : f64");
            break;
        case VK::Bool:
            emit("func.return " + boolReg(false) + " : i1");
            break;
        case VK::Str:
            emit("func.return " + strConst("").ssa + " : !llvm.ptr");
            break;
        case VK::Tensor: {
            std::string p = fresh();
            emit(p + " = \"lang.tensor_empty\"() : () -> !llvm.ptr");
            emit("func.return " + p + " : !llvm.ptr");
            break;
        }
        case VK::Result: {
            // 零值返回 = Err("")（与 VM 槽位零值语义一致）
            std::string p = fresh();
            emit(p + " = llvm.mlir.addressof " + internStr("") + " : !llvm.ptr");
            std::string h = fresh();
            emit(h + " = \"lang.result_err\"(" + p + ") : (!llvm.ptr) -> !llvm.ptr");
            emit("func.return " + h + " : !llvm.ptr");
            break;
        }
        case VK::List: {
            std::string p = fresh();
            emit(p + " = \"lang.list_empty\"() : () -> !llvm.ptr");
            emit("func.return " + p + " : !llvm.ptr");
            break;
        }
        default:
            emit("func.return");
            break;
        }
        cur_ = saved;
    }
    funcDefs_.push_back(renderFunction());
}

std::string Gen::renderFunction() const {
    std::string out = "  " + curSymLine_ + " {\n";
    for (const auto& b : blocks_) {
        if (!b.reachable) continue; // 不可达块整块不渲染
        if (!b.label.empty()) out += "  " + b.label + ":\n";
        for (const auto& l : b.lines) out += "    " + l + "\n";
    }
    out += "  }\n";
    return out;
}

void Gen::pruneUnreachable() {
    // 从入口块沿终结符目标（cf.br / cf.cond_br 的 ^bbN 操作数）BFS。
    std::vector<char> reach(blocks_.size(), 0);
    std::vector<int> work{0};
    reach[0] = 1;
    while (!work.empty()) {
        int b = work.back();
        work.pop_back();
        const Block& blk = blocks_[static_cast<size_t>(b)];
        if (!blk.terminated || blk.lines.empty()) continue;
        const std::string& term = blk.lines.back();
        for (size_t p = term.find("^bb"); p != std::string::npos;
             p = term.find("^bb", p + 1)) {
            size_t q = p + 3;
            int id = 0;
            bool any = false;
            while (q < term.size() && term[q] >= '0' && term[q] <= '9') {
                id = id * 10 + (term[q] - '0');
                ++q;
                any = true;
            }
            if (any && static_cast<size_t>(id) < reach.size() &&
                !reach[static_cast<size_t>(id)]) {
                reach[static_cast<size_t>(id)] = 1;
                work.push_back(id);
            }
        }
    }
    for (size_t i = 0; i < blocks_.size(); ++i)
        blocks_[i].reachable = reach[i] != 0;
}

// ============================================================================
// 存取
// ============================================================================

Slot Gen::allocSlot(VK vk) {
    if (vk == VK::None) return Slot{VK::None, ""};
    Slot slot;
    slot.vk = vk;
    slot.ptr = fresh();
    const char* ty = mlirTy(vk);
    emitEntry(slot.ptr + " = llvm.alloca " + oneReg() + " x " + ty +
              " : (i64) -> !llvm.ptr");
    // 零值初始化（语义上先写后读；与 VM 槽位零值对齐，防野值）。
    // 一律 volatile：try 可能在槽分配之后才出现，而含 setjmp 的函数中
    // 非 volatile 槽跨 longjmp 的值 indeterminate（clang -O2 会据此折叠
    // catch 块 load / 删 handler store）；入口块一次性开销可忽略。
    switch (vk) {
    case VK::Int:
    case VK::Range:
        emitEntry("llvm.store volatile " + zeroI64() + ", " + slot.ptr +
                  " : i64, !llvm.ptr");
        break;
    case VK::Float:
        emitEntry("llvm.store volatile " + zeroF64() + ", " + slot.ptr +
                  " : f64, !llvm.ptr");
        break;
    case VK::Bool:
        emitEntry("llvm.store volatile " + boolReg(false) + ", " + slot.ptr +
                  " : i1, !llvm.ptr");
        break;
    case VK::Str: {
        std::string p = fresh();
        emitEntry(p + " = llvm.mlir.addressof " + internStr("") + " : !llvm.ptr");
        emitEntry("llvm.store volatile " + p + ", " + slot.ptr +
                  " : !llvm.ptr, !llvm.ptr");
        break;
    }
    case VK::Tensor: {
        // 零值槽存空张量句柄（防野值；与 VM 槽位零值纪律一致）。
        std::string p = fresh();
        emitEntry(p + " = \"lang.tensor_empty\"() : () -> !llvm.ptr");
        emitEntry("llvm.store volatile " + p + ", " + slot.ptr +
                  " : !llvm.ptr, !llvm.ptr");
        break;
    }
    case VK::Result: {
        // 零值槽存 Err("") 句柄（与 VM 槽位零值纪律一致）
        std::string p = fresh();
        emitEntry(p + " = llvm.mlir.addressof " + internStr("") + " : !llvm.ptr");
        std::string h = fresh();
        emitEntry(h + " = \"lang.result_err\"(" + p +
                  ") : (!llvm.ptr) -> !llvm.ptr");
        emitEntry("llvm.store volatile " + h + ", " + slot.ptr +
                  " : !llvm.ptr, !llvm.ptr");
        break;
    }
    case VK::List: {
        std::string p = fresh();
        emitEntry(p + " = \"lang.list_empty\"() : () -> !llvm.ptr");
        emitEntry("llvm.store volatile " + p + ", " + slot.ptr +
                  " : !llvm.ptr, !llvm.ptr");
        break;
    }
    default:
        break;
    }
    return slot;
}

Value Gen::loadSlot(const Slot& slot) {
    if (slot.vk == VK::None) return Value{VK::None, ""};
    std::string ssa = fresh();
    emit(ssa + " = llvm.load" + vol() + " " + slot.ptr + " : !llvm.ptr -> " +
         mlirTy(slot.vk));
    Value v{slot.vk, ssa, slot.tInt, slot.tRank};
    v.lElem = slot.lElem;
    return v;
}

/// 存储槽位：唯一隐式转换 int → float（与 isConvertible 一致）；
/// Tensor 槽同步更新元素 dtype / 秩记录；List 槽同步更新元素 VK 记录。
void Gen::storeTo(Slot& slot, Value v) {
    if (slot.vk == VK::None) return;
    if (slot.vk == VK::Tensor) {
        slot.tInt = v.tInt;
        slot.tRank = v.tRank;
    }
    if (slot.vk == VK::List) slot.lElem = v.lElem;
    if (slot.vk == VK::Float && v.vk == VK::Int) {
        std::string ssa = fresh();
        emit(ssa + " = arith.sitofp " + v.ssa + " : i64 to f64");
        v = Value{VK::Float, ssa};
    }
    emit(std::string("llvm.store") + vol() + " " + v.ssa + ", " + slot.ptr +
         " : " + mlirTy(slot.vk) + ", !llvm.ptr");
}

Value Gen::loadGlobal(const std::string& module, const std::string& name) {
    auto it = globals_.find(module);
    if (it == globals_.end()) return Value{VK::Bad, ""};
    auto gt = it->second.find(name);
    if (gt == it->second.end()) return Value{VK::Bad, ""};
    const GlobalSlot& g = gt->second;
    if (g.vk == VK::None) return Value{VK::None, ""};
    std::string p = fresh();
    emit(p + " = llvm.mlir.addressof " + g.sym + " : !llvm.ptr");
    std::string ssa = fresh();
    emit(ssa + " = llvm.load " + p + " : !llvm.ptr -> " + mlirTy(g.vk));
    return Value{g.vk, ssa};
}

void Gen::storeGlobal(const GlobalSlot& g, Value v) {
    if (g.vk == VK::None) return;
    std::string p = fresh();
    emit(p + " = llvm.mlir.addressof " + g.sym + " : !llvm.ptr");
    if (g.vk == VK::Float && v.vk == VK::Int) {
        std::string ssa = fresh();
        emit(ssa + " = arith.sitofp " + v.ssa + " : i64 to f64");
        v = Value{VK::Float, ssa};
    }
    emit("llvm.store " + v.ssa + ", " + p + " : " + mlirTy(g.vk) + ", !llvm.ptr");
}

// ============================================================================
// 调用发射
// ============================================================================

Value Gen::emitCallRet(const std::string& sym, const std::vector<Value>& args,
                       const std::vector<VK>& paramVKs, VK retVK,
                       const TypePtr& retType) {
    std::vector<Value> a = args;
    std::string tys, vals;
    size_t n = 0;
    for (size_t k = 0; k < a.size(); ++k) {
        if (paramVKs[k] == VK::None) continue; // None 形参无 SSA
        if (paramVKs[k] == VK::Float && a[k].vk == VK::Int) {
            std::string ssa = fresh();
            emit(ssa + " = arith.sitofp " + a[k].ssa + " : i64 to f64");
            a[k] = Value{VK::Float, ssa};
        }
        if (n++) {
            vals += ", ";
            tys += ", ";
        }
        vals += a[k].ssa;
        tys += mlirTy(paramVKs[k]);
    }
    std::string callTail = (vals.empty() ? "()" : "(" + vals + ")") + " : (" +
                           tys + ") -> " +
                           (retVK == VK::None ? "()" : std::string(mlirTy(retVK)));
    if (retVK == VK::None) {
        emit("func.call " + sym + callTail);
        return Value{VK::None, ""};
    }
    std::string ssa = fresh();
    emit(ssa + " = func.call " + sym + callTail);
    if (retVK == VK::Tensor && retType &&
        retType->kind == Type::Kind::Tensor) {
        bool tInt = !retType->params.empty() &&
                    retType->params[0]->kind != Type::Kind::Float;
        return Value{retVK, ssa, tInt,
                     static_cast<uint8_t>(retType->shape.size())};
    }
    if (retVK == VK::List && retType && retType->kind == Type::Kind::List &&
        !retType->params.empty()) {
        VK ev = vkOfType(retType->params[0]);
        return Value{retVK, ssa, false, 0, static_cast<uint8_t>(ev)};
    }
    return Value{retVK, ssa};
}

// ============================================================================
// 表达式
// ============================================================================

/// 静态 VK 推断（三元结果类型等）。推断不出 → Bad。
/// 类型/形状推导的单一事实源是检查器（Session::exprTypes() 注解表）：
/// 此处优先查表直取，未命中（Err/Unknown 不入表）才走本地保守回退，
/// 回退逻辑与检查器推导规则同构——不在生成期重算推导。
VK Gen::inferVK(const Expr& e) const {
    if (auto it = types_.find(&e); it != types_.end()) {
        VK vk = vkOfType(it->second);
        if (vk != VK::Bad) return vk;
    }
    switch (e.kind) {
    case Expr::Kind::IntLit: return VK::Int;
    case Expr::Kind::FloatLit: return VK::Float;
    case Expr::Kind::BoolLit: return VK::Bool;
    case Expr::Kind::NoneLit: return VK::None;
    case Expr::Kind::StrLit: return VK::Str;
    case Expr::Kind::Name: {
        auto& n = static_cast<const Name&>(e);
        auto lt = locals_.find(n.name);
        if (lt != locals_.end()) return lt->second.vk;
        if (curGlobals_) {
            auto gt = curGlobals_->find(n.name);
            if (gt != curGlobals_->end()) return gt->second.vk;
        }
        return VK::Bad;
    }
    case Expr::Kind::Unary: {
        auto& u = static_cast<const Unary&>(e);
        if (u.op == "not") return VK::Bool;
        return inferVK(*u.operand);
    }
    case Expr::Kind::Binary: {
        auto& b = static_cast<const Binary&>(e);
        const std::string& op = b.op;
        if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
            op == ">=" || op == "and" || op == "or" || op == "in")
            return VK::Bool;
        if (op == "@") return VK::Tensor;
        VK l = inferVK(*b.lhs), r = inferVK(*b.rhs);
        if (op == "+" && l == VK::Str && r == VK::Str) return VK::Str;
        if (op == "*" &&
            ((l == VK::Str && r == VK::Int) || (l == VK::Int && r == VK::Str)))
            return VK::Str;
        if (l == VK::Tensor || r == VK::Tensor) return VK::Tensor;
        if (!isNumVK(l) || !isNumVK(r)) return VK::Bad;
        if (op == "/") return VK::Float;
        return (l == VK::Float || r == VK::Float) ? VK::Float : VK::Int;
    }
    case Expr::Kind::Ternary: {
        auto& t = static_cast<const Ternary&>(e);
        VK a = inferVK(*t.thenE), b = inferVK(*t.elseE);
        if (a == b) return a;
        if (isNumVK(a) && isNumVK(b))
            return (a == VK::Float || b == VK::Float) ? VK::Float : VK::Int;
        return VK::Bad;
    }
    case Expr::Kind::Call: {
        auto& c = static_cast<const Call&>(e);
        if (c.callee->kind == Expr::Kind::Name) {
            auto& n = static_cast<const Name&>(*c.callee);
            if (isBuiltinName(n.name)) {
                if (n.name == "print") return VK::None;
                if (n.name == "str") return VK::Str;
                if (n.name == "len") return VK::Int;
                if (n.name == "now") return VK::Float;
                if (n.name == "range") return VK::Range;
                if (n.name == "abs") {
                    VK a = inferVK(*c.args[0]);
                    return a == VK::Float ? VK::Float : VK::Int;
                }
                if (n.name == "sqrt" || n.name == "exp" || n.name == "log")
                    return VK::Float;
                if (n.name == "ok" || n.name == "err") return VK::Result;
                return VK::Bad; // sum/max/min/transpose/reshape
            }
            auto ft = s_.registry().find(curModule_);
            if (ft != s_.registry().end()) {
                auto f = ft->second->funcs.find(n.name);
                if (f != ft->second->funcs.end()) return vkOfType(f->second.ret);
            }
            return VK::Bad;
        }
        if (c.callee->kind == Expr::Kind::Attr) {
            auto& at = static_cast<const Attr&>(*c.callee);
            if (at.base->kind == Expr::Kind::Name) {
                auto& mn = static_cast<const Name&>(*at.base);
                auto mt = s_.registry().find(mn.name);
                if (mt != s_.registry().end()) {
                    auto f = mt->second->funcs.find(at.name);
                    if (f != mt->second->funcs.end())
                        return vkOfType(f->second.ret);
                }
            }
        }
        return VK::Bad;
    }
    case Expr::Kind::Await:
        return inferVK(*static_cast<const Await&>(e).operand);
    case Expr::Kind::Index: {
        auto& ix = static_cast<const Index&>(e);
        return inferVK(*ix.base) == VK::Tensor ? VK::Tensor : VK::Bad;
    }
    case Expr::Kind::ListLit:
        return static_cast<const ListLit&>(e).checkedAsTensor ? VK::Tensor
                                                              : VK::Bad;
    case Expr::Kind::Attr: {
        auto& at = static_cast<const Attr&>(e);
        if (at.base->kind == Expr::Kind::Name) {
            auto& mn = static_cast<const Name&>(*at.base);
            auto gt = globals_.find(mn.name);
            if (gt != globals_.end()) {
                auto g = gt->second.find(at.name);
                if (g != gt->second.end()) return g->second.vk;
            }
        }
        return VK::Bad;
    }
    default:
        return VK::Bad;
    }
}

Value Gen::strConst(const std::string& text) {
    std::string ssa = fresh();
    emitEntry(ssa + " = llvm.mlir.addressof " + internStr(text) + " : !llvm.ptr");
    return Value{VK::Str, ssa};
}

// ============================================================================
// 张量辅助
// ============================================================================

/// 8 字节元素数组第 k 元素地址：GEP 元素类型取 i64（索引自动按 8 字节
/// 缩放）。LLVM 23 起元素类型跟随结果类型之后（"-> !llvm.ptr, i64"）。
std::string Gen::elemAddr8(const std::string& arr, const std::string& idxSsa) {
    std::string addr = fresh();
    emit(addr + " = llvm.getelementptr " + arr + "[" + idxSsa +
         "] : (!llvm.ptr, i64) -> !llvm.ptr, i64");
    return addr;
}

/// i64 值数组落盘：入口块 alloca N×i64（行主序连续），逐元素 store 到当前块。
std::string Gen::buildI64Array(const std::vector<Value>& items) {
    std::string arr = fresh();
    emitEntry(arr + " = llvm.alloca " +
              intConst(items.empty() ? 1 : (int64_t)items.size()) +
              " x i64 : (i64) -> !llvm.ptr");
    for (size_t k = 0; k < items.size(); ++k) {
        std::string addr = elemAddr8(arr, intConst((int64_t)k));
        emit("llvm.store " + items[k].ssa + ", " + addr + " : i64, !llvm.ptr");
    }
    return arr;
}

/// 张量字面量：全标量 → 一维（任一 Float → f64，与 VM tensorNew 一致）；
/// 全张量 → 行拼接升维（只比形状不比 dtype，混合 → 提升 f64）。
Value Gen::genTensorLit(const ListLit& ls) {
    std::vector<Value> es;
    es.reserve(ls.elems.size());
    for (const auto& el : ls.elems) es.push_back(genExpr(*el));

    bool anyScalar = false, anyTensor = false, anyFloat = false, anyBool = false;
    for (const Value& v : es) {
        if (v.vk == VK::Tensor) {
            anyTensor = true;
        } else if (isNumVK(v.vk)) {
            anyScalar = true;
            if (v.vk == VK::Float) anyFloat = true;
            if (v.vk == VK::Bool) anyBool = true;
        }
    }
    if (anyScalar && anyTensor)
        unsupported(ls.range, "张量字面量标量与张量混排");
    // VM tensorNew 对 bool 元素 panic（"NewTensor 元素必须为数值或张量"），
    // 此处静态拒绝以保持双模失败等价。
    if (anyBool) unsupported(ls.range, "张量字面量含 bool 元素");

    if (anyTensor) {
        // 全张量行拼接升维
        std::string arr = fresh();
        emitEntry(arr + " = llvm.alloca " + intConst((int64_t)es.size()) +
                  " x !llvm.ptr : (i64) -> !llvm.ptr");
        bool allInt = true;
        uint8_t rowRank = es.empty() ? 0 : es[0].tRank;
        for (size_t k = 0; k < es.size(); ++k) {
            std::string addr = elemAddr8(arr, intConst((int64_t)k));
            emit("llvm.store " + es[k].ssa + ", " + addr +
                 " : !llvm.ptr, !llvm.ptr");
            allInt &= es[k].tInt;
        }
        std::string ssa = fresh();
        emit(ssa + " = \"lang.tensor_concat\"(" +
             intConst((int64_t)es.size()) + ", " + arr +
             ") : (i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
        return Value{VK::Tensor, ssa, allInt,
                     static_cast<uint8_t>(rowRank + 1)};
    }

    // 全标量 → 一维
    const bool isF = anyFloat;
    std::string arr = fresh();
    emitEntry(arr + " = llvm.alloca " +
              intConst(es.empty() ? 1 : (int64_t)es.size()) + " x " +
              (isF ? "f64" : "i64") + " : (i64) -> !llvm.ptr");
    for (size_t k = 0; k < es.size(); ++k) {
        Value ev = es[k];
        if (isF && ev.vk == VK::Int) ev = promoteF64(ev);
        std::string addr = elemAddr8(arr, intConst((int64_t)k));
        emit("llvm.store " + ev.ssa + ", " + addr + " : " +
             (isF ? "f64" : "i64") + ", !llvm.ptr");
    }
    std::string ssa = fresh();
    if (isF) {
        emit(ssa + " = \"lang.tensor_from_f64\"(" +
             intConst((int64_t)es.size()) + ", " + arr +
             ") : (i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
    } else {
        emit(ssa + " = \"lang.tensor_from_i64\"(" +
             intConst((int64_t)es.size()) + ", " + arr +
             ") : (i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
    }
    return Value{VK::Tensor, ssa, !isF, 1};
}

/// 指针句柄数组落盘（str 列表字面量）：入口块 alloca N×ptr，逐元素 store。
std::string Gen::buildPtrArray(const std::vector<Value>& items) {
    std::string arr = fresh();
    emitEntry(arr + " = llvm.alloca " +
              intConst(items.empty() ? 1 : (int64_t)items.size()) +
              " x !llvm.ptr : (i64) -> !llvm.ptr");
    for (size_t k = 0; k < items.size(); ++k) {
        std::string addr = elemAddr8(arr, intConst((int64_t)k));
        emit("llvm.store " + items[k].ssa + ", " + addr +
             " : !llvm.ptr, !llvm.ptr");
    }
    return arr;
}

/// f64 值数组落盘（float 列表字面量）：入口块 alloca N×f64，逐元素 store。
std::string Gen::buildF64Array(const std::vector<Value>& items) {
    std::string arr = fresh();
    emitEntry(arr + " = llvm.alloca " +
              intConst(items.empty() ? 1 : (int64_t)items.size()) +
              " x f64 : (i64) -> !llvm.ptr");
    for (size_t k = 0; k < items.size(); ++k) {
        std::string addr = elemAddr8(arr, intConst((int64_t)k));
        emit("llvm.store " + items[k].ssa + ", " + addr + " : f64, !llvm.ptr");
    }
    return arr;
}

/// 列表字面量：元素 VK 统一（同 VK 原样；int/float 混排 → f64，与检查器
/// unifyNumeric 一致）后经 lang.list_new_* 构造堆句柄（与 VM NewList 一致）。
Value Gen::genListLit(const ListLit& ls) {
    std::vector<Value> es;
    es.reserve(ls.elems.size());
    for (const auto& el : ls.elems) es.push_back(genExpr(*el));

    if (es.empty()) {
        std::string ssa = fresh();
        emit(ssa + " = \"lang.list_empty\"() : () -> !llvm.ptr");
        return Value{VK::List, ssa};
    }

    // 元素统一：与检查器 checkListLit 一致——任一 Float → 全 f64；
    // 同为 Int 则保持 i64（bool 只能与 bool 混排，检查器 E2001 已拒异型）。
    bool anyFloat = false, allInt = true, allBool = true, allStr = true;
    for (const Value& v : es) {
        if (v.vk == VK::Float) anyFloat = true;
        if (v.vk != VK::Int) allInt = false;
        if (v.vk != VK::Bool) allBool = false;
        if (v.vk != VK::Str) allStr = false;
    }
    if (!allInt && !allBool && !allStr && !anyFloat)
        unsupported(ls.range, "列表字面量含该元素类型");

    // 逐元素统一转换（anyFloat 时 int/bool → f64；bool 元素在整型表中 zext i64）
    const bool isF = anyFloat;
    if (isF) {
        for (Value& v : es) {
            if (v.vk == VK::Int) v = promoteF64(v);
            else if (v.vk == VK::Bool) {
                std::string f = fresh();
                emit(f + " = arith.uitofp " + v.ssa + " : i1 to f64");
                v = Value{VK::Float, f};
            }
        }
        std::string arr = buildF64Array(es);
        std::string h = fresh();
        emit(h + " = \"lang.list_new_f\"(" + intConst((int64_t)es.size()) +
             ", " + arr + ") : (i64, !llvm.ptr) -> !llvm.ptr");
        return Value{VK::List, h, false, 0, static_cast<uint8_t>(VK::Float)};
    }
    if (allStr) {
        std::string arr = buildPtrArray(es);
        std::string h = fresh();
        emit(h + " = \"lang.list_new_s\"(" + intConst((int64_t)es.size()) +
             ", " + arr + ") : (i64, !llvm.ptr) -> !llvm.ptr");
        return Value{VK::List, h, false, 0, static_cast<uint8_t>(VK::Str)};
    }
    if (allBool) {
        // bool 列表：运行时按 i64 存储（etag 3），元素经 zext 提升
        std::vector<Value> wide;
        wide.reserve(es.size());
        for (const Value& v : es) {
            std::string z = fresh();
            emit(z + " = arith.extui " + v.ssa + " : i1 to i64");
            wide.push_back(Value{VK::Int, z});
        }
        std::string arr = buildI64Array(wide);
        std::string h = fresh();
        emit(h + " = \"lang.list_new_b\"(" + intConst((int64_t)es.size()) +
             ", " + arr + ") : (i64, !llvm.ptr) -> !llvm.ptr");
        return Value{VK::List, h, false, 0, static_cast<uint8_t>(VK::Bool)};
    }
    // 全 int
    std::string arr = buildI64Array(es);
    std::string h = fresh();
    emit(h + " = \"lang.list_new_i\"(" + intConst((int64_t)es.size()) + ", " +
         arr + ") : (i64, !llvm.ptr) -> !llvm.ptr");
    return Value{VK::List, h, false, 0, static_cast<uint8_t>(VK::Int)};
}

Value Gen::genExpr(const Expr& e) {
    line_ = lineOf(e.range); // panic 行号锚点（markLine 等价）
    switch (e.kind) {
    case Expr::Kind::IntLit:
        return Value{VK::Int, intConst(static_cast<const IntLit&>(e).value)};
    case Expr::Kind::FloatLit: {
        double d = static_cast<const FloatLit&>(e).value;
        std::string ssa = fresh();
        emitEntry(ssa + " = arith.constant " + fmtF64(d) + " : f64");
        return Value{VK::Float, ssa};
    }
    case Expr::Kind::BoolLit:
        return Value{VK::Bool, boolReg(static_cast<const BoolLit&>(e).boolValue)};
    case Expr::Kind::NoneLit:
        return Value{VK::None, ""};
    case Expr::Kind::StrLit:
        return strConst(static_cast<const StrLit&>(e).strValue);
    case Expr::Kind::Name:
        return genName(static_cast<const Name&>(e));
    case Expr::Kind::Unary:
        return genUnary(static_cast<const Unary&>(e));
    case Expr::Kind::Binary: {
        auto& b = static_cast<const Binary&>(e);
        Value l = genExpr(*b.lhs);
        Value r = genExpr(*b.rhs);
        return genBinary(b.op, l, r, b.range, b.rhs.get(), b.lhs.get());
    }
    case Expr::Kind::Ternary:
        return genTernary(static_cast<const Ternary&>(e));
    case Expr::Kind::Call:
        return genCall(static_cast<const Call&>(e));
    case Expr::Kind::Attr: {
        auto& at = static_cast<const Attr&>(e);
        if (at.base->kind == Expr::Kind::Name) {
            auto& mn = static_cast<const Name&>(*at.base);
            // 模块常量访问：m.CONST → 全局加载
            if (std::find(curImports_.begin(), curImports_.end(), mn.name) !=
                curImports_.end()) {
                Value v = loadGlobal(mn.name, at.name);
                if (v.vk == VK::Bad)
                    unsupported(e.range, "模块 '" + mn.name + "' 无常量 '" +
                                             at.name + "'");
                return v;
            }
        }
        unsupported(e.range, "成员访问（Attr）");
    }
    case Expr::Kind::Await:
        unsupported(e.range, "async / await");
    case Expr::Kind::Index: {
        auto& ix = static_cast<const Index&>(e);
        Value base = genExpr(*ix.base);
        if (base.vk == VK::List) {
            // List 单下标（检查器 E2004 已拒多下标）：list_get_* 内做
            // normIndex（负下标归一 + 越界 panic，消息与 VM 逐字一致）
            if (ix.indices.size() != 1)
                unsupported(ix.range, "List 只支持单下标");
            Value iv = genExpr(*ix.indices[0]);
            if (iv.vk != VK::Int)
                unsupported(ix.indices[0]->range, "列表下标必须是 int");
            std::string ssa = fresh();
            switch (static_cast<VK>(base.lElem)) {
            case VK::Int:
                emit(ssa + " = \"lang.list_get_i\"(" + base.ssa + ", " + iv.ssa +
                     ") : (!llvm.ptr, i64) -> i64 " + opLoc());
                return Value{VK::Int, ssa};
            case VK::Float:
                emit(ssa + " = \"lang.list_get_f\"(" + base.ssa + ", " + iv.ssa +
                     ") : (!llvm.ptr, i64) -> f64 " + opLoc());
                return Value{VK::Float, ssa};
            case VK::Bool:
                emit(ssa + " = \"lang.list_get_b\"(" + base.ssa + ", " + iv.ssa +
                     ") : (!llvm.ptr, i64) -> i1 " + opLoc());
                return Value{VK::Bool, ssa};
            case VK::Str:
                emit(ssa + " = \"lang.list_get_s\"(" + base.ssa + ", " + iv.ssa +
                     ") : (!llvm.ptr, i64) -> !llvm.ptr " + opLoc());
                return Value{VK::Str, ssa};
            default:
                unsupported(ix.base->range, "该元素类型的列表下标读取");
            }
        }
        if (base.vk != VK::Tensor)
            unsupported(ix.base->range, "下标读取仅支持张量与列表");
        // 下标逐个求值（与 Compiler 的 base → idxs 求值序一致）
        std::vector<Value> idxs;
        for (const auto& ie : ix.indices) {
            Value iv = genExpr(*ie);
            if (iv.vk != VK::Int)
                unsupported(ie->range, "张量下标必须是 int");
            idxs.push_back(iv);
        }
        std::string arr = buildI64Array(idxs);
        std::string ssa = fresh();
        if (idxs.size() == base.tRank) { // 全秩 → 标量读取
            if (base.tInt)
                emit(ssa + " = \"lang.tensor_get_i\"(" + base.ssa + ", " +
                     intConst((int64_t)idxs.size()) + ", " + arr +
                     ") : (!llvm.ptr, i64, !llvm.ptr) -> i64 " + opLoc());
            else
                emit(ssa + " = \"lang.tensor_get_f\"(" + base.ssa + ", " +
                     intConst((int64_t)idxs.size()) + ", " + arr +
                     ") : (!llvm.ptr, i64, !llvm.ptr) -> f64 " + opLoc());
            return Value{base.tInt ? VK::Int : VK::Float, ssa};
        }
        // 部分索引 → 子张量切片（n > 秩时由运行时 panic，消息与 VM 一致）
        emit(ssa + " = \"lang.tensor_slice\"(" + base.ssa + ", " +
             intConst((int64_t)idxs.size()) + ", " + arr +
             ") : (!llvm.ptr, i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
        uint8_t outRank = idxs.size() < base.tRank
                              ? static_cast<uint8_t>(base.tRank - idxs.size())
                              : uint8_t{0};
        return Value{VK::Tensor, ssa, base.tInt, outRank};
    }
    case Expr::Kind::ListLit: {
        auto& ls = static_cast<const ListLit&>(e);
        if (!ls.checkedAsTensor) return genListLit(ls);
        return genTensorLit(ls);
    }
    case Expr::Kind::TupleLit:
        unsupported(e.range, "元组字面量");
    }
    unsupported(e.range, "该表达式");
}

Value Gen::genName(const Name& n) {
    // 渐进式局部槽：读取先局部后全局（与 Compiler 语义一致）。
    auto lt = locals_.find(n.name);
    if (lt != locals_.end()) return loadSlot(lt->second);
    if (curGlobals_) {
        auto gt = curGlobals_->find(n.name);
        if (gt != curGlobals_->end()) {
            if (gt->second.vk == VK::None) return Value{VK::None, ""};
            std::string p = fresh();
            emit(p + " = llvm.mlir.addressof " + gt->second.sym + " : !llvm.ptr");
            std::string ssa = fresh();
            emit(ssa + " = llvm.load " + p + " : !llvm.ptr -> " +
                 mlirTy(gt->second.vk));
            return Value{gt->second.vk, ssa};
        }
    }
    unsupported(n.range, "内建函数 / 模块名不能作为值使用");
}

Value Gen::genUnary(const Unary& u) {
    Value v = genExpr(*u.operand);
    if (u.op == "not") {
        std::string ssa = fresh();
        emit(ssa + " = arith.xori " + v.ssa + ", " + boolReg(true) + " : i1");
        return Value{VK::Bool, ssa};
    }
    if (u.op == "-") {
        if (v.vk == VK::Int) {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.negi\"(" + v.ssa + ") : (i64) -> i64 " +
                 opLoc());
            return Value{VK::Int, ssa};
        }
        if (v.vk == VK::Float) {
            std::string ssa = fresh();
            emit(ssa + " = arith.negf " + v.ssa + " : f64");
            return Value{VK::Float, ssa};
        }
        unsupported(u.range, "取负不支持该类型");
    }
    if (u.op == "+") {
        if (v.vk == VK::Int || v.vk == VK::Float) return v; // 原样
        unsupported(u.range, "一元 '+' 不支持该类型");
    }
    unsupported(u.range, "未知一元运算符 '" + u.op + "'");
}

Value Gen::promoteF64(Value v) {
    if (v.vk == VK::Float) return v;
    std::string ssa = fresh();
    emit(ssa + " = arith.sitofp " + v.ssa + " : i64 to f64");
    return Value{VK::Float, ssa};
}

std::optional<std::vector<int64_t>>
Gen::staticShapeOf(const Expr* e) const {
    if (!e) return std::nullopt;
    auto it = types_.find(e);
    if (it == types_.end() || it->second->kind != Type::Kind::Tensor)
        return std::nullopt;
    const Type& t = *it->second;
    // 仅 int/float 元素进入覆盖集（str 等其他元素张量兜底 sg_*）
    if (t.params.empty() || (t.params[0]->kind != Type::Kind::Float &&
                             t.params[0]->kind != Type::Kind::Int))
        return std::nullopt;
    for (int64_t d : t.shape)
        if (d == kDyn) return std::nullopt;
    // 0-d 张量不在覆盖集（shapeArrAttr 空形状文本非法；当前语言亦不可达，
    // 全量索引得标量、部分索引秩 ≥ 1——守卫保留为防御）
    if (t.shape.empty()) return std::nullopt;
    return t.shape;
}

std::optional<std::vector<int64_t>>
Gen::staticF64Shape(const Expr* e) const {
    auto sh = staticShapeOf(e);
    if (!sh) return std::nullopt;
    if (types_.find(e)->second->params[0]->kind != Type::Kind::Float)
        return std::nullopt;
    return sh;
}

std::string Gen::shapeArrAttr(const std::vector<int64_t>& s) {
    std::string out = "array<i64: ";
    for (size_t k = 0; k < s.size(); ++k) {
        if (k) out += ", ";
        out += std::to_string(s[k]);
    }
    out += ">";
    return out;
}

Value Gen::genBinary(const std::string& op, Value l, Value r,
                     const SourceRange& range, const Expr* rExpr,
                     const Expr* lExpr) {
    // ---- 逻辑（两侧恒为 bool） ----
    if (op == "and") {
        std::string ssa = fresh();
        emit(ssa + " = arith.andi " + l.ssa + ", " + r.ssa + " : i1");
        return Value{VK::Bool, ssa};
    }
    if (op == "or") {
        std::string ssa = fresh();
        emit(ssa + " = arith.ori " + l.ssa + ", " + r.ssa + " : i1");
        return Value{VK::Bool, ssa};
    }

    // ---- 成员测试：仅 str in str ----
    if (op == "in") {
        if (l.vk == VK::Str && r.vk == VK::Str) {
            // C 运行时返回 int（i32），与 ABI 一致；调用方转回 i1 布尔。
            // 参数序与 VM 的 r.s.find(l.s) 对齐：hay = 右操作数。
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_contains\"(" + r.ssa + ", " + l.ssa +
                 ") : (!llvm.ptr, !llvm.ptr) -> i32 " + opLoc());
            std::string z = fresh();
            emit(z + " = arith.constant 0 : i32");
            std::string b = fresh();
            emit(b + " = arith.cmpi ne, " + ssa + ", " + z + " : i32");
            return Value{VK::Bool, b};
        }
        unsupported(range, "'in' 仅支持 str 右操作数");
    }

    // ---- 比较（谓词查 kCmpOps 三族） ----
    if (const CmpInfo* cop = findCmpOp(op)) {
        const bool eq = op == "==" || op == "!=";
        if (l.vk == VK::Tensor || r.vk == VK::Tensor)
            unsupported(range, "张量比较运算");
        // None：常量折叠（VM valuesEqual：None==None → true，其余 false）。
        if (l.vk == VK::None || r.vk == VK::None) {
            bool res = eq && l.vk == VK::None && r.vk == VK::None;
            return Value{VK::Bool, boolReg(res)};
        }
        if (l.vk == VK::Str && r.vk == VK::Str) {
            std::string s = fresh();
            emit(s + " = \"lang.str_cmp\"(" + l.ssa + ", " + r.ssa +
                 ") : (!llvm.ptr, !llvm.ptr) -> i64 " + opLoc());
            std::string b = fresh();
            emit(b + " = arith.cmpi " + cop->s + ", " + s + ", " + zeroI64() +
                 " : i64");
            return Value{VK::Bool, b};
        }
        if (l.vk == VK::Int && r.vk == VK::Int) {
            std::string b = fresh();
            emit(b + " = arith.cmpi " + cop->s + ", " + l.ssa + ", " + r.ssa +
                 " : i64");
            return Value{VK::Bool, b};
        }
        if (l.vk == VK::Bool && r.vk == VK::Bool) {
            // VM 按数值比较（asDouble）——0/1 的无符号整数比较等价。
            std::string b = fresh();
            emit(b + " = arith.cmpi " + cop->u + ", " + l.ssa + ", " + r.ssa +
                 " : i1");
            return Value{VK::Bool, b};
        }
        // 数值混合（int/float）：提升 f64；Ne 用 une（NaN != NaN 与 VM 一致）。
        Value lf = promoteF64(l), rf = promoteF64(r);
        std::string b = fresh();
        emit(b + " = arith.cmpf " + cop->f + ", " + lf.ssa + ", " + rf.ssa +
             " : f64");
        return Value{VK::Bool, b};
    }

    // ---- 字符串运算 ----
    if (op == "+" && l.vk == VK::Str && r.vk == VK::Str) {
        std::string ssa = fresh();
        emit(ssa + " = \"lang.str_concat\"(" + l.ssa + ", " + r.ssa +
             ") : (!llvm.ptr, !llvm.ptr) -> !llvm.ptr " + opLoc());
        return Value{VK::Str, ssa};
    }
    if (op == "*" &&
        ((l.vk == VK::Str && r.vk == VK::Int) ||
         (l.vk == VK::Int && r.vk == VK::Str))) {
        Value s = l.vk == VK::Str ? l : r;
        Value n = l.vk == VK::Str ? r : l;
        std::string ssa = fresh();
        emit(ssa + " = \"lang.str_repeat\"(" + s.ssa + ", " + n.ssa +
             ") : (!llvm.ptr, i64) -> !llvm.ptr " + opLoc());
        return Value{VK::Str, ssa};
    }

    // ---- 张量运算（@ 与算术广播；语义与 VM tensorBinOp / tensorMatMul 一致） ----
    // op 编码查 kArithOps（与 C 运行时 sg_tiop/sg_tfop 一致）。
    const OpInfo* aop = findArithOp(op);
    const int tOp = aop ? aop->tCode : -1;
    if (op == "@" ||
        (tOp >= 0 && (l.vk == VK::Tensor || r.vk == VK::Tensor))) {
        if (op == "@") {
            if (l.vk != VK::Tensor || r.vk != VK::Tensor)
                unsupported(range, "'@' 需要两个张量操作数");
            // linalg 覆盖集：双 f64 + 两侧形状全静态 + 内维一致（内维不匹配
            // 保留兜底 sg_*，运行时 panic 语义与 VM 零分叉）
            std::string attrs;
            if (!l.tInt && !r.tInt) {
                auto lShape = staticF64Shape(lExpr);
                auto rShape = staticF64Shape(rExpr);
                if (lShape && rShape && (*lShape)[1] == (*rShape)[0])
                    attrs = " {lhs_shape = " + shapeArrAttr(*lShape) +
                            ", rhs_shape = " + shapeArrAttr(*rShape) + "}";
            }
            std::string ssa = fresh();
            emit(ssa + " = \"lang.tensor_matmul\"(" + l.ssa + ", " + r.ssa +
                 ")" + attrs + " : (!llvm.ptr, !llvm.ptr) -> !llvm.ptr " +
                 opLoc());
            return Value{VK::Tensor, ssa, l.tInt && r.tInt, 2};
        }
        if (l.vk == VK::Tensor && r.vk == VK::Tensor) {
            // 张量降级覆盖集：+ - * / // % **（0-6）全 dtype 组合、形状全
            // 静态。双侧都必须静态——检查器 unifyShape 允许「静态⊕动态」
            // 统一成静态结果，只查单侧会把动态张量误当静态降级（输入桥
            // 读错内存）；int 溢出/除零 panic 由逐元素 lang 标量 op 调用
            // 保持零分叉（sg_add_i64 等 == sg_tiop 同一实现）。
            std::string attrs;
            if (tOp >= 0) {
                auto lShape = staticShapeOf(lExpr);
                auto rShape = staticShapeOf(rExpr);
                if (lShape && rShape && *lShape == *rShape)
                    attrs = " {kind = " + std::to_string(tOp) + " : i32" +
                            ", shape = " + shapeArrAttr(*lShape) +
                            ", lhs_int = " + std::to_string(l.tInt ? 1 : 0) +
                            " : i32, rhs_int = " +
                            std::to_string(r.tInt ? 1 : 0) + " : i32}";
            }
            std::string ssa = fresh();
            emit(ssa + " = \"lang.tensor_binop_tt\"(" + i32Const(tOp) + ", " +
                 l.ssa + ", " + r.ssa + ")" + attrs +
                 " : (i32, !llvm.ptr, !llvm.ptr) -> !llvm.ptr " + opLoc());
            // outInt = 非 '/' 且两侧 int（VM tensorBinOp 规则）；形状相等秩不变
            return Value{VK::Tensor, ssa, tOp != 3 && l.tInt && r.tInt,
                         l.tRank};
        }
        Value sc = l.vk == VK::Tensor ? r : l;
        if (!isNumVK(sc.vk))
            unsupported(range, "运算符 '" + op + "' 的标量操作数类型");
        // 标量双寄存器（vi/vf）按静态类型准备；bool 按 int 处理（VM 规则）
        std::string vi, vf;
        if (sc.vk == VK::Float) {
            vi = zeroI64();
            vf = sc.ssa;
        } else if (sc.vk == VK::Bool) {
            std::string z = fresh();
            emit(z + " = arith.extui " + sc.ssa + " : i1 to i64");
            vi = z;
            std::string f = fresh();
            emit(f + " = arith.uitofp " + sc.ssa + " : i1 to f64");
            vf = f;
        } else {
            vi = sc.ssa;
            std::string f = fresh();
            emit(f + " = arith.sitofp " + sc.ssa + " : i64 to f64");
            vf = f;
        }
        std::string si = i32Const(sc.vk != VK::Float ? 1 : 0);
        Value t = l.vk == VK::Tensor ? l : r;
        // 张量降级覆盖集：+ - * / // % **（0-6）全 dtype 组合、张量侧形状
        // 全静态（标量广播任意形状合法）。t_int/s_int 告知降级 pass 张量
        // 元素与标量槽位（vi/sf）的 dtype；float 张量 × int 标量走 vf（
        // MLIRGen 已 sitofp），int 张量 × float 标量降级处 sitofp 元素。
        //（ts 查左表达式、st 查右表达式——张量所在侧）
        std::string attrs;
        if (tOp >= 0) {
            if (auto sh = staticShapeOf(l.vk == VK::Tensor ? lExpr : rExpr))
                attrs = " {kind = " + std::to_string(tOp) + " : i32" +
                        ", shape = " + shapeArrAttr(*sh) +
                        ", t_int = " + std::to_string(t.tInt ? 1 : 0) +
                        " : i32, s_int = " +
                        std::to_string(sc.vk != VK::Float ? 1 : 0) + " : i32}";
        }
        std::string ssa = fresh();
        if (l.vk == VK::Tensor) { // 张量 op 标量 → ts
            emit(ssa + " = \"lang.tensor_binop_ts\"(" + i32Const(tOp) + ", " +
                 t.ssa + ", " + si + ", " + vi + ", " + vf + ")" + attrs +
                 " : (i32, !llvm.ptr, i32, i64, f64) -> !llvm.ptr " + opLoc());
        } else { // 标量 op 张量 → st（标量在左，C 侧负责方向）
            emit(ssa + " = \"lang.tensor_binop_st\"(" + i32Const(tOp) + ", " +
                 si + ", " + vi + ", " + vf + ", " + t.ssa + ")" + attrs +
                 " : (i32, i32, i64, f64, !llvm.ptr) -> !llvm.ptr " + opLoc());
        }
        return Value{VK::Tensor, ssa, tOp != 3 && t.tInt && sc.vk != VK::Float,
                     t.tRank};
    }

    // ---- 算术（kArithOps 注册表驱动） ----
    if (aop) {
        if (!isNumVK(l.vk) || !isNumVK(r.vk))
            unsupported(range, "运算符 '" + op + "' 的操作数类型");

        if (l.vk != VK::Float && r.vk != VK::Float) {
            // ---- 纯整数路径 ----
            if (op == "**") {
                // 负指数结果为 float（结果类型依赖运行时符号）——AOT 要求
                // 指数为非负整数字面量。
                if (!rExpr || rExpr->kind != Expr::Kind::IntLit ||
                    static_cast<const IntLit&>(*rExpr).value < 0)
                    unsupported(range,
                                "整数幂 '**' 需要非负整数字面量指数");
            }
            std::string ssa = fresh();
            emit(ssa + " = \"" + aop->intSym + "\"(" + l.ssa + ", " + r.ssa +
                 ") : (i64, i64) -> " + aop->intRet + " " + opLoc());
            return Value{aop->intRet[0] == 'f' ? VK::Float : VK::Int, ssa};
        }

        // ---- 浮点路径：arith 直降或 lang op（promote 后 emit） ----
        Value lf = promoteF64(l), rf = promoteF64(r);
        std::string ssa = fresh();
        if (aop->fSym)
            emit(ssa + " = \"" + aop->fSym + "\"(" + lf.ssa + ", " + rf.ssa +
                 ") : (f64, f64) -> f64 " + opLoc());
        else
            emit(ssa + " = " + aop->fArith + " " + lf.ssa + ", " + rf.ssa +
                 " : f64");
        return Value{VK::Float, ssa};
    }

    unsupported(range, "运算符 '" + op + "'");
}

Value Gen::genTernary(const Ternary& t) {
    VK vk = inferVK(t);
    if (vk == VK::Bad || vk == VK::Void || vk == VK::None)
        unsupported(t.range, "三元表达式分支类型");
    Value cond = genExpr(*t.cond);
    Slot slot = allocSlot(vk);
    uint8_t thenElem = 0, elseElem = 0;
    int thenB = newBlock(), elseB = newBlock(), joinB = newBlock();
    terminate("cf.cond_br " + cond.ssa + ", " + blk(thenB).label + ", " +
              blk(elseB).label);
    cur_ = thenB;
    {
        Value v = genExpr(*t.thenE);
        thenElem = v.lElem;
        storeTo(slot, v);
    }
    terminate("cf.br " + blk(joinB).label);
    cur_ = elseB;
    {
        Value v = genExpr(*t.elseE);
        elseElem = v.lElem;
        storeTo(slot, v);
    }
    terminate("cf.br " + blk(joinB).label);
    cur_ = joinB;
    // 静态模型一致性：两分支列表元素 VK 不一致时槽位单值记录会失真
    //（运行时 etag 与静态分派不符 → 内存误读），显式拒绝。
    if (vk == VK::List && thenElem != elseElem)
        unsupported(t.range, "三元表达式分支列表元素类型不一致");
    return loadSlot(slot);
}

// ============================================================================
// 调用
// ============================================================================

Value Gen::genCall(const Call& c) {
    // callee: Name（内建 / 本模块函数）或 Attr（mod.f 跨模块）。
    if (c.callee->kind == Expr::Kind::Name) {
        auto& n = static_cast<const Name&>(*c.callee);
        // 内建优先（与检查器/字节码后端同序；变量遮蔽场景检查器已报错）。
        if (isBuiltinName(n.name)) return genBuiltinCall(n.name, c);

        auto ft = s_.registry().find(curModule_);
        if (ft != s_.registry().end()) {
            auto f = ft->second->funcs.find(n.name);
            if (f != ft->second->funcs.end()) {
                std::vector<Value> args;
                std::vector<VK> paramVKs;
                for (const auto& t : f->second.params)
                    paramVKs.push_back(vkOfType(t));
                if (c.args.size() != paramVKs.size())
                    unsupported(c.range, "函数 '" + n.name + "' 实参数量不符");
                for (const auto& a : c.args) args.push_back(genExpr(*a));
                return emitCallRet(funcSym(curModule_, n.name), args, paramVKs,
                                   vkOfType(f->second.ret), f->second.ret);
            }
        }
        unsupported(c.range, "调用未解析的函数 '" + n.name + "'");
    }
    if (c.callee->kind == Expr::Kind::Attr) {
        auto& at = static_cast<const Attr&>(*c.callee);
        // Result 消费方法（is_ok / unwrap / unwrap_or）：载荷 VK 由检查器
        // 类型表（ExprTypeMap）反查，self 先 args 后（与 VM CallMethod 一致）。
        if (auto bt = types_.find(at.base.get());
            bt != types_.end() && bt->second &&
            bt->second->kind == Type::Kind::Result) {
            const TypePtr& rt = bt->second;
            VK pv = rt->params.empty() ? VK::Bad : vkOfType(rt->params[0]);
            if (at.name == "is_ok") {
                if (!c.args.empty())
                    unsupported(c.range, "is_ok() 不需要参数");
                Value self = genExpr(*at.base);
                std::string ssa = fresh();
                emit(ssa + " = \"lang.result_is_ok\"(" + self.ssa +
                     ") : (!llvm.ptr) -> i1");
                return Value{VK::Bool, ssa};
            }
            if (at.name == "unwrap" || at.name == "unwrap_or") {
                if (c.args.size() != 1)
                    unsupported(c.range,
                                at.name + "() 需要 1 个缺省值参数");
                Value self = genExpr(*at.base);
                if (pv == VK::None)
                    return Value{VK::None, ""}; // None 载荷无 SSA；ok/err 均取 None
                if (pv != VK::Int && pv != VK::Float && pv != VK::Bool &&
                    pv != VK::Str)
                    unsupported(c.range, "该载荷类型的 " + at.name + "()");
                Value def = genExpr(*c.args[0]);
                // 缺省值 int → float 载荷（与 emitCallRet 隐式转换一致）
                if (pv == VK::Float && def.vk == VK::Int) {
                    std::string f = fresh();
                    emit(f + " = arith.sitofp " + def.ssa + " : i64 to f64");
                    def = Value{VK::Float, f};
                }
                const char* sym = pv == VK::Int   ? "lang.result_unwrap_i"
                                  : pv == VK::Float ? "lang.result_unwrap_f"
                                  : pv == VK::Bool  ? "lang.result_unwrap_b"
                                                    : "lang.result_unwrap_s";
                const char* retTy =
                    pv == VK::Int ? "i64"
                    : pv == VK::Float ? "f64"
                    : pv == VK::Bool  ? "i1"
                                      : "!llvm.ptr";
                std::string ssa = fresh();
                emit(ssa + " = \"" + sym + "\"(" + self.ssa + ", " + def.ssa +
                     ") : (!llvm.ptr, " + std::string(mlirTy(pv)) + ") -> " +
                     retTy);
                return Value{pv, ssa};
            }
        }
        if (at.base->kind == Expr::Kind::Name) {
            auto& mn = static_cast<const Name&>(*at.base);
            if (std::find(curImports_.begin(), curImports_.end(), mn.name) !=
                curImports_.end()) {
                auto mt = s_.registry().find(mn.name);
                if (mt == s_.registry().end())
                    unsupported(c.range, "模块 '" + mn.name + "' 不在符号表中");
                auto f = mt->second->funcs.find(at.name);
                if (f == mt->second->funcs.end())
                    unsupported(c.range,
                                "模块 '" + mn.name + "' 无函数 '" + at.name + "'");
                std::vector<Value> args;
                std::vector<VK> paramVKs;
                for (const auto& t : f->second.params)
                    paramVKs.push_back(vkOfType(t));
                if (c.args.size() != paramVKs.size())
                    unsupported(c.range, "函数 '" + mn.name + "." + at.name +
                                             "' 实参数量不符");
                for (const auto& a : c.args) args.push_back(genExpr(*a));
                return emitCallRet(funcSym(mn.name, at.name), args, paramVKs,
                                   vkOfType(f->second.ret), f->second.ret);
            }
        }
        unsupported(c.range, "Result 等方法调用（is_ok / unwrap / unwrap_or）");
    }
    unsupported(c.range, "该调用形式");
}

Value Gen::genBuiltinCall(const std::string& name, const Call& c) {
    auto need = [&](size_t want) {
        if (c.args.size() != want) unsupported(c.range, "内建 " + name + "() 参数个数");
    };

    if (name == "print") {
        std::vector<Value> args;
        for (const auto& a : c.args) args.push_back(genExpr(*a));
        for (size_t k = 0; k < args.size(); ++k) {
            if (k) emit("\"lang.print_space\"() : () -> ()");
            const Value& v = args[k];
            switch (v.vk) {
            case VK::Int:
                emit("\"lang.print_i64\"(" + v.ssa + ") : (i64) -> ()");
                break;
            case VK::Float:
                emit("\"lang.print_f64\"(" + v.ssa + ") : (f64) -> ()");
                break;
            case VK::Bool:
                emit("\"lang.print_bool\"(" + v.ssa + ") : (i1) -> ()");
                break;
            case VK::Str:
                emit("\"lang.print_str\"(" + v.ssa + ") : (!llvm.ptr) -> ()");
                break;
            case VK::None:
                emit("\"lang.print_none\"() : () -> ()");
                break;
            case VK::Tensor:
                emit("\"lang.print_tensor\"(" + v.ssa + ") : (!llvm.ptr) -> ()");
                break;
            default:
                unsupported(c.range, "print 该参数类型");
            }
        }
        emit("\"lang.print_nl\"() : () -> ()");
        return Value{VK::None, ""};
    }
    if (name == "str") {
        need(1);
        Value v = genExpr(*c.args[0]);
        switch (v.vk) {
        case VK::Int: {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_from_i64\"(" + v.ssa +
                 ") : (i64) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        }
        case VK::Float: {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_from_f64\"(" + v.ssa +
                 ") : (f64) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        }
        case VK::Bool: {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_from_bool\"(" + v.ssa +
                 ") : (i1) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        }
        case VK::Str:
            return v; // VM stringify(Str) 原样返回
        case VK::Tensor: {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_from_tensor\"(" + v.ssa +
                 ") : (!llvm.ptr) -> !llvm.ptr");
            return Value{VK::Str, ssa};
        }
        default:
            unsupported(c.args[0]->range, "str() 该参数类型");
        }
    }
    if (name == "len") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk == VK::Str) {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.str_len\"(" + v.ssa + ") : (!llvm.ptr) -> i64");
            return Value{VK::Int, ssa};
        }
        if (v.vk == VK::List) {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.list_len\"(" + v.ssa + ") : (!llvm.ptr) -> i64");
            return Value{VK::Int, ssa};
        }
        unsupported(c.args[0]->range, "len() 仅支持 str 与列表");
    }
    if (name == "now") {
        std::string ssa = fresh();
        emit(ssa + " = \"lang.now\"() : () -> f64");
        return Value{VK::Float, ssa};
    }
    if (name == "range") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Int) unsupported(c.args[0]->range, "range() 参数必须是 int");
        return Value{VK::Range, v.ssa}; // 负数原样存（首轮即结束）
    }
    // 一元数学内建（kUnOps 注册表驱动）
    if (const UnInfo* uop = findUnOp(name)) {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk == VK::Tensor) {
            // 逐元素：fnid 查表；fnid 0 = abs 保 dtype，其余恒 f64
            std::string ssa = fresh();
            emit(ssa + " = \"lang.tensor_un\"(" + i32Const(uop->fnid) + ", " +
                 v.ssa + ") : (i32, !llvm.ptr) -> !llvm.ptr " + opLoc());
            return Value{VK::Tensor, ssa, uop->fnid == 0 && v.tInt, v.tRank};
        }
        if (v.vk != VK::Int && v.vk != VK::Float)
            unsupported(c.args[0]->range, name + "() 仅支持数值");
        if (uop->iSym) { // abs：双路径保 dtype，不提升不挂 opLoc
            std::string ssa = fresh();
            if (v.vk == VK::Int) {
                emit(ssa + " = \"" + uop->iSym + "\"(" + v.ssa +
                     ") : (i64) -> i64");
                return Value{VK::Int, ssa};
            }
            emit(ssa + " = \"" + uop->uSym + "\"(" + v.ssa + ") : (f64) -> f64");
            return Value{VK::Float, ssa};
        }
        // sqrt/exp/log：提升 f64
        Value vf = promoteF64(v);
        std::string ssa = fresh();
        emit(ssa + " = \"" + uop->uSym + "\"(" + vf.ssa + ") : (f64) -> f64 " +
             opLoc());
        return Value{VK::Float, ssa};
    }
    if (name == "sum") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Tensor)
            unsupported(c.args[0]->range, "sum() 仅支持张量");
        std::string ssa = fresh();
        if (v.tInt) {
            emit(ssa + " = \"lang.tensor_sum_i\"(" + v.ssa +
                 ") : (!llvm.ptr) -> i64 " + opLoc());
            return Value{VK::Int, ssa};
        }
        // linalg 覆盖集：f64 + 形状静态 → 写形状属性（否则 LangToLLVM 兜底）
        std::string attrs;
        if (auto sh = staticF64Shape(c.args[0].get()))
            attrs = " {shape = " + shapeArrAttr(*sh) + "}";
        emit(ssa + " = \"lang.tensor_sum_f\"(" + v.ssa + ")" + attrs +
             " : (!llvm.ptr) -> f64 " + opLoc());
        return Value{VK::Float, ssa};
    }
    if (name == "max" || name == "min") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Tensor)
            unsupported(c.args[0]->range, name + "() 仅支持张量");
        Value nm = strConst(name);
        std::string ssa = fresh();
        if (v.tInt) {
            emit(ssa + " = \"lang.tensor_reduce_i\"(" +
                 i32Const(name == "max" ? 1 : 0) + ", " + v.ssa + ", " + nm.ssa +
                 ") : (i32, !llvm.ptr, !llvm.ptr) -> i64 " + opLoc());
            return Value{VK::Int, ssa};
        }
        emit(ssa + " = \"lang.tensor_reduce_f\"(" +
             i32Const(name == "max" ? 1 : 0) + ", " + v.ssa + ", " + nm.ssa +
             ") : (i32, !llvm.ptr, !llvm.ptr) -> f64 " + opLoc());
        return Value{VK::Float, ssa};
    }
    if (name == "transpose") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Tensor)
            unsupported(c.args[0]->range, "transpose() 仅支持张量");
        std::string ssa = fresh();
        // 张量降级覆盖集：2D int/float + 形状静态 → 写形状属性（纯数据
        // 搬移与 dtype 无关；int 经 t_int 属性分派 i64 视图与句柄）
        std::string attrs;
        if (v.tRank == 2) {
            if (auto sh = staticShapeOf(c.args[0].get()))
                attrs = " {shape = " + shapeArrAttr(*sh) +
                        ", t_int = " + std::to_string(v.tInt ? 1 : 0) +
                        " : i32}";
        }
        emit(ssa + " = \"lang.tensor_transpose\"(" + v.ssa + ")" + attrs +
             " : (!llvm.ptr) -> !llvm.ptr " + opLoc());
        return Value{VK::Tensor, ssa, v.tInt, 2};
    }
    if (name == "reshape") {
        // 变参维度：reshape(t, d1, d2, ...)（与 VM doBuiltin 一致）
        if (c.args.size() < 2)
            unsupported(c.range, "reshape() 需要张量 + 至少 1 个维度");
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Tensor)
            unsupported(c.args[0]->range, "reshape() 首参必须是张量");
        std::vector<Value> dims;
        for (size_t k = 1; k < c.args.size(); ++k) {
            Value d = genExpr(*c.args[k]);
            if (d.vk != VK::Int)
                unsupported(c.args[k]->range, "reshape() 维度必须是 int");
            dims.push_back(d);
        }
        std::string arr = buildI64Array(dims);
        std::string ssa = fresh();
        emit(ssa + " = \"lang.tensor_reshape\"(" + v.ssa + ", " +
             intConst((int64_t)dims.size()) + ", " + arr +
             ") : (!llvm.ptr, i64, !llvm.ptr) -> !llvm.ptr " + opLoc());
        return Value{VK::Tensor, ssa, v.tInt,
                     static_cast<uint8_t>(dims.size())};
    }
    // ok / err（Result 构造；检查器已强制恰好 1 参）
    if (name == "ok") {
        need(1);
        Value v = genExpr(*c.args[0]);
        std::string ssa = fresh();
        switch (v.vk) {
        case VK::Int:
            emit(ssa + " = \"lang.result_ok_i\"(" + v.ssa +
                 ") : (i64) -> !llvm.ptr");
            break;
        case VK::Float:
            emit(ssa + " = \"lang.result_ok_f\"(" + v.ssa +
                 ") : (f64) -> !llvm.ptr");
            break;
        case VK::Bool:
            emit(ssa + " = \"lang.result_ok_b\"(" + v.ssa +
                 ") : (i1) -> !llvm.ptr");
            break;
        case VK::Str:
            emit(ssa + " = \"lang.result_ok_s\"(" + v.ssa +
                 ") : (!llvm.ptr) -> !llvm.ptr");
            break;
        case VK::None:
            emit(ssa + " = \"lang.result_ok_n\"() : () -> !llvm.ptr");
            break;
        default:
            unsupported(c.args[0]->range, "ok() 该载荷类型");
        }
        return Value{VK::Result, ssa, false, 0, static_cast<uint8_t>(v.vk)};
    }
    if (name == "err") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk != VK::Str)
            unsupported(c.args[0]->range, "err() 参数必须是 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.result_err\"(" + v.ssa +
             ") : (!llvm.ptr) -> !llvm.ptr");
        return Value{VK::Result, ssa, false, 0, static_cast<uint8_t>(VK::Str)};
    }
    unsupported(c.range, "内建 " + name + "()（该内建）");
}

// ============================================================================
// 语句
// ============================================================================

void Gen::genStmtList(const StmtList& list) {
    for (const auto& s : list) genStmt(*s);
}

void Gen::genStmt(const Stmt& s) {
    line_ = lineOf(s.range);
    switch (s.kind) {
    case Stmt::Kind::Assign:
        genAssign(static_cast<const Assign&>(s));
        break;
    case Stmt::Kind::AugAssign:
        genAugAssign(static_cast<const AugAssign&>(s));
        break;
    case Stmt::Kind::ExprStmt:
        genExpr(*static_cast<const ExprStmt&>(s).expr);
        break;
    case Stmt::Kind::If: {
        auto& st = static_cast<const If&>(s);
        genIf(*st.cond, st.thenBody,
              st.elifs.empty() ? nullptr : &st.elifs,
              st.elseBody.empty() ? nullptr : &st.elseBody);
        break;
    }
    case Stmt::Kind::While:
        genWhile(static_cast<const While&>(s));
        break;
    case Stmt::Kind::For:
        genFor(static_cast<const For&>(s));
        break;
    case Stmt::Kind::Return: {
        auto& st = static_cast<const Return&>(s);
        if (curIsTop_) unsupported(s.range, "顶层 return");
        if (!st.value) {
            popTriesAbove(0); // return 穿越全部激活 try（对齐 VM doReturn 清理）
            terminate("func.return");
            break;
        }
        Value v = genExpr(*st.value);
        if (curRetVK_ == VK::None || curRetVK_ == VK::Void) {
            popTriesAbove(0); // return 穿越全部激活 try（对齐 VM doReturn 清理）
            terminate("func.return");
            break;
        }
        if (curRetVK_ == VK::Float && v.vk == VK::Int) {
            std::string ssa = fresh();
            emit(ssa + " = arith.sitofp " + v.ssa + " : i64 to f64");
            v = Value{VK::Float, ssa};
        }
        popTriesAbove(0); // return 穿越全部激活 try（对齐 VM doReturn 清理）
        terminate("func.return " + v.ssa + " : " + mlirTy(curRetVK_));
        break;
    }
    case Stmt::Kind::Break: {
        if (loopStack_.empty()) unsupported(s.range, "break 不在循环内");
        popTriesAbove(loopStack_.back().tryDepth); // 显式 ExitTry 穿越（VM 同构）
        terminate("cf.br " + loopStack_.back().brk);
        break;
    }
    case Stmt::Kind::Continue: {
        if (loopStack_.empty()) unsupported(s.range, "continue 不在循环内");
        popTriesAbove(loopStack_.back().tryDepth);
        terminate("cf.br " + loopStack_.back().cont);
        break;
    }
    case Stmt::Kind::Pass:
        break;
    case Stmt::Kind::Assert:
        genAssert(static_cast<const Assert&>(s));
        break;
    case Stmt::Kind::Try:
        genTry(static_cast<const Try&>(s));
        break;
    case Stmt::Kind::Import:
    case Stmt::Kind::FuncDef:
        break; // 顶层处理；嵌套定义与字节码后端一致地忽略
    }
}

void Gen::genAssign(const Assign& a) {
    // 张量下标写：base → 下标… → value（与 Compiler 的 StoreIndex 求值序一致）
    if (a.target->kind == Expr::Kind::Index) {
        auto& ix = static_cast<const Index&>(*a.target);
        Value base = genExpr(*ix.base);
        if (base.vk != VK::Tensor)
            unsupported(ix.base->range, "下标赋值仅支持张量");
        std::vector<Value> idxs;
        for (const auto& ie : ix.indices) {
            Value iv = genExpr(*ie);
            if (iv.vk != VK::Int)
                unsupported(ie->range, "张量下标必须是 int");
            idxs.push_back(iv);
        }
        Value v = genExpr(*a.value);
        std::string arr = buildI64Array(idxs);
        if (base.tInt) {
            if (v.vk == VK::Float)
                unsupported(a.value->range, "浮点值无法赋给整型张量元素");
            if (v.vk == VK::Bool) {
                std::string z = fresh();
                emit(z + " = arith.extui " + v.ssa + " : i1 to i64");
                v = Value{VK::Int, z};
            }
            emit("\"lang.tensor_set_i\"(" + base.ssa + ", " +
                 intConst((int64_t)idxs.size()) + ", " + arr + ", " + v.ssa +
                 ") : (!llvm.ptr, i64, !llvm.ptr, i64) -> () " + opLoc());
        } else {
            if (v.vk == VK::Int) {
                std::string f = fresh();
                emit(f + " = arith.sitofp " + v.ssa + " : i64 to f64");
                v = Value{VK::Float, f};
            } else if (v.vk == VK::Bool) {
                std::string f = fresh();
                emit(f + " = arith.uitofp " + v.ssa + " : i1 to f64");
                v = Value{VK::Float, f};
            }
            emit("\"lang.tensor_set_f\"(" + base.ssa + ", " +
                 intConst((int64_t)idxs.size()) + ", " + arr + ", " + v.ssa +
                 ") : (!llvm.ptr, i64, !llvm.ptr, f64) -> () " + opLoc());
        }
        return;
    }
    if (a.target->kind != Expr::Kind::Name)
        unsupported(a.target->range, "该赋值目标");
    auto& n = static_cast<const Name&>(*a.target);
    Value v = genExpr(*a.value);

    if (curIsTop_) {
        auto gt = curGlobals_->find(n.name);
        if (gt == curGlobals_->end())
            unsupported(a.target->range, "全局变量 '" + n.name + "' 未登记");
        storeGlobal(gt->second, v);
        return;
    }

    // 渐进式局部槽：新建时按注解类型（其次值类型）确定 VK。
    auto lt = locals_.find(n.name);
    if (lt == locals_.end()) {
        if (a.ann && a.ann->kind == TypeExpr::Kind::Tensor) {
            // 张量注解：dtype/rank 从注解回填（shape 维度数即秩；
            // 缺省形状时秩为 0，由首次 store 修正）
            Slot slot = allocSlot(VK::Tensor);
            if (!a.ann->params.empty() &&
                a.ann->params[0]->kind == TypeExpr::Kind::Named)
                slot.tInt = a.ann->params[0]->name != "float";
            slot.tRank = static_cast<uint8_t>(a.ann->shape.size());
            lt = locals_.emplace(n.name, slot).first;
            storeTo(lt->second, v);
            return;
        }
        VK vk;
        if (a.ann) {
            vk = a.ann->kind == TypeExpr::Kind::Named ? vkOfAnnName(a.ann->name)
                                                      : VK::Bad;
            if (vk == VK::Bad)
                unsupported(a.ann->range, "变量注解类型 '" + a.ann->name + "'");
        } else {
            vk = v.vk;
        }
        if (vk == VK::Bad || vk == VK::Void)
            unsupported(a.value->range, "变量 '" + n.name + "' 的类型");
        Slot slot = allocSlot(vk);
        if (vk == VK::List && a.ann && a.ann->params.size() == 1 &&
            a.ann->params[0]->kind == TypeExpr::Kind::Named) {
            // List[T] 注解：元素 VK 从注解参数回填（storeTo 亦会修正）
            VK ev = vkOfAnnName(a.ann->params[0]->name);
            if (ev != VK::Bad) slot.lElem = static_cast<uint8_t>(ev);
        }
        lt = locals_.emplace(n.name, slot).first;
    }
    storeTo(lt->second, v);
}

void Gen::genAugAssign(const AugAssign& a) {
    if (a.target->kind != Expr::Kind::Name)
        unsupported(a.target->range, "下标复合赋值");
    auto& n = static_cast<const Name&>(*a.target);
    std::string op = a.op;
    if (!op.empty() && op.back() == '=') op.pop_back(); // "+=" → "+"

    Value r = genExpr(*a.value);

    // 渐进式语义：先局部后全局；都不在 → 检查器已报 E2003（防御性报错）。
    Slot slot;
    bool isLocal = false;
    GlobalSlot g;
    auto lt = locals_.find(n.name);
    if (lt != locals_.end()) {
        slot = lt->second;
        isLocal = true;
    } else {
        auto gt = curGlobals_->find(n.name);
        if (gt == curGlobals_->end())
            unsupported(a.target->range, "未声明的标识符 '" + n.name + "'");
        g = gt->second;
    }
    VK slotVK = isLocal ? slot.vk : g.vk;

    // 复合赋值结果类型必须与槽一致（VM 槽动态类型允许变型，AOT 静态模型
    // 显式拒绝；与"张量静默降级禁止"同一纪律）。
    if (slotVK == VK::Str) {
        if (!(op == "+" && r.vk == VK::Str))
            unsupported(a.range, "字符串复合赋值仅支持 '+='");
        Value l = isLocal ? loadSlot(slot) : loadGlobal(curModule_, n.name);
        Value v = genBinary("+", l, r, a.range, nullptr);
        if (isLocal) storeTo(slot, v);
        else storeGlobal(g, v);
        return;
    }
    if (slotVK == VK::Tensor) {
        Value l = isLocal ? loadSlot(slot) : loadGlobal(curModule_, n.name);
        // 目标标识符表达式供 staticF64Shape 查 ExprTypeMap（lExpr 参数）
        Value v = genBinary(op, l, r, a.range, nullptr, a.target.get());
        if (isLocal) storeTo(slot, v);
        else storeGlobal(g, v);
        return;
    }
    if (!isNumVK(slotVK)) unsupported(a.range, "复合赋值该变量类型");
    if (op == "/" || r.vk == VK::Float) {
        if (slotVK != VK::Float)
            unsupported(a.range, "复合赋值 '" + a.op + "' 会改变 '" + n.name +
                                     "' 的静态类型（结果为 float）");
    }
    Value l = isLocal ? loadSlot(slot) : loadGlobal(curModule_, n.name);
    Value v = genBinary(op, l, r, a.range, nullptr);
    if (isLocal) storeTo(slot, v);
    else storeGlobal(g, v);
}

void Gen::genIf(const Expr& cond, const StmtList& thenBody,
                const std::vector<If::Elif>* elifs, const StmtList* elseBody) {
    Value c = genExpr(cond);
    int thenB = newBlock();
    if (!elifs && !elseBody) {
        int endB = newBlock();
        terminate("cf.cond_br " + c.ssa + ", " + blk(thenB).label + ", " +
                  blk(endB).label);
        cur_ = thenB;
        genStmtList(thenBody);
        terminate("cf.br " + blk(endB).label);
        cur_ = endB;
        return;
    }
    int elseB = newBlock(), endB = newBlock();
    terminate("cf.cond_br " + c.ssa + ", " + blk(thenB).label + ", " +
              blk(elseB).label);
    cur_ = thenB;
    genStmtList(thenBody);
    terminate("cf.br " + blk(endB).label);
    cur_ = elseB;
    if (elifs) {
        // elif 链：迭代展开（尾部 else 归入最后一环）。
        for (size_t k = 0; k < elifs->size(); ++k) {
            const If::Elif& e = (*elifs)[k];
            Value ec = genExpr(*e.cond);
            int eBody = newBlock(), nextB = newBlock();
            terminate("cf.cond_br " + ec.ssa + ", " + blk(eBody).label + ", " +
                      blk(nextB).label);
            cur_ = eBody;
            genStmtList(e.body);
            terminate("cf.br " + blk(endB).label);
            cur_ = nextB;
        }
        if (elseBody) genStmtList(*elseBody);
        terminate("cf.br " + blk(endB).label);
    } else {
        genStmtList(*elseBody);
        terminate("cf.br " + blk(endB).label);
    }
    cur_ = endB;
}

void Gen::genWhile(const While& w) {
    int condB = newBlock();
    terminate("cf.br " + blk(condB).label);
    cur_ = condB;
    Value c = genExpr(*w.cond);
    int bodyB = newBlock(), endB = newBlock();
    terminate("cf.cond_br " + c.ssa + ", " + blk(bodyB).label + ", " +
              blk(endB).label);
    cur_ = bodyB;
    loopStack_.push_back(LoopCtx{blk(endB).label, blk(condB).label, tryStack_.size()});
    genStmtList(w.body);
    loopStack_.pop_back();
    terminate("cf.br " + blk(condB).label); // 回边重新求值条件
    cur_ = endB;
}

void Gen::genFor(const For& f) {
    // range 迭代 + List 迭代（List 值：字面量 / 函数返回，均已有静态元素 VK）
    Value src = genExpr(*f.iter);
    Slot counter = allocSlot(VK::Int);
    Slot varSlot;
    if (src.vk == VK::Range) {
        varSlot = allocSlot(VK::Int);
        locals_[f.var] = varSlot;
        storeTo(counter, Value{VK::Int, zeroI64()});

        int condB = newBlock(), incrB = newBlock();
        terminate("cf.br " + blk(condB).label);
        cur_ = condB;
        std::string c = fresh();
        emit(c + " = llvm.load" + vol() + " " + counter.ptr +
             " : !llvm.ptr -> i64");
        std::string cont = fresh();
        emit(cont + " = arith.cmpi slt, " + c + ", " + src.ssa + " : i64");
        int bodyB = newBlock(), endB = newBlock();
        terminate("cf.cond_br " + cont + ", " + blk(bodyB).label + ", " +
                  blk(endB).label);
        cur_ = bodyB;
        storeTo(varSlot, Value{VK::Int, c});
        // continue 目标 = 独立增量块（增量绝不能放 body 尾单一路径——
        // continue 跳过 body 尾直落条件块会让计数器永不递增，死循环）。
        loopStack_.push_back(
            LoopCtx{blk(endB).label, blk(incrB).label, tryStack_.size()});
        genStmtList(f.body);
        loopStack_.pop_back();
        terminate("cf.br " + blk(incrB).label);
        cur_ = incrB;
        // 计数器 +1（body 以 return 终止时，后续 emit 自动落入死代码丢弃）。
        std::string c2 = fresh();
        emit(c2 + " = llvm.load" + vol() + " " + counter.ptr +
             " : !llvm.ptr -> i64");
        std::string c3 = fresh();
        emit(c3 + " = arith.addi " + c2 + ", " + oneReg() + " : i64");
        storeTo(counter, Value{VK::Int, c3});
        terminate("cf.br " + blk(condB).label);
        cur_ = endB;
        return;
    }
    if (src.vk != VK::List)
        unsupported(f.iter->range, "for 迭代源仅支持 range 与列表");
    // List 迭代：0..len-1 计数 + 逐轮 list_get（越界不可达；与 VM ForNext
    // 的「元素 + true / 仅 false」两值语义等价——循环变量即元素）
    VK ev = static_cast<VK>(src.lElem);
    if (ev != VK::Int && ev != VK::Float && ev != VK::Bool && ev != VK::Str)
        unsupported(f.iter->range, "该元素类型的列表 for 迭代");
    varSlot = allocSlot(ev);
    locals_[f.var] = varSlot;
    storeTo(counter, Value{VK::Int, zeroI64()});

    int condB = newBlock(), incrB = newBlock();
    terminate("cf.br " + blk(condB).label);
    cur_ = condB;
    std::string c = fresh();
    emit(c + " = llvm.load" + vol() + " " + counter.ptr +
         " : !llvm.ptr -> i64");
    std::string len = fresh();
    emit(len + " = \"lang.list_len\"(" + src.ssa + ") : (!llvm.ptr) -> i64");
    std::string cont = fresh();
    emit(cont + " = arith.cmpi slt, " + c + ", " + len + " : i64");
    int bodyB = newBlock(), endB = newBlock();
    terminate("cf.cond_br " + cont + ", " + blk(bodyB).label + ", " +
              blk(endB).label);
    cur_ = bodyB;
    const char* getSym = ev == VK::Int   ? "lang.list_get_i"
                         : ev == VK::Float ? "lang.list_get_f"
                         : ev == VK::Bool  ? "lang.list_get_b"
                                           : "lang.list_get_s";
    const char* elemTy = ev == VK::Int   ? "i64"
                         : ev == VK::Float ? "f64"
                         : ev == VK::Bool  ? "i1"
                                           : "!llvm.ptr";
    std::string el = fresh();
    emit(el + " = \"" + getSym + "\"(" + src.ssa + ", " + c + ") : (!llvm.ptr, i64) -> " +
         elemTy);
    storeTo(varSlot, Value{ev, el});
    // continue 目标 = 独立增量块（同 range 分支——continue 必须经增量再回条件）。
    loopStack_.push_back(
        LoopCtx{blk(endB).label, blk(incrB).label, tryStack_.size()});
    genStmtList(f.body);
    loopStack_.pop_back();
    terminate("cf.br " + blk(incrB).label);
    cur_ = incrB;
    std::string c2 = fresh();
    emit(c2 + " = llvm.load" + vol() + " " + counter.ptr +
         " : !llvm.ptr -> i64");
    std::string c3 = fresh();
    emit(c3 + " = arith.addi " + c2 + ", " + oneReg() + " : i64");
    storeTo(counter, Value{VK::Int, c3});
    terminate("cf.br " + blk(condB).label);
    cur_ = endB;
}

void Gen::genAssert(const Assert& a) {
    // 求值顺序与 VM 一致：cond → msg（msg 不论断言成败都求值）→ 判定。
    Value cond = genExpr(*a.cond);
    Value msg;
    bool hasMsg = false;
    if (a.msg) {
        msg = genExpr(*a.msg);
        hasMsg = true;
    }
    int okB = newBlock(), panicB = newBlock();
    terminate("cf.cond_br " + cond.ssa + ", " + blk(okB).label + ", " +
              blk(panicB).label);
    cur_ = panicB;
    if (hasMsg) {
        Value prefix = strConst("断言失败：");
        std::string m = fresh();
        emit(m + " = \"lang.str_concat\"(" + prefix.ssa + ", " + msg.ssa +
             ") : (!llvm.ptr, !llvm.ptr) -> !llvm.ptr " + opLoc());
        emit("\"lang.panic\"(" + m + ") : (!llvm.ptr) -> () " + opLoc());
    } else {
        Value m = strConst("断言失败");
        emit("\"lang.panic\"(" + m.ssa + ") : (!llvm.ptr) -> () " + opLoc());
    }
    terminate("llvm.unreachable"); // 不可达兜底（sg_panic 不返回；无值 return
                                   // 在非 void 函数中不合法——llvm.unreachable
                                   // 语义精确且对任意返回类型成立）
    cur_ = okB;
}

// ============================================================================
// try / except（panic 恢复）
// ============================================================================

void Gen::popTriesAbove(size_t depth) {
    for (size_t k = tryStack_.size(); k-- > depth;)
        emit("func.call @sg_try_pop(" + tryStack_[k] + ") : (!llvm.ptr) -> ()");
    tryStack_.resize(depth);
}

void Gen::genTry(const Try& t) {
    // 蹦床：sg_try_push 取 handler 句柄 → 直调 CRT @_setjmp 双返回（0=直落
    // body，非 0=panic longjmp 到 handler）。setjmp 必须在生成函数自身活跃
    // 帧调用——包装函数方案下 longjmp 恢复的包装帧会被 try body 内同调用
    // 深度的兄弟调用覆盖，恢复到死帧。SgTry.jb 为首成员，句柄即 jmp_buf*。
    // x64 上 clang 前端对 setjmp 有 intrinsic 特判：@_setjmp 实为双参
    // (jmp_buf, frameaddress(0))，这里复刻该 ABI（@llvm.frameaddress.p0 经
    // 声明调用，LLVM 后端按 intrinsic 展开）。处理器栈簿记与 VM catches_
    // 同构：正常路径 body 尾 sg_try_pop；panic 路径由 longjmp 落 handler
    // 块，tryStack_ 以入口快照恢复（body 内 break/return 可能已弹走部分层）。
    line_ = lineOf(t.range);
    tryDeclsEmitted_ = true; // assemble 时补插蹦床声明（幂等，仅一次）
    frameHasSetjmp_ = true;  // 本函数槽位读写自此全部 volatile（跨 longjmp 存活）
    std::string h = fresh();
    emit(h + " = func.call @sg_try_push() : () -> !llvm.ptr");
    std::vector<std::string> entryStack = tryStack_;
    tryStack_.push_back(h);
    std::string r = fresh();
    std::string fa = fresh();
    emit(fa + " = func.call @llvm.frameaddress.p0(" + i32Const(0) +
         ") : (i32) -> !llvm.ptr");
    emit(r + " = func.call @_setjmp(" + h + ", " + fa +
         ") : (!llvm.ptr, !llvm.ptr) -> i32");
    std::string nz = fresh();
    emit(nz + " = arith.cmpi ne, " + r + ", " + i32Const(0) + " : i32");
    int bodyB = newBlock(), catchB = newBlock(), joinB = newBlock();
    terminate("cf.cond_br " + nz + ", " + blk(catchB).label + ", " +
              blk(bodyB).label);
    cur_ = bodyB;
    genStmtList(t.body);
    if (!tryStack_.empty() && tryStack_.back() == h) {
        // 正常路径 ExitTry（body 以 return/break 终止时 emit 已死、簿记已弹）
        emit("func.call @sg_try_pop(" + h + ") : (!llvm.ptr) -> ()");
        tryStack_.pop_back();
    }
    terminate("cf.br " + blk(joinB).label);
    cur_ = catchB;
    // longjmp 消费了 h（sg_panic 不弹栈，故 handler 首行补 sg_try_pop）；
    // 簿记恢复 = try 入口状态（本身即不含 h，无需再 pop）。
    tryStack_ = entryStack;
    emit("func.call @sg_try_pop(" + h + ") : (!llvm.ptr) -> ()");
    genStmtList(t.handlerBody);
    terminate("cf.br " + blk(joinB).label);
    cur_ = joinB;
}

// ============================================================================
// 模块级
// ============================================================================

void Gen::precollectGlobals() {
    // 遍历全部模块：登记全局槽并发射零值定义；函数签名 VK 预检。
    for (const auto& name : s_.modulesInDependencyOrder(entry_)) {
        const Module* m = s_.loadedModule(name);
        auto syms = s_.registry().find(name);
        if (!m || syms == s_.registry().end()) continue;
        curFile_ = s_.loadedFileId(name);
        auto& gm = globals_[name];

        for (const auto& stmt : m->body) {
            if (stmt->kind == Stmt::Kind::Assign) {
                auto& a = static_cast<const Assign&>(*stmt);
                if (a.target->kind != Expr::Kind::Name) continue;
                auto& n = static_cast<const Name&>(*a.target);
                auto gt = syms->second->globals.find(n.name);
                if (gt == syms->second->globals.end()) continue;
                VK vk = vkOfType(gt->second);
                if (vk == VK::Bad || vk == VK::Tensor || vk == VK::Result ||
                    vk == VK::List)
                    unsupported(a.target->range,
                                "全局变量 '" + name + "." + n.name + "' 的类型");
                gm.emplace(n.name, GlobalSlot{vk, globalSym(name, n.name)});
                switch (vk) {
                case VK::Int:
                case VK::Range:
                    globalDefs_.push_back("llvm.mlir.global internal " +
                                          globalSym(name, n.name) + "(0 : i64) : i64");
                    break;
                case VK::Float:
                    globalDefs_.push_back("llvm.mlir.global internal " +
                                          globalSym(name, n.name) + "(0.0 : f64) : f64");
                    break;
                case VK::Bool:
                    globalDefs_.push_back("llvm.mlir.global internal " +
                                          globalSym(name, n.name) + "(false) : i1");
                    break;
                case VK::Str: {
                    // LLVM dialect 中 ptr 全局的初始化器必须是同类型引用：
                    // 字面量初始值直接引用其字符串常量，否则引用空串占位
                    //（真正的赋值由 <top> 中的初始 store 完成）。
                    std::string init = internStr("");
                    if (a.value->kind == Expr::Kind::StrLit)
                        init = internStr(
                            static_cast<const StrLit&>(*a.value).strValue);
                    globalDefs_.push_back("llvm.mlir.global internal " +
                                          globalSym(name, n.name) + "(" + init +
                                          ") : !llvm.ptr");
                    break;
                }
                default:
                    break; // None：无存储
                }
            } else if (stmt->kind == Stmt::Kind::FuncDef) {
                auto& fd = static_cast<const FuncDef&>(*stmt);
                auto f = syms->second->funcs.find(fd.name);
                if (f == syms->second->funcs.end()) continue;
                if (fd.isAsync) unsupported(fd.nameRange, "async def");
                for (const auto& p : f->second.params)
                    if (vkOfType(p) == VK::Bad)
                        unsupported(fd.nameRange,
                                    "函数 '" + name + "." + fd.name + "' 的参数类型");
                if (vkOfType(f->second.ret) == VK::Bad)
                    unsupported(fd.nameRange,
                                "函数 '" + name + "." + fd.name + "' 的返回类型");
            }
        }
    }
}

void Gen::genModule(const std::string& name) {
    const Module* m = s_.loadedModule(name);
    if (!m) return;
    curModule_ = name;
    curFile_ = s_.loadedFileId(name);
    curFilePath_ = s_.loadedModulePath(name);
    curGlobals_ = &globals_[name];
    curImports_.clear();
    for (const auto& stmt : m->body)
        if (stmt->kind == Stmt::Kind::Import)
            curImports_.push_back(static_cast<const Import&>(*stmt).module);

    // 遍一：函数；遍二：<top> 顶层赋值（与字节码两遍同构）。
    for (const auto& stmt : m->body)
        if (stmt->kind == Stmt::Kind::FuncDef)
            genFunction(*m, static_cast<const FuncDef&>(*stmt));
    genTopAssigns(*m);
}

void Gen::genFunction(const Module& m, const FuncDef& f) {
    auto ft = s_.registry().find(m.name);
    if (ft == s_.registry().end()) return;
    auto sig = ft->second->funcs.find(f.name);
    if (sig == ft->second->funcs.end()) return;

    std::vector<std::pair<std::string, VK>> params;
    for (size_t k = 0; k < f.params.size(); ++k)
        params.emplace_back(f.params[k].name, vkOfType(sig->second.params[k]));

    // MLIR 签名（None 形参无 SSA 表示，从签名中剔除，与调用点一致）。
    std::string sigTys;
    {
        bool first = true;
        for (size_t k = 0; k < params.size(); ++k) {
            if (params[k].second == VK::None) continue;
            if (!first) sigTys += ", ";
            first = false;
            sigTys += "%a" + std::to_string(k) + ": " + mlirTy(params[k].second);
        }
    }
    VK retVK = vkOfType(sig->second.ret);
    std::string symLine =
        "func.func " + funcSym(m.name, f.name) +
        (sigTys.empty() ? "()" : "(" + sigTys + ")") +
        (retVK == VK::None ? "" : " -> " + std::string(mlirTy(retVK)));

    startFunction(symLine, m.name + "." + f.name, retVK, /*isTop=*/false, params);
    // 张量参数：从签名回填 dtype/rank（allocSlot 只建槽与零值）
    for (size_t k = 0; k < f.params.size(); ++k) {
        if (params[k].second != VK::Tensor) continue;
        auto it = locals_.find(f.params[k].name);
        const TypePtr& pt = sig->second.params[k];
        if (it != locals_.end() && pt && pt->kind == Type::Kind::Tensor) {
            if (!pt->params.empty())
                it->second.tInt = pt->params[0]->kind != Type::Kind::Float;
            it->second.tRank = static_cast<uint8_t>(pt->shape.size());
        }
    }
    genStmtList(f.body);
    finishFunction();
}

void Gen::genTopAssigns(const Module& m) {
    startFunction("func.func " + topSym(m.name) + "()", "<top " + m.name + ">",
                  VK::None, /*isTop=*/true, {});
    for (const auto& stmt : m.body)
        if (stmt->kind == Stmt::Kind::Assign) genStmt(*stmt);
    finishFunction();
}

void Gen::genMain() {
    const Module* em = s_.loadedModule(entry_);
    auto et = s_.registry().find(entry_);
    const FuncSig* mainSig = nullptr;
    SourceRange mainRange;
    if (et != s_.registry().end()) {
        auto f = et->second->funcs.find("main");
        if (f != et->second->funcs.end()) mainSig = &f->second;
    }
    if (em) {
        for (const auto& stmt : em->body)
            if (stmt->kind == Stmt::Kind::FuncDef &&
                static_cast<const FuncDef&>(*stmt).name == "main")
                mainRange = static_cast<const FuncDef&>(*stmt).nameRange;
    }

    if (mainSig && !mainSig->params.empty())
        unsupported(mainRange, "入口 main 不能带参数");

    curFile_ = s_.loadedFileId(entry_);
    std::string body;
    for (const auto& name : s_.modulesInDependencyOrder(entry_))
        body += "    func.call " + topSym(name) + "() : () -> ()\n";

    if (mainSig) {
        VK mainVK = vkOfType(mainSig->ret);
        if (mainVK == VK::Int) {
            body += "    %r = func.call " + funcSym(entry_, "main") +
                    "() : () -> i64\n";
            body += "    %c = arith.trunci %r : i64 to i32\n";
            body += "    func.return %c : i32\n";
        } else {
            std::string retTy =
                mainVK == VK::None ? "()" : std::string(mlirTy(mainVK));
            body += "    func.call " + funcSym(entry_, "main") + "() : () -> " +
                    retTy + "\n";
            body += "    %z = arith.constant 0 : i32\n";
            body += "    func.return %z : i32\n";
        }
    } else {
        // 与 VM::run 一致：无 main 静默跳过，退出码 0。
        body += "    %z = arith.constant 0 : i32\n";
        body += "    func.return %z : i32\n";
    }
    funcDefs_.push_back("  func.func @main() -> i32 {\n" + body + "  }\n");
}

void Gen::assemble(std::string& out) {
    out += "module {\n";
    out += "  // Scourge AOT 生成：入口 '" + entry_ + "'，模块 ";
    {
        bool first = true;
        for (const auto& name : s_.modulesInDependencyOrder(entry_)) {
            if (!first) out += ", ";
            first = false;
            out += name;
        }
    }
    out += "\n";
    if (!emptyStrDef_.empty()) out += "  " + emptyStrDef_ + "\n";
    for (const auto& d : strDefs_) out += "  " + d + "\n";
    for (const auto& d : globalDefs_) out += "  " + d + "\n";
    // try 蹦床：func.call 直连运行时（控制流设施例外——setjmp 双返回无法
    // 表达为无副作用 lang op；与 llvm.alloca 同类）。
    if (tryDeclsEmitted_) {
        out += "  func.func private @sg_try_push() -> !llvm.ptr\n";
        out += "  func.func private @sg_try_pop(!llvm.ptr) -> ()\n";
        // @_setjmp 双参 ABI（jmp_buf, frameaddress(0)）由 clang 前端
        // intrinsic 特判引入；@llvm.frameaddress.p0 经声明调用，LLVM 后端
        // 按 intrinsic 展开（无需库符号）。returns_twice 调用点属性由
        // InProcess 在 translate 后程序化补齐（LangRef 要求 call-site
        // 属性，声明 passthrough 仅作冗余标注）。
        out += "  func.func private @_setjmp(!llvm.ptr, !llvm.ptr) -> i32 "
               "attributes {passthrough = [\"returns_twice\"]}\n";
        out += "  func.func private @llvm.frameaddress.p0(i32) -> !llvm.ptr\n";
    }
    // 内嵌 C 运行时 @sg_* 外部声明已由 LangToLLVM pass 模块级补插
    // （lang 层不出现运行时符号——AOT 步骤 2 分层纪律）。
    for (const auto& f : funcDefs_) out += f;
    out += "}\n";
}

bool Gen::run(std::string& out) {
    try {
        precollectGlobals();
        for (const auto& name : s_.modulesInDependencyOrder(entry_))
            genModule(name);
        genMain();
        assemble(out);
    } catch (const GenAbort&) {
        return false;
    }
    return !s_.diag().hasErrors();
}

} // namespace

// ============================================================================
// 对外入口
// ============================================================================

bool generateMLIR(Session& session, const std::string& entry, std::string& out) {
    Gen gen(session, entry, session.exprTypes());
    return gen.run(out);
}

} // namespace aot
} // namespace scourge
