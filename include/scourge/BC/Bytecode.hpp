// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 字节码 SG-BC（设计文档第二/五章：调试模式后端）
// ----------------------------------------------------------------------------
//  值模型：运行时类型标签（设计文档 5.1"线程码解释器（类型标签）"）。
//  P1 值集：int / float / bool / str / None / List / range / Result / Tensor。
//  Tensor 执行已进入字节码后端（字面量、逐元素 + 标量广播、@、多维下标）；
//  Dict 的执行随后续迭代进入（编译期拦截并明确报错，无静默回退——
//  设计文档"显式拒绝"原则）。
//
//  语义纪律（与 AOT 共享前端，杜绝双模语义漂移）：
//    * 整数溢出 panic、除零 panic、索引越界 panic——无 UB；
//    * panic 由 try/except 捕获（文档第四章：不可恢复条件的恢复路径）；
//    * Result 错误即值：unwrap 失败态即 panic（强制显式消费）。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace scourge {
namespace bc {

// ----------------------------------------------------------------------------
// 运行时值
// ----------------------------------------------------------------------------

/// 张量数据：dtype + 形状 + 连续行主序存储（共享语义，与 List 一致）。
/// 仅支持 f64 / i64 两种元素类型：字面量按"出现任一浮点字面量 → f64，
/// 否则 i64"推断，运算结果元素类型与检查器 unifyNumeric 规则一致
/// （int∘int → int，其余 → float；真除法 "/" 恒为 float）。
struct TensorData {
    bool isInt = false;         ///< false = f64，true = i64
    std::vector<int64_t> shape; ///< 形状（秩 ≥ 1，行主序）
    std::vector<double> f64;    ///< isInt=false 时有效
    std::vector<int64_t> i64;   ///< isInt=true 时有效

    size_t size() const { return isInt ? i64.size() : f64.size(); }
    /// 各维大小之积（应与 size() 相等，由构造方保证）。
    int64_t prodOfShape() const {
        int64_t n = 1;
        for (int64_t d : shape) n *= d;
        return n;
    }
};

struct Value {
    enum class Kind : uint8_t {
        Int, Float, Bool, Str, None, List, Range, Result, Tensor,
    };

    Kind kind = Kind::None;

    int64_t i = 0;   ///< Int / Bool(0,1) / Range(n)
    double f = 0;    ///< Float
    bool b = false;  ///< Bool
    std::string s;   ///< Str / Result 失败信息
    bool isOk = false; ///< Result：成功/失败态
    Kind payloadKind = Kind::None; ///< Result-ok 的载荷类型（unwrap 重建用）
    std::shared_ptr<std::vector<Value>> list; ///< List（共享语义）
    std::shared_ptr<TensorData> tensor;       ///< Tensor（共享语义）

    static Value makeInt(int64_t v) {
        Value x; x.kind = Kind::Int; x.i = v; return x;
    }
    static Value makeFloat(double v) {
        Value x; x.kind = Kind::Float; x.f = v; return x;
    }
    static Value makeBool(bool v) {
        Value x; x.kind = Kind::Bool; x.b = v; return x;
    }
    static Value makeStr(std::string v) {
        Value x; x.kind = Kind::Str; x.s = std::move(v); return x;
    }
    static Value makeNone() { return Value{}; }
    static Value makeList(std::shared_ptr<std::vector<Value>> v) {
        Value x; x.kind = Kind::List; x.list = std::move(v); return x;
    }
    static Value makeTensor(std::shared_ptr<TensorData> t) {
        Value x; x.kind = Kind::Tensor; x.tensor = std::move(t); return x;
    }
    static Value makeRange(int64_t n) {
        Value x; x.kind = Kind::Range; x.i = n; return x;
    }
    static Value makeResult(const Value& payload, bool ok, std::string err = {}) {
        Value x = ok ? payload : Value{};
        x.kind = Kind::Result;
        x.payloadKind = payload.kind;
        x.isOk = ok;
        if (!ok) x.s = std::move(err);
        return x;
    }
};

/// print / 错误信息用的人类可读形式（确定性格式化）。
std::string stringify(const Value& v);

// ----------------------------------------------------------------------------
// 指令集
// ----------------------------------------------------------------------------
enum class Op : uint8_t {
    LoadConst,   ///< a=常量池索引
    LoadLocal,   ///< a=槽位
    StoreLocal,  ///< a=槽位
    LoadGlobal,  ///< a=全局名表索引
    StoreGlobal, ///< a=全局名表索引
    Pop,
    BinOp,       ///< a=Bin 枚举（运行时按操作数类型分派）
    UnaryOp,     ///< a=Unary 枚举（- + not）
    Jump,        ///< a=目标 pc
    JumpIfFalse, ///< a=目标 pc（弹出；false 跳转）
    JumpIfFalseOrPop, ///< false：跳转且保留值；true：弹出
    JumpIfTrueOrPop,  ///< true：跳转且保留值；false：弹出
    Call,        ///< a=函数表索引, b=实参个数
    CallMethod,  ///< a=方法枚举, b=实参个数（self 已在栈上）
    Return,      ///< 弹出返回值
    ReturnNone,
    Print,       ///< 弹出并输出
    NewList,     ///< a=元素个数（弹出）
    LoadIndex,   ///< b=下标个数；base + b 个下标 → value（Tensor 支持部分索引）
    StoreIndex,  ///< b=下标个数；base + b 个下标 + value →（List 单下标 / Tensor 全秩）
    Assert,      ///< b=1 时先弹 msg 再弹 cond
    EnterTry,    ///< a=handler pc（压运行时 catch：handler + 当前栈深）
    ExitTry,     ///< 弹运行时 catch
    ForNext,     ///< a=tmp 槽, b=counter 槽 → 压入下一元素 + true，或仅压入 false（结束）
    MatMul,      ///< 两操作数（二维张量）→ 矩阵乘 (m, k) @ (k, n) → (m, n)
    NewTensor,   ///< b=元素个数 → 全标量压成一维张量 / 全张量行拼接升一维
};

enum class Bin : uint8_t {
    Add, Sub, Mul, Div, FloorDiv, Mod, Pow,
    Eq, Ne, Lt, Le, Gt, Ge, And, Or, In,
};

enum class Unary : uint8_t { Neg, Pos, Not };

/// Result 消费方法。unwrap(缺省)/unwrap_or(缺省) 同形（检查器 P0 语义：
/// 必须携带缺省值，ok 取载荷、失败态取缺省）。
enum class Method : uint8_t { IsOk, Unwrap, UnwrapOr };

struct Instr {
    Op op;
    uint32_t a = 0;
    uint32_t b = 0;
};

// ----------------------------------------------------------------------------
// 函数与程序
// ----------------------------------------------------------------------------
struct FuncBC {
    std::string name;
    std::string module;
    uint32_t paramCount = 0;
    uint32_t requiredArgs = 0;          ///< 尾部缺省参数个数 = paramCount - requiredArgs
    std::vector<Value> defaults;        ///< 缺省参数值（编译期常量求值）
    uint32_t nlocals = 0;               ///< 局部槽位数
    std::vector<Instr> code;
    std::vector<std::pair<uint32_t, uint32_t>> lines; ///< (pc, line) 升序
    bool isBuiltin = false;             ///< 内建：print/str/len/range/ok/err
    std::string builtinName;
    bool isTopLevel = false;            ///< 模块顶层初始化代码（伪函数）
    bool isAsync = false;               ///< P0 阶段 await 透传，直接同步执行
};

struct Program {
    std::vector<Value> constants;
    std::vector<std::pair<std::string, std::string>> globalNames; ///< (模块, 变量)
    std::vector<FuncBC> funcs;
    int findFunc(const std::string& module, const std::string& name) const;
};

// ----------------------------------------------------------------------------
// 运行时 panic（try/except 的捕获对象）
// ----------------------------------------------------------------------------
struct Panic {
    std::string message;
    std::string func;
    std::string module; ///< 所属模块（CLI 据此映射 file:line）
    uint32_t line = 0;
};

} // namespace bc
} // namespace scourge
