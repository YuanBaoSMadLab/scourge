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
//  * 不进入 AOT 的构造（List/Tensor/Result/Index/Try 等）显式 E1007，
//    无静默回退。
// ============================================================================
#include "scourge/AOT/MLIRGen.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
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
/// Tensor 以 !llvm.ptr 存 C 运行时 SgTensor 句柄（tInt 记录元素 dtype）。
enum class VK { Void, Int, Float, Bool, Str, None, Range, Tensor, Bad };

struct Value {
    VK vk = VK::Void;
    std::string ssa;     ///< MLIR SSA 名；None/Void 为空串
    bool tInt = false;   ///< VK::Tensor 时有效：true = i64 元素，false = f64
    uint8_t tRank = 0;   ///< VK::Tensor 时有效：秩（get 标量 / slice 判定用）
};

const char* mlirTy(VK vk) {
    switch (vk) {
    case VK::Int: return "i64";
    case VK::Float: return "f64";
    case VK::Bool: return "i1";
    case VK::Str: return "!llvm.ptr";
    case VK::Range: return "i64"; // 存上限 n
    case VK::Tensor: return "!llvm.ptr";
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
    return VK::Bad;
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
                    const SourceRange& range, const Expr* rExpr);
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
    std::vector<std::pair<std::string, std::string>> loopStack_; // break / continue
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

    // 参数槽 + store（None 参数无 SSA，跳过 store）
    for (size_t k = 0; k < params.size(); ++k) {
        Slot slot = allocSlot(params[k].second);
        locals_.emplace(params[k].first, slot);
        if (params[k].second != VK::None) {
            std::string arg = "%a" + std::to_string(k);
            const char* ty = mlirTy(params[k].second);
            emit("llvm.store " + arg + ", " + slot.ptr + " : " + ty +
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
    switch (vk) {
    case VK::Int:
    case VK::Range:
        emitEntry("llvm.store " + zeroI64() + ", " + slot.ptr + " : i64, !llvm.ptr");
        break;
    case VK::Float:
        emitEntry("llvm.store " + zeroF64() + ", " + slot.ptr + " : f64, !llvm.ptr");
        break;
    case VK::Bool:
        emitEntry("llvm.store " + boolReg(false) + ", " + slot.ptr +
                  " : i1, !llvm.ptr");
        break;
    case VK::Str: {
        std::string p = fresh();
        emitEntry(p + " = llvm.mlir.addressof " + internStr("") + " : !llvm.ptr");
        emitEntry("llvm.store " + p + ", " + slot.ptr + " : !llvm.ptr, !llvm.ptr");
        break;
    }
    case VK::Tensor: {
        // 零值槽存空张量句柄（防野值；与 VM 槽位零值纪律一致）。
        std::string p = fresh();
        emitEntry(p + " = \"lang.tensor_empty\"() : () -> !llvm.ptr");
        emitEntry("llvm.store " + p + ", " + slot.ptr + " : !llvm.ptr, !llvm.ptr");
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
    emit(ssa + " = llvm.load " + slot.ptr + " : !llvm.ptr -> " + mlirTy(slot.vk));
    return Value{slot.vk, ssa, slot.tInt, slot.tRank};
}

/// 存储槽位：唯一隐式转换 int → float（与 isConvertible 一致）；
/// Tensor 槽同步更新元素 dtype / 秩记录。
void Gen::storeTo(Slot& slot, Value v) {
    if (slot.vk == VK::None) return;
    if (slot.vk == VK::Tensor) {
        slot.tInt = v.tInt;
        slot.tRank = v.tRank;
    }
    if (slot.vk == VK::Float && v.vk == VK::Int) {
        std::string ssa = fresh();
        emit(ssa + " = arith.sitofp " + v.ssa + " : i64 to f64");
        v = Value{VK::Float, ssa};
    }
    emit("llvm.store " + v.ssa + ", " + slot.ptr + " : " + mlirTy(slot.vk) +
         ", !llvm.ptr");
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
                return VK::Bad; // sum/max/min/transpose/reshape/ok/err
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
        return genBinary(b.op, l, r, b.range, b.rhs.get());
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
        if (base.vk != VK::Tensor)
            unsupported(ix.base->range, "下标读取仅支持张量");
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
        if (!ls.checkedAsTensor)
            unsupported(e.range, "列表字面量（AOT 仅支持张量字面量）");
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

Value Gen::genBinary(const std::string& op, Value l, Value r,
                     const SourceRange& range, const Expr* rExpr) {
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

    // ---- 比较 ----
    if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
        op == ">=") {
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
            std::string sign = op == "=="   ? "eq"
                               : op == "!=" ? "ne"
                               : op == "<"  ? "slt"
                               : op == "<=" ? "sle"
                               : op == ">"  ? "sgt"
                                            : "sge";
            std::string b = fresh();
            emit(b + " = arith.cmpi " + sign + ", " + s + ", " + zeroI64() +
                 " : i64");
            return Value{VK::Bool, b};
        }
        if (l.vk == VK::Int && r.vk == VK::Int) {
            const char* spred = op == "=="   ? "eq"
                                : op == "!=" ? "ne"
                                : op == "<"  ? "slt"
                                : op == "<=" ? "sle"
                                : op == ">"  ? "sgt"
                                             : "sge";
            std::string b = fresh();
            emit(b + " = arith.cmpi " + std::string(spred) + ", " + l.ssa +
                 ", " + r.ssa + " : i64");
            return Value{VK::Bool, b};
        }
        if (l.vk == VK::Bool && r.vk == VK::Bool) {
            // VM 按数值比较（asDouble）——0/1 的无符号整数比较等价。
            const char* upred = op == "=="   ? "eq"
                                : op == "!=" ? "ne"
                                : op == "<"  ? "ult"
                                : op == "<=" ? "ule"
                                : op == ">"  ? "ugt"
                                             : "uge";
            std::string b = fresh();
            emit(b + " = arith.cmpi " + upred + ", " + l.ssa + ", " + r.ssa +
                 " : i1");
            return Value{VK::Bool, b};
        }
        // 数值混合（int/float）：提升 f64；Ne 用 une（NaN != NaN 与 VM 一致）。
        Value lf = promoteF64(l), rf = promoteF64(r);
        const char* fpred = op == "=="   ? "oeq"
                            : op == "!=" ? "une"
                            : op == "<"  ? "olt"
                            : op == "<=" ? "ole"
                            : op == ">"  ? "ogt"
                                         : "oge";
        std::string b = fresh();
        emit(b + " = arith.cmpf " + fpred + ", " + lf.ssa + ", " + rf.ssa +
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
    // op 编码：0=+ 1=- 2=* 3=/ 4=// 5=% 6=**（与 C 运行时 sg_tiop/sg_tfop 一致）。
    const int tOp = op == "+"    ? 0
                    : op == "-"  ? 1
                    : op == "*"  ? 2
                    : op == "/"  ? 3
                    : op == "//" ? 4
                    : op == "%"  ? 5
                    : op == "**" ? 6
                                 : -1;
    if (op == "@" ||
        (tOp >= 0 && (l.vk == VK::Tensor || r.vk == VK::Tensor))) {
        if (op == "@") {
            if (l.vk != VK::Tensor || r.vk != VK::Tensor)
                unsupported(range, "'@' 需要两个张量操作数");
            std::string ssa = fresh();
            emit(ssa + " = \"lang.tensor_matmul\"(" + l.ssa + ", " + r.ssa +
                 ") : (!llvm.ptr, !llvm.ptr) -> !llvm.ptr " + opLoc());
            return Value{VK::Tensor, ssa, l.tInt && r.tInt, 2};
        }
        if (l.vk == VK::Tensor && r.vk == VK::Tensor) {
            std::string ssa = fresh();
            emit(ssa + " = \"lang.tensor_binop_tt\"(" + i32Const(tOp) + ", " +
                 l.ssa + ", " + r.ssa +
                 ") : (i32, !llvm.ptr, !llvm.ptr) -> !llvm.ptr " + opLoc());
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
        std::string ssa = fresh();
        if (l.vk == VK::Tensor) { // 张量 op 标量 → ts
            emit(ssa + " = \"lang.tensor_binop_ts\"(" + i32Const(tOp) + ", " +
                 t.ssa + ", " + si + ", " + vi + ", " + vf +
                 ") : (i32, !llvm.ptr, i32, i64, f64) -> !llvm.ptr " + opLoc());
        } else { // 标量 op 张量 → st（标量在左，C 侧负责方向）
            emit(ssa + " = \"lang.tensor_binop_st\"(" + i32Const(tOp) + ", " +
                 si + ", " + vi + ", " + vf + ", " + t.ssa +
                 ") : (i32, i32, i64, f64, !llvm.ptr) -> !llvm.ptr " + opLoc());
        }
        return Value{VK::Tensor, ssa, tOp != 3 && t.tInt && sc.vk != VK::Float,
                     t.tRank};
    }

    // ---- 算术 ----
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "//" ||
        op == "%" || op == "**") {
        if (!isNumVK(l.vk) || !isNumVK(r.vk))
            unsupported(range, "运算符 '" + op + "' 的操作数类型");

        if (l.vk != VK::Float && r.vk != VK::Float) {
            // ---- 纯整数路径 ----
            struct IntHelper {
                const char* sym;
                const char* ret;
            };
            IntHelper h = op == "+"    ? IntHelper{"lang.addi", "i64"}
                          : op == "-"  ? IntHelper{"lang.subi", "i64"}
                          : op == "*"  ? IntHelper{"lang.muli", "i64"}
                          : op == "/"  ? IntHelper{"lang.divi", "f64"}
                          : op == "//" ? IntHelper{"lang.floordivi", "i64"}
                          : op == "%"  ? IntHelper{"lang.modi", "i64"}
                                       : IntHelper{"lang.powi", "i64"};
            if (op == "**") {
                // 负指数结果为 float（结果类型依赖运行时符号）——AOT 要求
                // 指数为非负整数字面量。
                if (!rExpr || rExpr->kind != Expr::Kind::IntLit ||
                    static_cast<const IntLit&>(*rExpr).value < 0)
                    unsupported(range,
                                "整数幂 '**' 需要非负整数字面量指数");
            }
            std::string ssa = fresh();
            emit(ssa + " = \"" + h.sym + "\"(" + l.ssa + ", " + r.ssa +
                 ") : (i64, i64) -> " + h.ret + " " + opLoc());
            return Value{op == "/" ? VK::Float : VK::Int, ssa};
        }

        // ---- 浮点路径 ----
        Value lf = promoteF64(l), rf = promoteF64(r);
        if (op == "+") {
            std::string ssa = fresh();
            emit(ssa + " = arith.addf " + lf.ssa + ", " + rf.ssa + " : f64");
            return Value{VK::Float, ssa};
        }
        if (op == "-") {
            std::string ssa = fresh();
            emit(ssa + " = arith.subf " + lf.ssa + ", " + rf.ssa + " : f64");
            return Value{VK::Float, ssa};
        }
        if (op == "*") {
            std::string ssa = fresh();
            emit(ssa + " = arith.mulf " + lf.ssa + ", " + rf.ssa + " : f64");
            return Value{VK::Float, ssa};
        }
        const char* sym = op == "/"   ? "lang.divf"
                          : op == "//" ? "lang.floordivf"
                          : op == "%"  ? "lang.modf"
                                       : "lang.powf";
        std::string ssa = fresh();
        emit(ssa + " = \"" + sym + "\"(" + lf.ssa + ", " + rf.ssa +
             ") : (f64, f64) -> f64 " + opLoc());
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
    int thenB = newBlock(), elseB = newBlock(), joinB = newBlock();
    terminate("cf.cond_br " + cond.ssa + ", " + blk(thenB).label + ", " +
              blk(elseB).label);
    cur_ = thenB;
    storeTo(slot, genExpr(*t.thenE));
    terminate("cf.br " + blk(joinB).label);
    cur_ = elseB;
    storeTo(slot, genExpr(*t.elseE));
    terminate("cf.br " + blk(joinB).label);
    cur_ = joinB;
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
        if (v.vk != VK::Str) unsupported(c.args[0]->range, "len() 仅支持 str");
        std::string ssa = fresh();
        emit(ssa + " = \"lang.str_len\"(" + v.ssa + ") : (!llvm.ptr) -> i64");
        return Value{VK::Int, ssa};
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
    if (name == "abs" || name == "sqrt" || name == "exp" || name == "log") {
        need(1);
        Value v = genExpr(*c.args[0]);
        if (v.vk == VK::Tensor) {
            // 逐元素：fnid 0=abs 1=sqrt 2=exp 3=log；abs 保 dtype，其余恒 f64
            const int fnid = name == "abs"   ? 0
                             : name == "sqrt" ? 1
                             : name == "exp"  ? 2
                                              : 3;
            std::string ssa = fresh();
            emit(ssa + " = \"lang.tensor_un\"(" + i32Const(fnid) + ", " +
                 v.ssa + ") : (i32, !llvm.ptr) -> !llvm.ptr " + opLoc());
            return Value{VK::Tensor, ssa, fnid == 0 && v.tInt, v.tRank};
        }
        if (v.vk != VK::Int && v.vk != VK::Float)
            unsupported(c.args[0]->range, name + "() 仅支持数值");
        if (name == "abs") {
            std::string ssa = fresh();
            if (v.vk == VK::Int) {
                emit(ssa + " = \"lang.abs_i\"(" + v.ssa + ") : (i64) -> i64");
                return Value{VK::Int, ssa};
            }
            emit(ssa + " = \"lang.abs_f\"(" + v.ssa + ") : (f64) -> f64");
            return Value{VK::Float, ssa};
        }
        const char* sym = name == "sqrt" ? "lang.sqrt"
                          : name == "exp" ? "lang.exp"
                                          : "lang.log";
        Value vf = promoteF64(v);
        std::string ssa = fresh();
        emit(ssa + " = \"" + sym + "\"(" + vf.ssa + ") : (f64) -> f64 " + opLoc());
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
        emit(ssa + " = \"lang.tensor_sum_f\"(" + v.ssa + ") : (!llvm.ptr) -> f64");
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
        emit(ssa + " = \"lang.tensor_transpose\"(" + v.ssa +
             ") : (!llvm.ptr) -> !llvm.ptr " + opLoc());
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
    // ok / err（Result 构造）
    unsupported(c.range, "内建 " + name + "()（Result 构造）");
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
            terminate("func.return");
            break;
        }
        Value v = genExpr(*st.value);
        if (curRetVK_ == VK::None || curRetVK_ == VK::Void) {
            terminate("func.return");
            break;
        }
        if (curRetVK_ == VK::Float && v.vk == VK::Int) {
            std::string ssa = fresh();
            emit(ssa + " = arith.sitofp " + v.ssa + " : i64 to f64");
            v = Value{VK::Float, ssa};
        }
        terminate("func.return " + v.ssa + " : " + mlirTy(curRetVK_));
        break;
    }
    case Stmt::Kind::Break:
        if (loopStack_.empty()) unsupported(s.range, "break 不在循环内");
        terminate("cf.br " + loopStack_.back().first);
        break;
    case Stmt::Kind::Continue:
        if (loopStack_.empty()) unsupported(s.range, "continue 不在循环内");
        terminate("cf.br " + loopStack_.back().second);
        break;
    case Stmt::Kind::Pass:
        break;
    case Stmt::Kind::Assert:
        genAssert(static_cast<const Assert&>(s));
        break;
    case Stmt::Kind::Try:
        unsupported(s.range, "try / except（panic 恢复）");
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
        Value v = genBinary(op, l, r, a.range, nullptr);
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
    loopStack_.emplace_back(blk(endB).label, blk(condB).label);
    genStmtList(w.body);
    loopStack_.pop_back();
    terminate("cf.br " + blk(condB).label); // 回边重新求值条件
    cur_ = endB;
}

void Gen::genFor(const For& f) {
    // 仅 range 迭代（List 值在 AOT 中无法构造，其来源必先触发 E1007）。
    Value n = genExpr(*f.iter);
    if (n.vk != VK::Range) unsupported(f.iter->range, "for 迭代源仅支持 range");
    Slot counter = allocSlot(VK::Int);
    Slot varSlot = allocSlot(VK::Int);
    locals_[f.var] = varSlot;
    storeTo(counter, Value{VK::Int, zeroI64()});

    int condB = newBlock();
    terminate("cf.br " + blk(condB).label);
    cur_ = condB;
    std::string c = fresh();
    emit(c + " = llvm.load " + counter.ptr + " : !llvm.ptr -> i64");
    std::string cont = fresh();
    emit(cont + " = arith.cmpi slt, " + c + ", " + n.ssa + " : i64");
    int bodyB = newBlock(), endB = newBlock();
    terminate("cf.cond_br " + cont + ", " + blk(bodyB).label + ", " +
              blk(endB).label);
    cur_ = bodyB;
    storeTo(varSlot, Value{VK::Int, c});
    loopStack_.emplace_back(blk(endB).label, blk(condB).label);
    genStmtList(f.body);
    loopStack_.pop_back();
    // 计数器 +1（body 以 return 终止时，后续 emit 自动落入死代码丢弃）。
    std::string c2 = fresh();
    emit(c2 + " = llvm.load " + counter.ptr + " : !llvm.ptr -> i64");
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
                if (vk == VK::Bad || vk == VK::Tensor)
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
