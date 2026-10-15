// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 字节码 SG-BC（设计文档第二/五章：调试模式后端）
// ----------------------------------------------------------------------------
//  值模型：运行时类型标签（设计文档 5.1"线程码解释器（类型标签）"）。
//  P1 值集：int / float / bool / str / None / List / range / Result / Tensor；
//  0.9.3 起追加 Dict / Set，0.9.6 起追加 Tuple（异构定长不可变）。
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

/// 字典数据（插入序哈希表；完整定义在 Value 之后——Entry 按值持有 Value，
/// 而 Value 持 shared_ptr<DictData>）。迭代序 = 插入序，确定性；键限标量
/// （int/float/bool/str；数值按 double 位哈希使 1 与 1.0 同键，与
/// valuesEqual 跨类型比较一致）；NaN 键破坏全序，由 VM 显式拒绝。
struct DictData;

/// 集合数据（0.9.3）：仅键的插入序哈希表——哈希/相等/迭代语义与 Dict 键
/// 槽位完全一致（复用 dictHashKey/valuesEqual，实现与 DictData 并排在
/// VM.cpp）；元素限标量，插入去重，迭代序 = 首插序，NaN 显式拒绝。
struct SetData;

/// 动态 JSON 值（0.9.4 M 专项）：完整定义在 SetData 之后。
struct JsonData;

struct Value {
    enum class Kind : uint8_t {
        Int, Float, Bool, Str, None, List, Range, Result, Tensor, Dict, Set,
        Tuple, ///< 异构定长不可变元组（0.9.6；复用 list 字段持元素向量）
        Json,  ///< 动态 JSON 值（0.9.4 M 专项；json 字段，尾部追加兼容）
    };

    Kind kind = Kind::None;

    int64_t i = 0;   ///< Int / Bool(0,1) / Range(n)
    double f = 0;    ///< Float
    bool b = false;  ///< Bool
    std::string s;   ///< Str / Result 失败信息
    bool isOk = false; ///< Result：成功/失败态
    Kind payloadKind = Kind::None; ///< Result-ok 的载荷类型（unwrap 重建用）
    std::shared_ptr<std::vector<Value>> list; ///< List / Tuple（共享语义）
    std::shared_ptr<TensorData> tensor;       ///< Tensor（共享语义）
    std::shared_ptr<DictData> dict;           ///< Dict（共享语义）
    std::shared_ptr<SetData> set;             ///< Set（共享语义）
    std::shared_ptr<JsonData> json;           ///< Json（共享语义；尾部追加）

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
    static Value makeDict(std::shared_ptr<DictData> d) {
        Value x; x.kind = Kind::Dict; x.dict = std::move(d); return x;
    }
    static Value makeSet(std::shared_ptr<SetData> d) {
        Value x; x.kind = Kind::Set; x.set = std::move(d); return x;
    }
    static Value makeTuple(std::shared_ptr<std::vector<Value>> v) {
        Value x; x.kind = Kind::Tuple; x.list = std::move(v); return x;
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

/// 字典数据：插入序哈希表（链地址 + 插入序 entries 向量，迭代序 = 插入序）。
struct DictData {
    struct Entry {
        Value key;
        Value val;
        uint32_t next = kEmpty; ///< 同桶链下一项（entries 下标）
    };
    static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
    std::vector<Entry> entries;    ///< 插入序
    std::vector<uint32_t> buckets; ///< 桶头（kEmpty = 空桶）

    DictData() : buckets(8, kEmpty) {}

    /// 查找键所在 entry 下标；不存在返回 kEmpty。
    uint32_t find(const Value& key) const;
    /// 插入或覆盖；返回 true 表示新增。
    bool set(const Value& key, const Value& val);
};

/// 集合数据：仅键的插入序哈希表（0.9.3）。链表结构与 DictData 同构
/// （links[i] = 同桶链下一项，与桶头一样是 entries 下标）；entries 保持
/// 纯 Value 向量——迭代与序列化直接按插入序走，链表纯属查找加速。
/// find/add 复用 VM.cpp 内的 dictHashKey/valuesEqual（元素语义 = Dict 键
/// 语义：数值跨类型同键、迭代序 = 首插序）。
struct SetData {
    static constexpr uint32_t kEmpty = 0xFFFFFFFFu;
    std::vector<Value> entries;      ///< 插入序
    std::vector<uint32_t> links;     ///< 与 entries 平行：同桶链下一项
    std::vector<uint32_t> buckets;   ///< 桶头（kEmpty = 空桶）

    SetData() : buckets(8, kEmpty) {}

    /// 查找元素所在 entries 下标；不存在返回 kEmpty。
    uint32_t find(const Value& key) const;
    /// 插入元素（已存在则忽略）；返回 true 表示新增。
    bool add(const Value& key);
};

/// 动态 JSON 值（0.9.4 M 专项）：json_decode(str) 的产物。七形态标签值——
/// 对象以 DictData 承载（键恒 str，值恒嵌套 Json，按原文成员插入序），
/// 数组以元素向量承载（元素恒嵌套 Json）。只读视图：无变更方法，
/// 取值经成员方法（is_*/as_*/get/len/keys/dump）。
struct JsonData {
    enum class JK : uint8_t {
        Null, Bool, Int, Float, Str, Arr, Obj,
    };
    JK jk = JK::Null;
    bool b = false;     ///< Bool
    int64_t i = 0;      ///< Int
    double f = 0;       ///< Float
    std::string s;      ///< Str（UTF-8 字节；\uXXXX 转义已解码）
    std::shared_ptr<std::vector<Value>> arr; ///< Arr：元素 kind 恒 Json
    std::shared_ptr<DictData> obj;           ///< Obj：键恒 Str，值恒 Json
};

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
    IntToFloat,  ///< 弹出 int 压入等值 float（Q3 边界提升；追加于尾部，字节码兼容）
    NewDict,     ///< a=键值对个数（弹出 2a 个：k1,v1,k2,v2,...；追加于尾部，字节码兼容）
    NewSet,      ///< a=元素个数（弹出 a 个：e1,e2,...；追加于尾部，字节码兼容）
    NewTuple,    ///< a=元素个数（弹出 a 个：e1,e2,...；异构不统一提升；尾部追加）
};

enum class Bin : uint8_t {
    Add, Sub, Mul, Div, FloorDiv, Mod, Pow,
    Eq, Ne, Lt, Le, Gt, Ge, And, Or, In,
};

enum class Unary : uint8_t { Neg, Pos, Not };

/// Result 消费方法。unwrap(缺省)/unwrap_or(缺省) 同形（检查器 P0 语义：
/// 必须携带缺省值，ok 取载荷、失败态取缺省）。
/// 0.9.4 起追加 str/List 成员方法（追加尾部保 .sgbc 缓存兼容）：
/// contains/index 在 str 与 List 上同名——VM 按 self.kind 分派，AOT 按检查器
/// 类型表静态分派（MLIRGenCall）；ListCount 仅 List。全部纯函数（str 不可变
/// 且列表无变更操作），panic 消息与 AOT sg_str_* / sg_list_* 逐字一致。
enum class Method : uint8_t {
    IsOk, Unwrap, UnwrapOr,
    StrUpper, StrLower, StrTrim, StrTrimStart, StrTrimEnd,
    StrSplit, StrJoin, StrReplace, StrFind,
    Contains, ///< str→子串判定；List→成员判定（运行时按 self.kind）
    Index,    ///< str→子串下标（未找到 panic）；List→元素下标（未找到 panic）
    StrStartsWith, StrEndsWith, ListCount,
    // 0.9.4 M 专项追加：Json 成员方法（动态 JSON 值，尾部追加兼容）。
    // is_* 恒 bool；get 在数组/对象上同名——VM 按 JsonData::JK 分派。
    JsonIsNull, JsonIsBool, JsonIsInt, JsonIsFloat, JsonIsStr, JsonIsList,
    JsonIsObj,
    JsonAsInt, JsonAsFloat, JsonAsBool, JsonAsStr,
    JsonAt,      ///< at(i)：数组 int 下标（越界 panic）
    JsonGet,     ///< get(k)：对象 str 键（缺键返回 null，不 panic）
    JsonLen,     ///< 数组/对象长度（标量 panic）
    JsonKeys,    ///< 对象键列表 List[str]（非对象 panic）
    JsonDump,    ///< 重编码为紧凑 JSON 文本
};

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
    bool isExtern = false;              ///< @extern：动态库外部函数（H 专项；
                                        ///< 体不编译，VM 走 VMExtern thunk）
    std::string externDll;              ///< 动态库文件名（@extern 装饰器参数）
    char externRet = 'v';               ///< 返回类型码：i/f/b/s/v（thunk 读回）
    bool isVariadic = false;            ///< 参数表尾 ...（O 节）：变参被调方
                                        ///< 按参数位置读 GP 影子槽，装载分叉
};

struct Program {
    std::vector<Value> constants;
    std::vector<std::pair<std::string, std::string>> globalNames; ///< (模块, 变量)
    std::vector<FuncBC> funcs;
    /// 能力授予集（0.9.7）：全部依赖闭包模块 capability 声明的并集，
    /// 位布局 = Common/Capability.hpp 的 Cap 枚举。VM 运行时门禁查此位；
    /// 序列化随 Program 持久化（SG-BC v2）。
    uint64_t capMask = 0;
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
