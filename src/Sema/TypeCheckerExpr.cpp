// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 类型检查器 · 表达式骨架与二元运算
// ----------------------------------------------------------------------------
//  职责边界：checkExpr（推断缓存入口）/ checkExprImpl / checkName / builtinType / tupleElemsComparable / checkBinary（双模算术纪律）/ iterElemOf / checkListComp。
//  其余阶段见同目录 TypeChecker*.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/Sema/TypeChecker.hpp"

#include <cmath>
#include <functional>
#include <set>

namespace scourge {

// ============================================================================
// 表达式检查
// ============================================================================

TypePtr TypeChecker::checkExpr(const Expr& e) {
    TypePtr t = checkExprImpl(e);
    // 单一事实源：推断结果按节点缓存（Err/Unknown 为已报错/推断失败占位，
    // 不入表——后端查不到时走自己的保守回退，行为与未接入前一致）。
    if (t && t->kind != Type::Kind::Err && t->kind != Type::Kind::Unknown)
        exprTypes_[&e] = t;
    return t;
}

TypePtr TypeChecker::checkExprImpl(const Expr& e) {
    switch (e.kind) {
    case Expr::Kind::IntLit: return tyInt();
    case Expr::Kind::FloatLit: return tyFloat();
    case Expr::Kind::BoolLit: return tyBool();
    case Expr::Kind::NoneLit: return tyNone();
    case Expr::Kind::StrLit: return tyStr();
    case Expr::Kind::Name: return checkName(e);
    case Expr::Kind::Unary: {
        auto& u = static_cast<const Unary&>(e);
        TypePtr t = checkExpr(*u.operand);
        if (u.op == "not") {
            if (t->kind != Type::Kind::Bool && t->kind != Type::Kind::Err)
                return fail(Errc::E2001_TypeMismatch, e.range,
                            "not 要求 bool，实际为 " + typeToString(t));
            return tyBool();
        }
        if (t->kind == Type::Kind::Int || t->kind == Type::Kind::Float ||
            t->kind == Type::Kind::Tensor || t->kind == Type::Kind::Err)
            return t;
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "一元运算符 '" + u.op + "' 不支持 " + typeToString(t));
    }
    case Expr::Kind::Binary:
        return checkBinary(e);
    case Expr::Kind::Ternary: {
        auto& t3 = static_cast<const Ternary&>(e);
        requireBool(checkExpr(*t3.cond), t3.cond->range, "三元条件");
        TypePtr a = checkExpr(*t3.thenE);
        TypePtr b = checkExpr(*t3.elseE);
        if (typeEquals(a, b)) return a;
        if (TypePtr u = unifyNumeric(a, b)) return u;
        if (a->kind == Type::Kind::Tensor && b->kind == Type::Kind::Tensor &&
            typeEquals(a->params[0], b->params[0])) {
            std::vector<int64_t> sh;
            if (unifyShape(a->shape, b->shape, sh))
                return makeTensor(a->params[0], sh);
        }
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "三元分支类型不一致：" + typeToString(a) + " 与 " +
                        typeToString(b));
    }
    case Expr::Kind::Call:
        return checkCall(e);
    case Expr::Kind::Attr:
        return checkAttr(e);
    case Expr::Kind::Index:
        return checkIndex(e);
    case Expr::Kind::ListLit:
        return checkListLit(e);
    case Expr::Kind::ListComp:
        return checkListComp(e);
    case Expr::Kind::DictLit:
        return checkDictLit(e);
    case Expr::Kind::SetLit:
        return checkSetLit(e);
    case Expr::Kind::Await: {
        // await 仅在 async def 内合法（E2010）；类型透传操作数
        // （Task 语义随 P2 并发落地，P0 阶段 await 是类型恒等）。
        auto& aw = static_cast<const Await&>(e);
        if (!inAsyncFn_) {
            return fail(Errc::E2010_AwaitOutsideAsync, e.range,
                        "await 只能出现在 async def 函数体内");
        }
        return checkExpr(*aw.operand);
    }
    case Expr::Kind::TupleLit:
        return checkTupleLit(e);
    }
    return tyErr();
}

TypePtr TypeChecker::checkName(const Expr& e) {
    auto& n = static_cast<const Name&>(e);
    if (checkingEnsures_ && n.name == "result") return fnRet_;

    if (const VarSym* v = scope_.find(n.name)) return v->type;
    if (localFuncs_.count(n.name))
        return fail(Errc::E1007_UnsupportedFeature, e.range,
                    "函数 '" + n.name + "' 不能作为值使用（当前仅支持调用）");

    if (TypePtr b = builtinType(n.name)) {
        // 能力记录（0.9.7）：需授权内建的每个调用名只记首个使用点，
        // check 收尾统一按域裁决（E5004）。
        Cap cap;
        if (capOfBuiltin(n.name, cap))
            capUses_.push_back({n.name, cap, n.range});
        return b;
    }

    std::string msg = "未声明的标识符 '" + n.name + "'";
    if (n.name == "result") msg += "（result 仅在 ensures 契约中可用）";
    fail(Errc::E2003_UnknownName, e.range, msg);
    noteSimilarName(n.name);
    return tyErr();
}

TypePtr TypeChecker::builtinType(const std::string& name) const {
    // ok/err：错误即值的构造出口（P0 内建，标准库 v1 冻结后归入 result 模块）。
    if (name == "print" || name == "str" || name == "len" || name == "sum" ||
        name == "now" || name == "range" || name == "ok" || name == "err" ||
        // F-20：panic(msg) 可调用终止（语句位恒不返回；与 assert 语句同钩子）
        name == "panic" ||
        name == "abs" || name == "sqrt" || name == "exp" || name == "log" ||
        name == "max" || name == "min" || name == "transpose" ||
        name == "reshape" ||
        // v0.9.3 标量数学/随机/排序（与 Compiler/MLIRGen builtinNames 同表）
        name == "sin" || name == "cos" || name == "tan" || name == "asin" ||
        name == "acos" || name == "atan" || name == "sinh" || name == "cosh" ||
        name == "tanh" || name == "atan2" || name == "floor" ||
        name == "ceil" || name == "round" || name == "trunc" ||
        name == "random" || name == "seed" || name == "sorted" ||
        // 0.9.4 IO 子系统：函数式文件/目录/标准输入（与 Compiler/MLIRGen 同表）
        name == "input" || name == "read_text" || name == "write_text" ||
        name == "append_text" || name == "exists" || name == "list_dir" ||
        // 0.9.4 F/G 专项：网络客户端（TCP/UDP/HTTP）与睡眠（同表）
        name == "net_connect" || name == "net_send" || name == "net_recv" ||
        name == "net_close" || name == "net_udp_send" ||
        name == "net_http_get" || name == "net_http_get_json" ||
        name == "sleep" ||
        name == "net_bind" || name == "net_accept" ||
        name == "net_set_timeout" ||
        // 0.9.8 HTTP 服务端（同表）
        name == "net_http_read_request" || name == "net_http_write_response" ||
        // 0.9.4 M 专项：标准库广度（统计/倒序/哈希；同表）
        name == "mean" || name == "var" || name == "std" ||
        name == "median" ||
        name == "reversed" || name == "sha256" || name == "crc32" ||
        // 0.9.4 M 专项：JSON（动态 Json 值；同表）
        name == "json_decode" ||
        // 0.9.4 H 专项：系统编程（args/env/时间/进程/信号；同表）
        name == "args" || name == "env" || name == "has_env" ||
        name == "set_env" || name == "unset_env" || name == "epoch" ||
        name == "epoch_ms" || name == "localtime" || name == "gmtime" ||
        name == "system" || name == "spawn_wait" || name == "signal_caught" ||
        name == "signal_reset" ||
        // 0.9.6 O 节 FFI 宽类型域：数据地址导出（同表）
        name == "data_ptr" ||
        // 真并发 M1（thread 域；同表）：spawn 静态校验在 checkBuiltinCall
        name == "thread_spawn" || name == "thread_join" ||
        name == "lock_new" || name == "lock_acquire" ||
        name == "lock_release" || name == "channel_new" ||
        name == "chan_send" || name == "chan_recv" || name == "chan_close" ||
        // GUI M1+M2（gui 域；同表）：句柄/事件语义在 checkBuiltinCall
        name == "gui_window" || name == "gui_close" || name == "gui_poll" ||
        name == "gui_label" || name == "gui_button" || name == "gui_input" ||
        name == "gui_set_text" || name == "gui_get_text" ||
        name == "gui_show" || name == "gui_hide" || name == "gui_title" ||
        // format()：首参字面量时 Parser 已降糖为 str()+concat 链，能到达
        // 此处的只有非字面量首参/空实参形态（checkBuiltinCall 定向报错）
        name == "format" ||
        // 0.9.9 类型转换/码点（同表）
        name == "int" || name == "float" || name == "ord" ||
        name == "chr" ||
        // fmt()：f-string :spec 降糖目标（同表；checkBuiltinCall 静态校验）
        name == "fmt") {
        auto t = std::make_shared<Type>(Type::Kind::Builtin);
        t->name = name;
        return t;
    }
    return nullptr;
}

/// 元组元素两两可比性（==/!= 语义，与 VM valuesEqual / AOT genTupleEq
/// 同规则）：数值族互通（按 double 比较）、双 str、None 对任意标量/元组
/// （异类恒 false）、嵌套元组递归同构。容器元素不可比——AOT 无容器相等
/// 先例，显式拒绝保持双模覆盖一致。
static bool tupleElemsComparable(const TypePtr& a, const TypePtr& b) {
    auto isNum = [](const TypePtr& t) {
        return t->kind == Type::Kind::Int || t->kind == Type::Kind::Float ||
               t->kind == Type::Kind::Bool;
    };
    if (isNum(a) && isNum(b)) return true;
    if (a->kind == Type::Kind::Str && b->kind == Type::Kind::Str) return true;
    if (a->kind == Type::Kind::None && b->kind == Type::Kind::None) return true;
    if (a->kind == Type::Kind::None || b->kind == Type::Kind::None)
        return true; // 异类 None：值恒 false（合法可比）
    if (a->kind == Type::Kind::Tuple && b->kind == Type::Kind::Tuple) {
        if (a->params.size() != b->params.size()) return false;
        for (size_t k = 0; k < a->params.size(); ++k)
            if (!tupleElemsComparable(a->params[k], b->params[k]))
                return false;
        return true;
    }
    return false;
}

TypePtr TypeChecker::checkBinary(const Expr& e) {
    auto& b = static_cast<const Binary&>(e);
    const std::string& op = b.op;
    TypePtr l = checkExpr(*b.lhs);
    TypePtr r = checkExpr(*b.rhs);
    if (l->kind == Type::Kind::Err || r->kind == Type::Kind::Err) return tyErr();

    auto failOp = [&](std::string extra) -> TypePtr {
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "运算符 '" + op + "' 不支持 " + typeToString(l) + " 与 " +
                        typeToString(r) + extra);
    };

    // 逻辑
    if (op == "and" || op == "or") {
        if (l->kind == Type::Kind::Bool && r->kind == Type::Kind::Bool)
            return tyBool();
        return failOp("（逻辑运算要求两侧均为 bool）");
    }

    // 成员测试
    if (op == "in") {
        if (r->kind == Type::Kind::List && isConvertible(l, r->params[0]))
            return tyBool();
        if (r->kind == Type::Kind::Str && l->kind == Type::Kind::Str)
            return tyBool();
        if (r->kind == Type::Kind::Dict && isConvertible(l, r->params[0]))
            return tyBool();
        if (r->kind == Type::Kind::Set && r->params.size() >= 1 &&
            isConvertible(l, r->params[0]))
            return tyBool();
        return failOp("");
    }

    // 矩阵乘（@）
    if (op == "@") {
        if (l->kind == Type::Kind::Tensor && r->kind == Type::Kind::Tensor) {
            if (l->shape.size() != 2 || r->shape.size() != 2) {
                return fail(Errc::E2004_ShapeMismatch, e.range,
                            "matmul（@）要求两个二维张量，实际 " +
                                typeToString(l) + " 与 " + typeToString(r));
            }
            if (l->shape[1] != kDyn && r->shape[0] != kDyn &&
                l->shape[1] != r->shape[0]) {
                return fail(Errc::E2004_ShapeMismatch, e.range,
                            "matmul 维度不匹配：" + typeToString(l) + " 与 " +
                                typeToString(r) + "（左列数 != 右行数）");
            }
            TypePtr elem = unifyNumeric(l->params[0], r->params[0]);
            if (!elem) elem = l->params[0];
            return makeTensor(elem, {l->shape[0], r->shape[1]});
        }
        return failOp("");
    }

    // 比较
    if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
        op == ">=") {
        if (l->kind == Type::Kind::Tensor || r->kind == Type::Kind::Tensor) {
            return failOp("（张量不支持整体比较；请用逐元素运算 + 归约显式表达）");
        }
        if (unifyNumeric(l, r)) return tyBool();
        if (l->kind == r->kind &&
            (l->kind == Type::Kind::Str || l->kind == Type::Kind::Bool))
            return tyBool();
        if ((op == "==" || op == "!=") &&
            (l->kind == Type::Kind::None || r->kind == Type::Kind::None))
            return tyBool();
        // 元组 ==/!=：仅同构元组（长度 + 逐元素可比性），运行时逐位展开。
        if ((op == "==" || op == "!=") && l->kind == Type::Kind::Tuple &&
            r->kind == Type::Kind::Tuple) {
            if (l->params.size() != r->params.size())
                return fail(Errc::E2001_TypeMismatch, e.range,
                            "元组比较长度不一致：" + typeToString(l) + " 与 " +
                                typeToString(r));
            for (size_t k = 0; k < l->params.size(); ++k) {
                if (!tupleElemsComparable(l->params[k], r->params[k]))
                    return fail(Errc::E2001_TypeMismatch, e.range,
                                "元组第 " + std::to_string(k + 1) +
                                    " 个元素不可比较：" +
                                    typeToString(l->params[k]) + " 与 " +
                                    typeToString(r->params[k]));
            }
            return tyBool();
        }
        return failOp("");
    }

    // 算术
    if (op == "+" || op == "-" || op == "*" || op == "//" || op == "%" ||
        op == "**" || op == "/") {
        // 张量逐元素（含标量广播）
        if (l->kind == Type::Kind::Tensor || r->kind == Type::Kind::Tensor) {
            TypePtr lt = l->kind == Type::Kind::Tensor ? l->params[0] : l;
            TypePtr rt = r->kind == Type::Kind::Tensor ? r->params[0] : r;
            TypePtr elem = unifyNumeric(lt, rt);
            if (!elem && typeEquals(lt, rt)) elem = lt;
            if (!elem) return failOp("（元素类型不一致）");
            // 真除法恒为 float（与标量 int/int → float 语义一致）。
            if (op == "/") elem = tyFloat();
            if (l->kind == Type::Kind::Tensor && r->kind == Type::Kind::Tensor) {
                std::vector<int64_t> sh;
                if (!unifyShape(l->shape, r->shape, sh)) {
                    return fail(Errc::E2004_ShapeMismatch, e.range,
                                "张量形状不兼容：" + typeToString(l) + " 与 " +
                                    typeToString(r));
                }
                return makeTensor(elem, sh);
            }
            const TypePtr& t = l->kind == Type::Kind::Tensor ? l : r;
            return makeTensor(elem, t->shape);
        }
        // 字符串
        if (l->kind == Type::Kind::Str || r->kind == Type::Kind::Str) {
            if (op == "+" && l->kind == Type::Kind::Str &&
                r->kind == Type::Kind::Str)
                return tyStr();
            if (op == "*" &&
                ((l->kind == Type::Kind::Str && r->kind == Type::Kind::Int) ||
                 (l->kind == Type::Kind::Int && r->kind == Type::Kind::Str)))
                return tyStr();
            return failOp("");
        }
        // 数值
        if (TypePtr u = unifyNumeric(l, r)) {
            if (op == "/") return tyFloat(); // 真除法恒为 float
            if (op == "//" || op == "%")
                return u->kind == Type::Kind::Int ? tyInt() : tyFloat();
            return u; // + - * **
        }
        return failOp("");
    }

    return failOp("");
}

TypePtr TypeChecker::iterElemOf(const TypePtr& it) const {
    if (it->kind == Type::Kind::Range) return tyInt();
    if (it->kind == Type::Kind::List && !it->params.empty())
        return it->params[0];
    if (it->kind == Type::Kind::Str)
        return tyStr(); // 字节遍历（与 str 下标同域：按字节，非码点感知）
    if (it->kind == Type::Kind::Dict && !it->params.empty())
        return it->params[0]; // 字典遍历键（插入序，与 VM/AOT 一致）
    if (it->kind == Type::Kind::Set && !it->params.empty())
        return it->params[0]; // 集合遍历元素（首插序，与 VM/AOT 一致）
    return tyErr();
}

TypePtr TypeChecker::checkListComp(const Expr& e) {
    // 列表推导式（0.9.8）：迭代源限 range/List/str（dict/Set 暂缓——
    // AOT 静态生成按源 VK 分支，先同拒保双模一致，后续按需扩）。
    auto& c = static_cast<const ListComp&>(e);
    TypePtr it = checkExpr(*c.iter);
    if (it->kind != Type::Kind::Range && it->kind != Type::Kind::List &&
        it->kind != Type::Kind::Str && it->kind != Type::Kind::Err)
        return fail(Errc::E2001_TypeMismatch, c.iter->range,
                    "推导式迭代源仅支持 range、List 或 str，实际为 " +
                        typeToString(it));
    TypePtr elem = iterElemOf(it);
    if (elem->kind == Type::Kind::Err) return tyErr();
    // 循环变量声明与 for 语句同构（Python 式复用：同名同型复用槽位，
    // 异型报错，否则新声明；推导式结束后变量残留与 for 语义一致）。
    if (const VarSym* ex = scope_.findInBack(c.var)) {
        if (!typeEquals(ex->type, elem))
            return fail(Errc::E2007_DuplicateDefinition, c.varRange,
                        "推导式循环变量 '" + c.var + "' 与本函数内已声明的 " +
                            typeToString(ex->type) + " 型变量冲突（类型 " +
                            typeToString(elem) + "）；请换用其他名称");
    } else {
        scope_.declare(c.var, VarSym{elem, c.varRange});
    }
    if (c.cond) {
        TypePtr ct = checkExpr(*c.cond);
        if (ct->kind != Type::Kind::Bool && ct->kind != Type::Kind::Err)
            return fail(Errc::E2001_TypeMismatch, c.cond->range,
                        "推导式过滤条件必须是 bool，实际为 " +
                            typeToString(ct));
    }
    TypePtr et = checkExpr(*c.elem);
    if (et->kind == Type::Kind::Err) return tyErr();
    return makeType(Type::Kind::List, {et});
}

} // namespace scourge
