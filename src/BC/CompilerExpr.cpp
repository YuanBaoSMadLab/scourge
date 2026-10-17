// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 字节码编译器 · 表达式（Compiler 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：compileExpr——字面量/名字/一元二元/比较/逻辑短路/三元/下标/
//  成员/调用/推导式等全量表达式 → SG-BC 指令发射（panic 语义与 VM 一致）。
//  从 Compiler.cpp 按生命周期拆出；语句与模块装配见 CompilerStmt.cpp 与
//  Compiler.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/BC/Compiler.hpp"
#include "scourge/BC/CompilerDetail.hpp"

#include <stdexcept>

namespace scourge {
namespace bc {


// ----------------------------------------------------------------------------
// 表达式
// ----------------------------------------------------------------------------

void Compiler::compileExpr(const Expr& e) {
    markLine(static_cast<uint32_t>(lineOf(e.range)));
    switch (e.kind) {
    case Expr::Kind::IntLit:
    case Expr::Kind::FloatLit:
    case Expr::Kind::BoolLit:
    case Expr::Kind::StrLit:
    case Expr::Kind::NoneLit:
        emit(Op::LoadConst, addConst(constOf(e)));
        return;

    case Expr::Kind::Name: {
        auto& n = static_cast<const Name&>(e);
        if (locals_) {
            auto it = locals_->slots.find(n.name);
            if (it != locals_->slots.end()) {
                emit(Op::LoadLocal, it->second);
                return;
            }
        }
        if (isBuiltinName(n.name) || lookupFunc(n.name) >= 0) {
            unsupported(e, "函数作为头等值将在 P1 提供；当前函数只能被调用");
        }
        // from-import 常量（0.9.6）：别名 → 源模块全局重定向。
        if (auto fg = fromGlobals_.find(n.name); fg != fromGlobals_.end()) {
            emit(Op::LoadGlobal, addGlobal(fg->second.first, fg->second.second));
            return;
        }
        // 模块级全局（含导入模块的常量经 Attr 走 LoadGlobal）
        emit(Op::LoadGlobal, addGlobal(moduleName_, n.name));
        return;
    }

    case Expr::Kind::Unary: {
        // bc::Unary 是操作码枚举，AST 节点需显式限定 scourge::Unary。
        auto& u = static_cast<const scourge::Unary&>(e);
        compileExpr(*u.operand);
        Unary op = u.op == "-"  ? Unary::Neg
                   : u.op == "+" ? Unary::Pos
                                 : Unary::Not;
        emit(Op::UnaryOp, static_cast<uint32_t>(op));
        return;
    }

    case Expr::Kind::Binary: {
        auto& b = static_cast<const Binary&>(e);
        // 短路求值：and/or 保留左值作为短路结果（与 AOT 语义一致）。
        if (b.op == "and") {
            compileExpr(*b.lhs);
            uint32_t j = emitJump(Op::JumpIfFalseOrPop);
            compileExpr(*b.rhs);
            patchJump(j);
            return;
        }
        if (b.op == "or") {
            compileExpr(*b.lhs);
            uint32_t j = emitJump(Op::JumpIfTrueOrPop);
            compileExpr(*b.rhs);
            patchJump(j);
            return;
        }
        compileExpr(*b.lhs);
        compileExpr(*b.rhs);
        // 矩阵乘：独立指令（运行时校验秩与内维，与检查器 E2004 同义）。
        if (b.op == "@") {
            emit(Op::MatMul);
            return;
        }
        Bin op = Bin::Add;
        if (b.op == "+") op = Bin::Add;
        else if (b.op == "-") op = Bin::Sub;
        else if (b.op == "*") op = Bin::Mul;
        else if (b.op == "/") op = Bin::Div;
        else if (b.op == "//") op = Bin::FloorDiv;
        else if (b.op == "%") op = Bin::Mod;
        else if (b.op == "**") op = Bin::Pow;
        else if (b.op == "==") op = Bin::Eq;
        else if (b.op == "!=") op = Bin::Ne;
        else if (b.op == "<") op = Bin::Lt;
        else if (b.op == "<=") op = Bin::Le;
        else if (b.op == ">") op = Bin::Gt;
        else if (b.op == ">=") op = Bin::Ge;
        else if (b.op == "in") op = Bin::In;
        else unsupported(e, "运算符 '" + b.op + "'");
        emit(Op::BinOp, static_cast<uint32_t>(op));
        return;
    }

    case Expr::Kind::Ternary: {
        auto& t = static_cast<const Ternary&>(e);
        compileExpr(*t.cond);
        uint32_t jFalse = emitJump(Op::JumpIfFalse);
        compileExpr(*t.thenE);
        uint32_t jEnd = emitJump(Op::Jump);
        patchJump(jFalse);
        compileExpr(*t.elseE);
        patchJump(jEnd);
        return;
    }

    case Expr::Kind::Call: {
        auto& c = static_cast<const Call&>(e);
        if (c.callee->kind == Expr::Kind::Attr) {
            auto& at = static_cast<const Attr&>(*c.callee);
            // 跨模块调用：m.func(args) / a.b.func(args)（0.9.6 点分链）。
            // 声明遍已为 import 模块建立函数索引（importedFuncs_），
            // 模块前缀经 foldModuleKey 折叠为键后按索引调用。
            std::string qm = foldModuleKey(*at.base);
            if (!qm.empty()) {
                auto mit = importedFuncs_.find(qm);
                if (mit != importedFuncs_.end()) {
                    auto fit = mit->second.find(at.name);
                    if (fit != mit->second.end()) {
                        for (const auto& a : c.args) {
                            compileExpr(*a);
                            if (needsF64(*a)) emit(Op::IntToFloat); // Q3
                        }
                        emit(Op::Call, static_cast<uint32_t>(fit->second),
                             static_cast<uint32_t>(c.args.size()));
                        return;
                    }
                }
            }
            // Result 消费方法：self 与实参依次压栈后 CallMethod。
            // 0.9.4 追加 str/List 成员方法（contains/index 同名——VM 运行时
            // 按 self.kind 分派；名称映射须与 Sema checkAttr 签名表一致）。
            Method m;
            if (at.name == "is_ok" || at.name == "is_some") m = Method::IsOk;
            else if (at.name == "unwrap") m = Method::Unwrap;
            else if (at.name == "unwrap_or") m = Method::UnwrapOr;
            else if (at.name == "upper") m = Method::StrUpper;
            else if (at.name == "lower") m = Method::StrLower;
            else if (at.name == "trim") m = Method::StrTrim;
            else if (at.name == "trim_start") m = Method::StrTrimStart;
            else if (at.name == "trim_end") m = Method::StrTrimEnd;
            else if (at.name == "split") m = Method::StrSplit;
            else if (at.name == "join") m = Method::StrJoin;
            else if (at.name == "replace") m = Method::StrReplace;
            else if (at.name == "find") m = Method::StrFind;
            else if (at.name == "slice") m = Method::StrSlice;
            else if (at.name == "contains") m = Method::Contains;
            else if (at.name == "index") m = Method::Index;
            else if (at.name == "startswith") m = Method::StrStartsWith;
            else if (at.name == "endswith") m = Method::StrEndsWith;
            else if (at.name == "count") m = Method::ListCount;
            else if (at.name == "append") m = Method::ListAppend;
            // 0.9.9：Dict/Set 成员方法（Sema checkAttr 按 base kind 分域）。
            // get/keys 与 Json 方法同名——BC 侧无 base 类型信息，名称映射
            // 统一发 Dict* 枚举值：Json 前置路由进 jsonMethod（内有转发
            // case），实参数差异由 Sema 按 base 分域校验（Json.get 1 参、
            // Dict.get 恒 2 参）。
            else if (at.name == "keys") m = Method::DictKeys;
            else if (at.name == "values") m = Method::DictValues;
            else if (at.name == "items") m = Method::DictItems;
            else if (at.name == "add") m = Method::SetAdd;
            // 0.9.4 M 专项：Json 成员方法（Sema checkAttr 已按 base kind
            // 分派；VM callValueMethod 按 self.kind==Json 路由到 VMJson.cpp）
            else if (at.name == "is_null") m = Method::JsonIsNull;
            else if (at.name == "is_bool") m = Method::JsonIsBool;
            else if (at.name == "is_int") m = Method::JsonIsInt;
            else if (at.name == "is_float") m = Method::JsonIsFloat;
            else if (at.name == "is_str") m = Method::JsonIsStr;
            else if (at.name == "is_list") m = Method::JsonIsList;
            else if (at.name == "is_obj") m = Method::JsonIsObj;
            else if (at.name == "as_int") m = Method::JsonAsInt;
            else if (at.name == "as_float") m = Method::JsonAsFloat;
            else if (at.name == "as_bool") m = Method::JsonAsBool;
            else if (at.name == "as_str") m = Method::JsonAsStr;
            else if (at.name == "at") m = Method::JsonAt;
            else if (at.name == "get") m = Method::DictGet;
            else if (at.name == "len") m = Method::JsonLen;
            else if (at.name == "dump") m = Method::JsonDump;
            else unsupported(e, "成员方法 '" + at.name + "'");
            compileExpr(*at.base); // self
            for (const auto& a : c.args) compileExpr(*a);
            emit(Op::CallMethod, static_cast<uint32_t>(m),
                 static_cast<uint32_t>(c.args.size()));
            return;
        }
        if (c.callee->kind != Expr::Kind::Name)
            unsupported(*c.callee, "该调用形式");
        auto& n = static_cast<const Name&>(*c.callee);
        int idx = lookupFunc(n.name);
        if (idx < 0) unsupported(e, "未解析的函数 '" + n.name + "'");
        for (const auto& a : c.args) {
            compileExpr(*a);
            if (needsF64(*a)) emit(Op::IntToFloat); // Q3 边界提升（实参）
        }
        emit(Op::Call, static_cast<uint32_t>(idx),
             static_cast<uint32_t>(c.args.size()));
        return;
    }

    case Expr::Kind::Attr: {
        auto& at = static_cast<const Attr&>(e);
        // 模块常量访问：m.CONST / a.b.CONST → LoadGlobal(规范模块名, 名)
        // （0.9.6 点分链；绑定键经 foldModuleKey 还原真实全限定名）。
        std::string qm = foldModuleKey(*at.base);
        if (!qm.empty()) {
            emit(Op::LoadGlobal, addGlobal(qm, at.name));
            return;
        }
        unsupported(e, "非调用位置的成员访问 '" + at.name + "'");
    }

    case Expr::Kind::Index: {
        auto& ix = static_cast<const Index&>(e);
        // 切片 s[a:b]（0.9.9）：压 4 个 int [start,end,hasA,hasB] 后
        // CallMethod Slice2——缺省端压 0 且 has 位 0（执行侧补端点），
        // 零新 opcode；str/List 分派在 VM callValueMethod / AOT 类型表。
        if (ix.isSlice) {
            // 实参弹出序 = 压栈逆序：先 base，再 start/end（缺省压 0），
            // 最后 hasA/hasB（缺省端 0）→ a=[start,end,hasA,hasB]。
            compileExpr(*ix.base);
            for (int k = 0; k < 2; ++k) {
                const Expr* ex = ix.indices[static_cast<size_t>(k)].get();
                if (ex)
                    compileExpr(*ex);
                else
                    emit(Op::LoadConst, addConst(Value::makeInt(0)));
            }
            for (int k = 0; k < 2; ++k) {
                const Expr* ex = ix.indices[static_cast<size_t>(k)].get();
                emit(Op::LoadConst, addConst(Value::makeInt(ex ? 1 : 0)));
            }
            emit(Op::CallMethod, static_cast<uint32_t>(Method::Slice2), 4);
            return;
        }
        // 张量支持多维（部分索引返回子张量）；List/Str/Range 仅单下标
        //（由检查器保证），b 携带下标个数。
        compileExpr(*ix.base);
        for (const auto& idx : ix.indices) compileExpr(*idx);
        emit(Op::LoadIndex, 0, static_cast<uint32_t>(ix.indices.size()));
        return;
    }

    case Expr::Kind::ListLit: {
        auto& ll = static_cast<const ListLit&>(e);
        // 单一事实源 = 检查器 checkedAsTensor：张量字面量（全常量 → 常量池；
        // 含运行时元素 → NewTensor 逐元素构造）；其余一律 NewList——
        // 含 0.9.3 List 数值元素路径（List 注解上下文的数值字面量）。
        if (ll.checkedAsTensor) {
            if (isTensorLiteral(ll)) {
                emit(Op::LoadConst, addConst(constTensorOf(ll)));
                return;
            }
            for (const auto& el : ll.elems) compileExpr(*el);
            emit(Op::NewTensor, 0, static_cast<uint32_t>(ll.elems.size()));
            return;
        }
        for (const auto& el : ll.elems) {
            compileExpr(*el);
            if (needsF64(*el)) emit(Op::IntToFloat); // Q3 List[float] 元素提升
        }
        emit(Op::NewList, static_cast<uint32_t>(ll.elems.size()));
        return;
    }

    case Expr::Kind::ListComp: {
        // 列表推导式（0.9.8）：空列表句柄 + 计数循环 + 条件 append，与
        // For 编译同构。迭代源/循环变量/列表句柄槽为推导式内部状态，不进
        // 名字映射（N2 同理：嵌套推导式互不覆写）；var 槽按名复用（Python
        // 复用语义，检查器已落声明）。表达式位置语法上不可能出现 break/
        // continue，无需循环栈。
        auto& c = static_cast<const ListComp&>(e);
        markLine(static_cast<uint32_t>(lineOf(e.range)));
        auto freshSlot = [&]() { return locals_->nlocals++; };
        emit(Op::NewList, 0);
        uint32_t lst = freshSlot();
        emit(Op::StoreLocal, lst);
        compileExpr(*c.iter);
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
        uint32_t var = slotOf(c.var);

        uint32_t top = here();
        emit(Op::ForNext, tmp, counter);
        uint32_t jEnd = emitJump(Op::JumpIfFalse);
        emit(Op::StoreLocal, var);
        uint32_t jSkip = 0;
        if (c.cond) {
            compileExpr(*c.cond);
            jSkip = emitJump(Op::JumpIfFalse);
        }
        emit(Op::LoadLocal, lst);
        compileExpr(*c.elem);
        if (needsF64(*c.elem)) emit(Op::IntToFloat); // Q3 元素提升
        emit(Op::CallMethod, static_cast<uint32_t>(Method::ListAppend), 1u);
        emit(Op::Pop); // 丢弃 append 的 None 返回值
        if (c.cond) patchJump(jSkip);
        emit(Op::Jump, top);
        patchJump(jEnd);
        emit(Op::LoadLocal, lst);
        return;
    }

    case Expr::Kind::DictLit: {
        auto& dl = static_cast<const DictLit&>(e);
        // 检查器判为空集合（Set[T] 注解上下文的空 {}）→ NewSet(0)
        //（单一事实源 checkedAsSet，与 checkedAsTensor 同架构）。
        if (dl.checkedAsSet) {
            emit(Op::NewSet, 0);
            return;
        }
        // 栈序 k1,v1,k2,v2,...；float 统一键/值的 int 元素做边界提升
        //（Q3，promoteF64_ 单一事实源，与 ListLit 同规则）。
        for (const auto& p : dl.pairs) {
            compileExpr(*p.first);
            if (needsF64(*p.first)) emit(Op::IntToFloat);
            compileExpr(*p.second);
            if (needsF64(*p.second)) emit(Op::IntToFloat);
        }
        emit(Op::NewDict, static_cast<uint32_t>(dl.pairs.size()));
        return;
    }

    case Expr::Kind::SetLit: {
        auto& sl = static_cast<const SetLit&>(e);
        // 栈序 e1,e2,...；float 统一元素的 int 元素做边界提升
        //（Q3，promoteF64_ 单一事实源，与 DictLit 同规则）。
        for (const auto& el : sl.elems) {
            compileExpr(*el);
            if (needsF64(*el)) emit(Op::IntToFloat);
        }
        emit(Op::NewSet, static_cast<uint32_t>(sl.elems.size()));
        return;
    }

    case Expr::Kind::TupleLit: {
        // 异构定长不可变元组：逐元素编译后 NewTuple 装箱（不做跨元素
        // float 统一提升，与检查器 checkTupleLit 白名单一致）；Tuple 上下文
        // 对齐位（批3① promoteF64_ 单一事实源）按元素 IntToFloat——与
        // AOT genTupleLit 按位 sitofp 同一语义，位槽形态双模一致。
        auto& tl = static_cast<const TupleLit&>(e);
        for (const auto& el : tl.elems) {
            compileExpr(*el);
            if (needsF64(*el)) emit(Op::IntToFloat); // Q3 位提升（批3①）
        }
        emit(Op::NewTuple, static_cast<uint32_t>(tl.elems.size()));
        return;
    }
    case Expr::Kind::Await:
        // await 类型透传（Task 语义 P2 落地）；VM 内同步执行。
        compileExpr(*static_cast<const Await&>(e).operand);
        return;
    }
    unsupported(e, "该表达式");
}

} // namespace bc
} // namespace scourge
