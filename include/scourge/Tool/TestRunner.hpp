// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 测试框架 — 显式测试 + 契约属性测试（设计文档第六章第 5 条）
// ----------------------------------------------------------------------------
//  命令形态：scourge test [--json] [--seed N] [--runs N] <file.sg>
//
//  两类预言（"契约即测试预言"）：
//   * 显式测试：同级 <stem>.sgt 内顶层 def test_*() -> bool；返回 False
//     或 panic 即失败（每个测试独立 try/except 隔离，互不中断）。
//   * 属性测试：目标模块 @pure 且带契约的函数自动生成预言——
//       requires/invariant(参数部分) 求值为 False → 该例跳过；
//       前置满足后调用 panic / ensures 违反 / @pure 差分不一致 → 失败并报反例。
//
//  执行模型（与 REPL 同构）：每轮独立 Session（诊断/符号随轮销毁），驱动
//  模块在内存合成（print "#TEST ..." 协议行），C++ 侧解析协议聚合结果。
//  输入生成：splitmix64 确定性流；--seed 显式指定，缺省按 模块/函数名 派生
//  （同版本下逐字节可复现，设计文档"确定性"纪律）。
//
//  纯助手（Rng / renderFloat / renderString / qualifyContractText）置于
//  scourge::sgt 命名空间供单测直接覆盖。
// ============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace scourge {
namespace sgt {

/// splitmix64 — 测试输入生成用的确定性 PRNG（无状态推进，纯函数纪律）。
struct Rng {
    uint64_t state;

    explicit Rng(uint64_t seed) : state(seed) {}
    explicit Rng(const std::string& derived) ; ///< fnv1a64(derived) 派生种子

    uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    /// [lo, hi] 内均匀整数（模偏差对测试输入无影响，确定性优先）。
    int64_t below(int64_t lo, int64_t hi) {
        return lo + static_cast<int64_t>(next() %
                                         static_cast<uint64_t>(hi - lo + 1));
    }
};

/// 浮点字面量渲染：定点小数（无指数记法，规避字面量解析分歧），
/// 去尾零但保留小数点后至少一位（"100" 不合法的浮点形态）。NaN/inf 不产生。
std::string renderFloat(double v);

/// 字符串字面量渲染：双引号包裹，转义反斜杠/双引号/换行。
std::string renderString(const std::string& s);

/// 契约表达式文本的驱动适配（词法级状态机，纯文本变换）：
///   * 模块本地名（locals 内的标识符）→ "模块名.名字"——驱动模块中未限定
///     名只解析内建与本模块符号，跨模块引用必须限定（Sema::checkCall 规则）；
///   * result → _sgt_r（ensures 中被测调用结果在驱动里绑定为 _sgt_r）；
///   * 字符串字面量内容与紧随 '.' 的成员名不改写。
std::string qualifyContractText(const std::string& text,
                                const std::string& modName,
                                const std::map<std::string, bool>& locals);

} // namespace sgt

/// `scourge test` 命令入口；返回进程退出码（0 全过，1 有失败/诊断，2 用法错）。
int runTests(const std::vector<std::string>& args);

} // namespace scourge
