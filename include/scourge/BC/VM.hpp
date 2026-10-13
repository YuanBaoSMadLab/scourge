// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 栈式虚拟机（SG-BC 解释器，设计文档第五章"双模执行"）
// ----------------------------------------------------------------------------
//  * 运行时类型标签值模型（Value::Kind）——与前端共享语义，杜绝双模漂移；
//  * 整数溢出/除零/索引越界/断言失败/unwrap 失败态一律 panic，无 UB；
//  * panic 经运行时 catch 栈（EnterTry/ExitTry）被 try/except 捕获，
//    未捕获则携带 func/line 定位信息返回调用方（CLI 打印后退出）；
//  * 调用帧迭代实现（无 C++ 递归），深度护栏默认 200 层。
// ============================================================================
#pragma once

#include "scourge/BC/Bytecode.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace scourge {
namespace bc {

struct VMOptions {
    /// print 输出回调（测试注入断言用；空 → 写 stdout）。
    std::function<void(const std::string&)> out;
    /// 调用深度护栏：超出即 panic"调用深度超限"（可被 try/except 捕获）。
    size_t maxDepth = 200;
    /// 入口函数名（默认 main）。REPL 以 __repl__ 包装求值，
    /// 避免与用户在会话中定义的 main 冲突。
    std::string entryFunc = "main";
};

class VM {
public:
    VM(Program program, VMOptions opts = {});

    /// 执行程序：
    ///   1) 按 modules 顺序（依赖层序，被依赖者在前）执行各模块 <top>；
    ///   2) 调用 entry 模块的 main()（缺失则跳过）。
    /// 返回 false 表示存在未捕获 panic（lastPanic() 可取定位信息）。
    bool run(const std::vector<std::string>& modules, const std::string& entry);

    /// main() 的返回值（int 取值；缺失/非 int/panic → 0）。
    int exitCode() const { return exitCode_; }
    bool panicked() const { return panicked_; }
    const Panic& lastPanic() const { return lastPanic_; }

private:
    struct Frame {
        const FuncBC* fn = nullptr;
        uint32_t pc = 0;
        std::vector<Value> locals;
        std::vector<Value> stack;
    };
    struct CatchPoint {
        size_t frameIndex = 0; ///< 属于哪个调用帧
        uint32_t handlerPc = 0;
        size_t stackDepth = 0; ///< EnterTry 时该帧操作数栈深（panic 回退用）
    };
    /// 内部控制流异常：panic 抛出 → execute 捕获 → recover 回退或上抛。
    struct RuntimePanic {
        Panic panic;
    };

    // ---- 执行主循环 ------------------------------------------------------------
    void execute();               ///< panic 捕获与恢复重入
    void runLoop();               ///< 逐指令直到帧栈清空
    void dispatch(Instr in);      ///< 单指令分派（只使用 frames_.back()）
    void doReturn(Value v);       ///< 弹帧、清理失效 catch、回传返回值
    Value pop();
    void push(Value v);

    // ---- 调用 ------------------------------------------------------------------
    void callFunc(int fidx, uint32_t argc); ///< 内建直执行 / 用户函数压帧
    void doBuiltin(const FuncBC& fn, uint32_t argc);

    // ---- 运算（panic 语义集中在此） ---------------------------------------------
    Value doBinOp(Bin op, const Value& l, const Value& r);
    Value doUnaryOp(Unary op, const Value& v);
    int64_t normIndex(int64_t idx, int64_t len, const char* what);
    Value repeatStr(const std::string& s, int64_t n);

    // ---- 张量（P1 执行路径；panic 语义同样集中在此） ----------------------------
    /// 逐元素运算（同形张量）与标量广播（张量 ∘ 数值标量）。
    Value tensorBinOp(Bin op, const Value& l, const Value& r);
    /// I64 逐元素单步（溢出/除零 panic，语义与标量整数运算一致）。
    int64_t tensorIntOp(Bin op, int64_t x, int64_t y);
    /// F64 逐元素单步（除零 panic，Python 语义：floor/取模同号修正）。
    double tensorF64Op(Bin op, double x, double y);
    /// 二维 @ 二维 → (m, n)；秩或内维不符 panic（与检查器 E2004 同义）。
    Value tensorMatMul(const Value& l, const Value& r);
    /// NewTensor：弹 n 个元素（全标量 → 一维；全张量 → 行拼接升一维）。
    /// dtype 统一规则对齐检查器（int∘int → int，含 float → float）。
    Value tensorNew(uint32_t n);
    /// 张量下标读取：全索引 → 标量元素；部分索引 → 子张量拷贝（行主序切片）。
    Value tensorIndexRead(const Value& base, std::vector<Value>& idxs);
    /// 张量下标赋值：须提供全秩索引，就地写入共享缓冲区。
    void tensorIndexStore(Value& base, std::vector<Value>& idxs, const Value& val);

    // ---- panic 与恢复 ------------------------------------------------------------
    /// 以当前帧（frames_.back()，pc-1 指令）为定位抛出 panic。
    [[noreturn]] void panicHere(const std::string& msg);
    /// 按 catch 栈回退到最近 handler；false = 无捕获（execute 记录后返回）。
    bool recover(const Panic& p);

    Program program_;
    VMOptions opts_;
    std::vector<Frame> frames_;
    std::vector<CatchPoint> catches_;
    std::vector<Value> globals_; ///< 与 program_.globalNames 对齐
    Value lastReturn_;           ///< 最外层帧的返回值（main 的返回值）
    Panic lastPanic_;
    int exitCode_ = 0;
    bool panicked_ = false;
};

} // namespace bc
} // namespace scourge
