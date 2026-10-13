// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge REPL — 会话式求值循环（设计文档 5.3：表达式即时求值 + :check）
// ----------------------------------------------------------------------------
//  会话状态 = 活动模块图：import 模块集 + 累积顶层语句（preamble，仅
//  def / 赋值——与模块顶层规则一致）。每轮以「import 声明 + preamble +
//  求值包装」重建 repl 模块源码，import 模块从磁盘重新加载（磁盘编辑
//  即时生效），走完整 Lexer → Parser → TypeChecker → bc::Compiler →
//  bc::VM 管线，与 check / run 共享同一前端与执行语义，无双模漂移。
//
//  输入形态：
//    * 表达式       → 包装进 __repl__() 求值并输出（print 包装）；
//    * print 调用   → 直接作为语句执行（避免 print(print(...)) 二次包装）；
//    * import <名>  → 试编译验收（完整检查不执行），0 错误并入模块图，
//                     失败回滚；from-import 显式拒绝；
//    * 语句 / 块    → 行尾冒号开启块累积（空行结束），整体并入 preamble
//                     （仅验证不执行；失败回滚，会话状态不丢）；
//    * :命令        → :help :check :reset :quit。
//
//  MVP 已知限制（:help 中注明）：
//    * 表达式轮会重放 preamble 与 import 模块顶层的副作用；
//    * import 模块相对当前工作目录解析 <名>.sg；
//    * __repl__ 为求值包装保留名，会话内不可重复定义。
// ============================================================================
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace scourge {

class Repl {
public:
    Repl(std::ostream& out, std::ostream& err);

    /// 交互主循环（stdin 驱动；TTY 时输出提示符）。返回进程退出码。
    int run(std::istream& in);

    /// 喂入一行输入（测试 / 脚本驱动；run 的内循环即逐行 feed）。
    /// 返回 false = 会话已结束（:quit / :q）。
    bool feed(const std::string& line);

    bool finished() const { return finished_; }

private:
    /// 提交一个完整输入单元（单行或整块）：分类 → 求值 / 并入会话。
    void acceptUnit(const std::string& unit);
    void runCommand(const std::string& cmd);
    /// 表达式求值：合成 __repl__ 包装并完整编译执行。
    /// wrapPrint=true 时输出经 print 包装（print 调用本身不包装）。
    void evalExpression(const std::string& expr, bool wrapPrint);
    /// 语句并入会话状态；验证失败即回滚（错误恢复不丢会话）。
    void defineStatement(const std::string& stmt);
    /// import 并入会话模块图：试编译验收，失败回滚（模块不入会话）。
    void acceptImport(const std::string& unit);
    /// 合成整轮源码：import 声明 + preamble + tail（求值包装 / 空）。
    std::string sessionSource(const std::string& tail) const;
    /// 编译（可选执行）一段合成源码；诊断输出至 err_。返回是否 0 错误。
    bool compileSession(const std::string& source, bool execute);

    std::ostream& out_;
    std::ostream& err_;
    std::string preamble_;            ///< 会话状态：已接受的顶层语句（按输入序拼接）
    std::vector<std::string> imports_; ///< 会话模块图：已接受的 import 模块名（按序）
    bool inBlock_ = false;            ///< 块累积中（行尾冒号开启，空行结束）
    std::string blockBuf_;
    size_t lastWarnings_ = 0;         ///< 最近一轮编译的警告数（:check 报告用）
    bool finished_ = false;
};

/// CLI 入口：scourge repl（main.cpp 分发）。
int cmdRepl();

} // namespace scourge
