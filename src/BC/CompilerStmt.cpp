// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 字节码编译器 · 语句（Compiler 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：compileStmtList / emitDefers（defer LIFO 回链）/ compileStmt
//  （赋值/分支/循环/断言/try/defer/return/表达式语句）。
//  从 Compiler.cpp 按生命周期拆出；表达式见 CompilerExpr.cpp，
//  模块装配见 Compiler.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/BC/Compiler.hpp"
#include "scourge/BC/CompilerDetail.hpp"

#include <stdexcept>

namespace scourge {
namespace bc {

// ----------------------------------------------------------------------------
// 语句
// ----------------------------------------------------------------------------

void Compiler::compileStmtList(const StmtList& list) {
    for (const auto& s : list) compileStmt(*s);
}

void Compiler::emitDefers() {
    // LIFO 内联展开词法上位于该 return 之前的 defer 块。panic 传播走
    // 运行时 catch 栈回退，不经过 return 点，故不触发 defer（契约见
    // AST Defer 注释；VM/AOT 双模同构）。
    for (size_t k = deferStack_.size(); k > 0; --k)
        compileStmtList(*deferStack_[k - 1]);
}

void Compiler::compileStmt(const Stmt& s) {
    switch (s.kind) {
    case Stmt::Kind::Assign: {
        auto& a = static_cast<const Assign&>(s);
        if (a.target->kind == Expr::Kind::Name) {
            auto& n = static_cast<const Name&>(*a.target);
            compileExpr(*a.value);
            if (needsF64(*a.value)) emit(Op::IntToFloat); // Q3 边界提升
            if (fn_->isTopLevel) {
                emit(Op::StoreGlobal, addGlobal(moduleName_, n.name));
            } else {
                auto it = locals_->slots.find(n.name);
                if (it == locals_->slots.end()) {
                    it = locals_->slots
                             .emplace(n.name, locals_->nlocals++)
                             .first;
                }
                emit(Op::StoreLocal, it->second);
            }
            return;
        }
        // 下标赋值：base, 下标…, value → StoreIndex（张量须全秩，检查器保证）
        auto& ix = static_cast<const Index&>(*a.target);
        compileExpr(*ix.base);
        for (const auto& idx : ix.indices) compileExpr(*idx);
        compileExpr(*a.value);
        emit(Op::StoreIndex, 0, static_cast<uint32_t>(ix.indices.size()));
        return;
    }

    case Stmt::Kind::AugAssign: {
        auto& a = static_cast<const AugAssign&>(s);
        auto& n = static_cast<const Name&>(*a.target);
        uint32_t slot;
        if (fn_->isTopLevel) {
            slot = addGlobal(moduleName_, n.name);
            emit(Op::LoadGlobal, slot);
        } else {
            auto it = locals_->slots.find(n.name);
            if (it == locals_->slots.end()) {
                diag_.error(Errc::E2003_UnknownName, file_, n.range,
                            "增强赋值目标未声明");
                throw CompileAbort{};
            }
            slot = it->second;
            emit(Op::LoadLocal, slot);
        }
        compileExpr(*a.value);
        Bin op = Bin::Add;
        if (a.op == "+=") op = Bin::Add;
        else if (a.op == "-=") op = Bin::Sub;
        else if (a.op == "*=") op = Bin::Mul;
        else if (a.op == "/=") op = Bin::Div;
        else if (a.op == "//=") op = Bin::FloorDiv;
        else if (a.op == "%=") op = Bin::Mod;
        else if (a.op == "**=") op = Bin::Pow;
        else error(s.range, "增强赋值运算符 '" + a.op + "'");
        emit(Op::BinOp, static_cast<uint32_t>(op));
        if (fn_->isTopLevel) emit(Op::StoreGlobal, slot);
        else emit(Op::StoreLocal, slot);
        return;
    }

    case Stmt::Kind::ExprStmt:
        compileExpr(*static_cast<const ExprStmt&>(s).expr);
        emit(Op::Pop);
        return;

    case Stmt::Kind::If: {
        auto& st = static_cast<const If&>(s);
        compileExpr(*st.cond);
        uint32_t jFalse = emitJump(Op::JumpIfFalse);
        compileStmtList(st.thenBody);
        std::vector<uint32_t> ends;
        for (const auto& e : st.elifs) {
            uint32_t jNext = emitJump(Op::Jump);
            ends.push_back(jNext);
            patchJump(jFalse);
            compileExpr(*e.cond);
            jFalse = emitJump(Op::JumpIfFalse);
            compileStmtList(e.body);
        }
        uint32_t jElse = emitJump(Op::Jump);
        ends.push_back(jElse);
        patchJump(jFalse);
        compileStmtList(st.elseBody);
        for (uint32_t j : ends) patchJump(j);
        return;
    }

    case Stmt::Kind::While: {
        auto& st = static_cast<const While&>(s);
        uint32_t top = here();
        compileExpr(*st.cond);
        uint32_t jEnd = emitJump(Op::JumpIfFalse);
        loopDepth_++;
        breakJumps_.emplace_back();
        continueJumps_.emplace_back();
        loopEntryTries_.push_back(openTries_.size());
        compileStmtList(st.body);
        loopEntryTries_.pop_back();
        loopDepth_--;
        emit(Op::Jump, top); // 回边：重新求值条件
        for (uint32_t j : continueJumps_.back()) patchTo(j, top);
        continueJumps_.pop_back();
        for (uint32_t j : breakJumps_.back()) patchJump(j);
        breakJumps_.pop_back();
        patchJump(jEnd);
        return;
    }

    case Stmt::Kind::For: {
        auto& st = static_cast<const For&>(s);
        // tmp 迭代源 + counter 两槽；循环变量单独一槽。
        // N2 修复：iter/counter 为循环内部状态，不进名字映射——嵌套同名
        // 循环各自持有独立槽，内层不再覆写外层迭代器（否则外层回边读到
        // 已耗尽的内层迭代器而静默提前终止）。var 槽仍按名复用：与 Python
        // 一致，外层每次推进由 ForNext 后的 StoreLocal 重新赋值。
        auto freshSlot = [&]() { return locals_->nlocals++; };
        compileExpr(*st.iter);
        uint32_t tmp = freshSlot();
        emit(Op::StoreLocal, tmp);
        uint32_t counter = freshSlot();
        emit(Op::LoadConst, addConst(Value::makeInt(0)));
        emit(Op::StoreLocal, counter);
        auto slotOf = [&](const std::string& name) {
            auto it = locals_->slots.find(name);
            if (it == locals_->slots.end())
                it = locals_->slots.emplace(name, locals_->nlocals++).first;
            return it->second;
        };
        uint32_t var = slotOf(st.var);

        uint32_t top = here();
        emit(Op::ForNext, tmp, counter);   // 推进：压入下一元素或 false
        uint32_t jEnd = emitJump(Op::JumpIfFalse);
        emit(Op::StoreLocal, var);

        loopDepth_++;
        breakJumps_.emplace_back();
        continueJumps_.emplace_back();
        loopEntryTries_.push_back(openTries_.size());
        compileStmtList(st.body);
        loopEntryTries_.pop_back();
        loopDepth_--;
        emit(Op::Jump, top); // 回边：推进迭代器取下一元素
        for (uint32_t j : continueJumps_.back()) patchTo(j, top);
        continueJumps_.pop_back();
        for (uint32_t j : breakJumps_.back()) patchJump(j);
        breakJumps_.pop_back();
        patchJump(jEnd);
        return;
    }

    case Stmt::Kind::Return: {
        auto& st = static_cast<const Return&>(s);
        if (st.value) {
            compileExpr(*st.value);
            if (needsF64(*st.value)) emit(Op::IntToFloat); // Q3 边界提升（返回）
        }
        // defer 先于返回指令：返回值已驻留栈顶，defer 块语句栈自平衡
        // （语句不变式），执行完弹返回值——与 AOT 展开语义逐点对齐。
        emitDefers();
        if (st.value) {
            emit(Op::Return);
        } else {
            emit(Op::ReturnNone);
        }
        return;
    }

    case Stmt::Kind::Break:
    case Stmt::Kind::Continue: {
        if (loopEntryTries_.empty()) return; // 检查器已报 E2009
        // 穿越退出循环内已进入的 try（运行时 catch 栈保持一致）。
        size_t nExit = openTries_.size() - loopEntryTries_.back();
        for (size_t k = 0; k < nExit; ++k) emit(Op::ExitTry);
        if (s.kind == Stmt::Kind::Break) {
            if (breakJumps_.empty()) return; // 检查器已报 E2009
            breakJumps_.back().push_back(emitJump(Op::Jump));
        } else {
            if (continueJumps_.empty()) return;
            continueJumps_.back().push_back(emitJump(Op::Jump));
        }
        return;
    }

    case Stmt::Kind::Pass:
        return;

    case Stmt::Kind::Defer:
        // 仅登记不发射：展开发生在 return 点（emitDefers，LIFO）。
        deferStack_.push_back(&static_cast<const Defer&>(s).body);
        return;

    case Stmt::Kind::Assert: {
        auto& st = static_cast<const Assert&>(s);
        markLine(static_cast<uint32_t>(lineOf(s.range)));
        compileExpr(*st.cond);
        if (st.msg) compileExpr(*st.msg);
        emit(Op::Assert, 0, st.msg ? 1u : 0u);
        return;
    }

    case Stmt::Kind::Try: {
        auto& st = static_cast<const Try&>(s);
        // EnterTry 压运行时 catch（handler pc + 当前栈深）；body 正常结束
        // 时 ExitTry 弹出；panic 时 VM 按 catch 栈回退到 handler。
        emit(Op::EnterTry, 0); // handler 待 patch
        uint32_t enterAt = here() - 1;
        openTries_.push_back(enterAt);

        compileStmtList(st.body);

        openTries_.pop_back();
        emit(Op::ExitTry);
        uint32_t jOver = emitJump(Op::Jump);
        fn_->code[enterAt].a = here(); // handler 入口
        compileStmtList(st.handlerBody);
        patchJump(jOver);
        return;
    }

    case Stmt::Kind::Import:
    case Stmt::Kind::FuncDef:
        return; // 声明在模块级处理
    }
}

} // namespace bc
} // namespace scourge
