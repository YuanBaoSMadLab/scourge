// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 字节码 VM · 指令分派
// ----------------------------------------------------------------------------
//  职责边界：dispatch——单指令分派（只使用 frames_.back()）：取数/存数/栈
//  操作/跳转/分支/循环/三元/断言/try/调用/返回/成员方法路由等。
//  其余阶段见同目录 VM*.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/BC/VM.hpp"
#include "scourge/BC/VMDetail.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_set>

#include "scourge/Common/Capability.hpp"

namespace scourge {
namespace bc {

// ----------------------------------------------------------------------------
// 指令分派
// ----------------------------------------------------------------------------

void VM::dispatch(Instr in) {
    Frame& f = frames_.back();

    switch (in.op) {
    case Op::LoadConst:
        push(program_.constants[in.a]);
        return;

    case Op::LoadLocal:
        if (in.a >= f.locals.size()) panicHere("VM 内部错误：局部槽越界");
        push(f.locals[in.a]);
        return;

    case Op::StoreLocal:
        if (in.a >= f.locals.size()) panicHere("VM 内部错误：局部槽越界");
        f.locals[in.a] = pop();
        return;

    case Op::LoadGlobal:
        if (in.a >= globals_->size()) panicHere("VM 内部错误：全局槽越界");
        push((*globals_)[in.a]);
        return;

    case Op::StoreGlobal:
        if (in.a >= globals_->size()) panicHere("VM 内部错误：全局槽越界");
        (*globals_)[in.a] = pop();
        return;

    case Op::Pop:
        pop();
        return;

    case Op::BinOp: {
        Value r = pop();
        Value l = pop();
        // Int×Int 快路径：标量循环/递归的热点，绕过 doBinOp 的通用类型分派链。
        // 覆盖 add/sub/mul（溢出 panic 文本与 doBinOp 逐字一致）与六种比较；
        // 除法族/Pow/And/Or/In 及非 Int 操作数一律走通用路径，语义零分叉。
        if (l.kind == Value::Kind::Int && r.kind == Value::Kind::Int) {
            const int64_t x = l.i, y = r.i;
            switch (static_cast<Bin>(in.a)) {
            case Bin::Add: {
                int64_t o;
                if (__builtin_add_overflow(x, y, &o)) panicHere("整数加法溢出");
                push(Value::makeInt(o));
                return;
            }
            case Bin::Sub: {
                int64_t o;
                if (__builtin_sub_overflow(x, y, &o)) panicHere("整数减法溢出");
                push(Value::makeInt(o));
                return;
            }
            case Bin::Mul: {
                int64_t o;
                if (__builtin_mul_overflow(x, y, &o)) panicHere("整数乘法溢出");
                push(Value::makeInt(o));
                return;
            }
            case Bin::Lt: push(Value::makeBool(x < y)); return;
            case Bin::Le: push(Value::makeBool(x <= y)); return;
            case Bin::Gt: push(Value::makeBool(x > y)); return;
            case Bin::Ge: push(Value::makeBool(x >= y)); return;
            case Bin::Eq: push(Value::makeBool(x == y)); return;
            case Bin::Ne: push(Value::makeBool(x != y)); return;
            default: break; // 其余走 doBinOp
            }
        }
        push(doBinOp(static_cast<Bin>(in.a), l, r));
        return;
    }

    case Op::UnaryOp: {
        Value v = pop();
        push(doUnaryOp(static_cast<Unary>(in.a), v));
        return;
    }

    case Op::Jump:
        f.pc = in.a;
        return;

    case Op::JumpIfFalse: {
        Value v = pop();
        if (!truthy(v)) f.pc = in.a;
        return;
    }

    case Op::JumpIfFalseOrPop:
    case Op::JumpIfTrueOrPop: {
        if (f.stack.empty()) panicHere("VM 内部错误：操作数栈下溢");
        bool t = truthy(f.stack.back());
        bool taken = in.op == Op::JumpIfFalseOrPop ? !t : t;
        if (taken)
            f.pc = in.a; // 短路：保留操作数作为结果值
        else
            f.stack.pop_back();
        return;
    }

    case Op::Call:
        callFunc(static_cast<int>(in.a), in.b);
        return; // frames_ 已变，勿再触碰 f

    case Op::CallMethod: {
        uint32_t n = in.b;
        if (f.stack.size() < static_cast<size_t>(n) + 1)
            panicHere("VM 内部错误：方法调用栈下溢");
        std::vector<Value> args(n);
        for (uint32_t k = n; k-- > 0;) args[k] = pop();
        Value self = pop();
        switch (static_cast<Method>(in.a)) {
        case Method::IsOk:
            // Option 三形态（0.9.9 批3，Option[T] ≡ Result[T,None] 同构）：
            // Result 句柄取 ok 位；None 值即空选项；裸标量即自动包装的
            // Some（赋值未包装，消费点动态判定）。Sema 仅在 Result/Option
            // 类型上分派此方法，其余形态不可达。
            if (self.kind == Value::Kind::Result)
                push(Value::makeBool(self.isOk));
            else if (self.kind == Value::Kind::None)
                push(Value::makeBool(false));
            else
                push(Value::makeBool(true));
            return;
        case Method::Unwrap:
        case Method::UnwrapOr:
            // 检查器 P0 语义：unwrap(缺省)/unwrap_or(缺省) 同形——必须携带
            // 1 个缺省值参数，ok 取载荷、失败态取缺省（无裸 unwrap 路径）。
            // 三形态与 IsOk 同规则：None → 缺省；裸标量 → 自身即 Some 载荷。
            if (args.size() != 1)
                panicHere("unwrap()/unwrap_or() 需要 1 个缺省值参数");
            if (self.kind == Value::Kind::None) {
                push(std::move(args[0]));
                return;
            }
            if (self.kind != Value::Kind::Result) {
                push(std::move(self));
                return;
            }
            if (self.isOk) {
                Value p = self;
                p.kind = self.payloadKind;
                push(std::move(p));
            } else {
                push(std::move(args[0]));
            }
            return;
        }
        // 0.9.4 str / List 成员方法：实现分块在 VMMethod.cpp（Result 三方法
        // 仍走上方直接分支，历史路径不动）。
        callValueMethod(static_cast<Method>(in.a), self, args);
        return;
    }

    case Op::Return:
        doReturn(pop());
        return;

    case Op::ReturnNone:
        doReturn(Value::makeNone());
        return;

    case Op::Print: {
        Value v = pop();
        std::string line = stringify(v);
        line += '\n';
        if (opts_.out)
            opts_.out(line);
        else
            std::fputs(line.c_str(), stdout);
        return;
    }

    case Op::NewList: {
        if (f.stack.size() < in.a) panicHere("VM 内部错误：NewList 栈下溢");
        auto vec = std::make_shared<std::vector<Value>>();
        vec->resize(in.a);
        for (size_t k = in.a; k-- > 0;) (*vec)[k] = pop();
        push(Value::makeList(std::move(vec)));
        return;
    }

    case Op::NewDict: {
        // 栈序 k1,v1,k2,v2,...（自顶向下成对弹出）→ 按源顺序插入（迭代序
        // = 插入序，确定性）；重复键后写覆盖先写（与 AOT dict_new 一致）。
        if (f.stack.size() < static_cast<size_t>(in.a) * 2)
            panicHere("VM 内部错误：NewDict 栈下溢");
        std::vector<std::pair<Value, Value>> ps(in.a);
        for (size_t k = in.a; k-- > 0;) {
            Value v = pop();
            Value key = pop();
            if (key.kind == Value::Kind::Float && std::isnan(key.f))
                panicHere("字典键不能为 NaN");
            ps[k] = {std::move(key), std::move(v)};
        }
        auto d = std::make_shared<DictData>();
        for (auto& p : ps) d->set(p.first, p.second);
        push(Value::makeDict(std::move(d)));
        return;
    }

    case Op::NewSet: {
        // 栈序 e1,e2,...（自顶向下弹出）→ 按源顺序插入去重（迭代序 =
        // 首插序，确定性）；元素语义 = Dict 键：数值跨类型同键、NaN 拒绝。
        if (f.stack.size() < in.a) panicHere("VM 内部错误：NewSet 栈下溢");
        std::vector<Value> es(in.a);
        for (size_t k = in.a; k-- > 0;) {
            Value el = pop();
            if (el.kind == Value::Kind::Float && std::isnan(el.f))
                panicHere("集合元素不能为 NaN");
            es[k] = std::move(el);
        }
        auto s = std::make_shared<SetData>();
        for (auto& e : es) s->add(e);
        push(Value::makeSet(std::move(s)));
        return;
    }

    case Op::NewTuple: {
        // 栈序 e1,e2,...（自顶向下弹出）→ 按源序装入（异构定长，无统一
        // 提升与去重；元素语义由检查器白名单保证）。
        if (f.stack.size() < in.a) panicHere("VM 内部错误：NewTuple 栈下溢");
        auto vec = std::make_shared<std::vector<Value>>();
        vec->resize(in.a);
        for (size_t k = in.a; k-- > 0;) (*vec)[k] = pop();
        push(Value::makeTuple(std::move(vec)));
        return;
    }

    case Op::LoadIndex: {
        const uint32_t n = in.b;
        if (n == 0) panicHere("VM 内部错误：下标数量为 0");
        if (f.stack.size() < static_cast<size_t>(n) + 1)
            panicHere("VM 内部错误：下标栈下溢");
        std::vector<Value> idxs(n);
        for (uint32_t k = n; k-- > 0;) idxs[k] = pop();
        Value base = pop();
        switch (base.kind) {
        case Value::Kind::List: {
            if (n != 1) panicHere("VM 内部错误：List 下标数量不符");
            if (!base.list) panicHere("VM 内部错误：空列表对象");
            int64_t len = static_cast<int64_t>(base.list->size());
            int64_t i = normIndex(toInt(idxs[0]), len, "列表");
            push((*base.list)[static_cast<size_t>(i)]);
            return;
        }
        case Value::Kind::Str: {
            if (n != 1) panicHere("VM 内部错误：Str 下标数量不符");
            // P1 按字节索引（UTF-8 多字节字符返回单字节切片）。
            int64_t len = static_cast<int64_t>(base.s.size());
            int64_t i = normIndex(toInt(idxs[0]), len, "字符串");
            push(Value::makeStr(
                std::string(1, base.s[static_cast<size_t>(i)])));
            return;
        }
        case Value::Kind::Range: {
            if (n != 1) panicHere("VM 内部错误：Range 下标数量不符");
            int64_t i = normIndex(toInt(idxs[0]), base.i, "range");
            push(Value::makeInt(i));
            return;
        }
        case Value::Kind::Tensor: {
            if (!base.tensor) panicHere("VM 内部错误：空张量对象");
            push(tensorIndexRead(base, idxs));
            return;
        }
        case Value::Kind::Dict: {
            if (n != 1) panicHere("VM 内部错误：Dict 下标数量不符");
            if (!base.dict) panicHere("VM 内部错误：空字典对象");
            const Value& key = idxs[0];
            if (key.kind == Value::Kind::Float && std::isnan(key.f))
                panicHere("字典键不能为 NaN");
            uint32_t e = base.dict->find(key);
            if (e == DictData::kEmpty) panicHere("字典键不存在");
            push(base.dict->entries[e].val);
            return;
        }
        case Value::Kind::Tuple: {
            // 检查器已强制：单下标 + 编译期整数字面量 + 静态越界——
            // 此处 n != 1 与负下标归一均不可达（防御兜底）。
            if (n != 1) panicHere("VM 内部错误：Tuple 下标数量不符");
            if (!base.list) panicHere("VM 内部错误：空元组对象");
            int64_t len = static_cast<int64_t>(base.list->size());
            int64_t i = normIndex(toInt(idxs[0]), len, "元组");
            push((*base.list)[static_cast<size_t>(i)]);
            return;
        }
        default:
            panicHere("该类型不支持下标读取");
        }
    }

    case Op::StoreIndex: {
        const uint32_t n = in.b;
        if (n == 0) panicHere("VM 内部错误：下标数量为 0");
        Value val = pop();
        if (f.stack.size() < static_cast<size_t>(n) + 1)
            panicHere("VM 内部错误：下标栈下溢");
        std::vector<Value> idxs(n);
        for (uint32_t k = n; k-- > 0;) idxs[k] = pop();
        Value base = pop();
        if (base.kind == Value::Kind::Tensor) {
            if (!base.tensor) panicHere("VM 内部错误：空张量对象");
            tensorIndexStore(base, idxs, val);
            return;
        }
        if (base.kind == Value::Kind::Dict) {
            if (n != 1) panicHere("VM 内部错误：Dict 下标数量不符");
            if (!base.dict) panicHere("VM 内部错误：空字典对象");
            const Value& key = idxs[0];
            if (key.kind == Value::Kind::Float && std::isnan(key.f))
                panicHere("字典键不能为 NaN");
            base.dict->set(key, std::move(val));
            return;
        }
        if (n != 1) panicHere("VM 内部错误：下标数量不符");
        if (base.kind != Value::Kind::List || !base.list)
            panicHere("下标赋值仅支持 list、dict 与 tensor");
        int64_t len = static_cast<int64_t>(base.list->size());
        int64_t i = normIndex(toInt(idxs[0]), len, "列表");
        (*base.list)[static_cast<size_t>(i)] = std::move(val);
        return;
    }

    case Op::Assert: {
        std::string msg;
        if (in.b) {
            Value m = pop();
            if (m.kind == Value::Kind::Str) msg = m.s;
        }
        Value c = pop();
        if (!truthy(c))
            panicHere(msg.empty() ? std::string("断言失败") : "断言失败：" + msg);
        return;
    }

    case Op::EnterTry:
        catches_.push_back(
            CatchPoint{frames_.size() - 1, in.a, f.stack.size()});
        return;

    case Op::ExitTry:
        if (catches_.empty() ||
            catches_.back().frameIndex != frames_.size() - 1)
            panicHere("VM 内部错误：ExitTry 无匹配 EnterTry");
        catches_.pop_back();
        return;

    case Op::MatMul: {
        Value r = pop();
        Value l = pop();
        push(tensorMatMul(l, r));
        return;
    }
    case Op::NewTensor:
        push(tensorNew(in.b));
        return;

    case Op::ForNext: {
        if (in.a >= f.locals.size() || in.b >= f.locals.size())
            panicHere("VM 内部错误：ForNext 槽越界");
        const Value& src = f.locals[in.a];
        int64_t c = f.locals[in.b].i;
        // 约定：有下一元素 → 压入元素 + true；耗尽 → 仅压入 false。
        // 元素与结束标志分离，元素本身为假值（0/空串）不会误判终止。
        if (src.kind == Value::Kind::Range) {
            if (c < src.i) {
                f.locals[in.b] = Value::makeInt(c + 1);
                push(Value::makeInt(c));
                push(Value::makeBool(true));
            } else {
                push(Value::makeBool(false));
            }
            return;
        }
        if (src.kind == Value::Kind::List && src.list) {
            int64_t len = static_cast<int64_t>(src.list->size());
            if (c < len) {
                f.locals[in.b] = Value::makeInt(c + 1);
                push((*src.list)[static_cast<size_t>(c)]);
                push(Value::makeBool(true));
            } else {
                push(Value::makeBool(false));
            }
            return;
        }
        if (src.kind == Value::Kind::Str) {
            // 字节遍历（与 str 下标同域：P1 按字节索引，非码点感知）。
            if (c < static_cast<int64_t>(src.s.size())) {
                f.locals[in.b] = Value::makeInt(c + 1);
                push(Value::makeStr(
                    std::string(1, src.s[static_cast<size_t>(c)])));
                push(Value::makeBool(true));
            } else {
                push(Value::makeBool(false));
            }
            return;
        }
        if (src.kind == Value::Kind::Dict) {
            if (!src.dict) panicHere("VM 内部错误：空字典对象");
            // 按插入序遍历键；迭代中新增键同样访问（追加式确定性）。
            if (c < static_cast<int64_t>(src.dict->entries.size())) {
                f.locals[in.b] = Value::makeInt(c + 1);
                push(src.dict->entries[static_cast<size_t>(c)].key);
                push(Value::makeBool(true));
            } else {
                push(Value::makeBool(false));
            }
            return;
        }
        if (src.kind == Value::Kind::Set) {
            if (!src.set) panicHere("VM 内部错误：空集合对象");
            // 按首插序遍历元素（语义与 Dict 键迭代一致）。
            if (c < static_cast<int64_t>(src.set->entries.size())) {
                f.locals[in.b] = Value::makeInt(c + 1);
                push(src.set->entries[static_cast<size_t>(c)]);
                push(Value::makeBool(true));
            } else {
                push(Value::makeBool(false));
            }
            return;
        }
        panicHere("for 迭代源仅支持 range、list、dict、Set 与 str");
    }

    case Op::IntToFloat: {
        // Q3 边界提升：int → float（存储/传参/返回边界的双模一致转换）。
        // 幂等：缺省值边界已在编译期（constOf）转为 f64，此处透传。
        Value v = pop();
        if (v.kind == Value::Kind::Int)
            push(Value::makeFloat(static_cast<double>(v.i)));
        else if (v.kind == Value::Kind::Float)
            push(v);
        else
            panicHere("VM 内部错误：IntToFloat 操作数不是数值");
        return;
    }
    }
    panicHere("VM 内部错误：未知指令");
}

} // namespace bc
} // namespace scourge
