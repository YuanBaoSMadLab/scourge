// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 类型检查器 · 字面量、索引与属性
// ----------------------------------------------------------------------------
//  职责边界：checkListLit / checkDictLit / checkSetLit / checkTupleLit / checkIndex / checkAttr（跨模块点分路径在此消费 qualifiedModuleName）。
//  其余阶段见同目录 TypeChecker*.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/Sema/TypeChecker.hpp"

#include "scourge/Sema/TypeRegistry.hpp"

#include <cmath>
#include <functional>
#include <set>

namespace scourge {

TypePtr TypeChecker::checkListLit(const Expr& e) {
    auto& ll = static_cast<const ListLit&>(e);
    // 空列表：Tensor 上下文沿用 Unknown（注解对齐交给赋值层）；否则以
    // List[Unknown] 占位，由首次写入/实参/返回值精化元素类型。
    if (ll.elems.empty())
        return asTensorLit_ == &e ? tyUnknown()
                                  : makeType(Type::Kind::List, {tyUnknown()});

    // 递归推断：数值/张量嵌套 → 张量字面量；其余 → List[T]。
    // 嵌套行是内层 ListLit——A 案默认已翻转为 List，若不干预外层将收不到
    // Tensor 行而整体塌成 List[List]（AOT 不可表示）。故检查元素时对
    // ListLit 行临时置张量上下文（保存/恢复外层旗标，支持任意深度嵌套）。
    std::vector<TypePtr> ts;
    const Expr* savedTensorCtx = asTensorLit_;
    for (const auto& el : ll.elems) {
        if (el->kind == Expr::Kind::ListLit) asTensorLit_ = el.get();
        ts.push_back(checkExpr(*el));
    }
    asTensorLit_ = savedTensorCtx;

    bool allNumeric = true, allTensor = true;
    std::vector<int64_t> rowShape;
    for (size_t i = 0; i < ts.size(); ++i) {
        TypePtr t = ts[i];
        if (t->kind == Type::Kind::Err) return tyErr();
        allNumeric &=
            (t->kind == Type::Kind::Int || t->kind == Type::Kind::Float);
        if (t->kind == Type::Kind::Tensor) {
            if (i == 0) rowShape = t->shape;
            else if (rowShape != t->shape) {
                return fail(Errc::E2004_ShapeMismatch, e.range,
                            "张量字面量各行形状不一致：第 1 行与第 " +
                                std::to_string(i + 1) + " 行");
            }
        } else {
            allTensor = false;
        }
    }

    // F-18 A 案（0.9.10）：一维数值字面量默认 List[T]——训练语料里
    // [1,2,3] 恒是列表；Tensor 上下文（Tensor 注解 / 既有 Tensor 变量
    // 重赋值，asTensorLit_ 旗标）或嵌套字面量才推断为张量。int/float
    // 混排统一为 float，int 元素打 Q3 提升标记（与 AOT genListLit 按
    // 元素种类统一对齐；VM 路径经 promoteF64_ 集合插 IntToFloat）。
    if (allNumeric && &e != asTensorLit_) {
        TypePtr elem = tyInt();
        for (auto& t : ts)
            if (t->kind == Type::Kind::Float) elem = tyFloat();
        if (elem->kind == Type::Kind::Float) {
            for (size_t i = 0; i < ts.size(); ++i)
                if (ts[i]->kind == Type::Kind::Int)
                    promoteF64_.insert(ll.elems[i].get());
        }
        return makeType(Type::Kind::List, {elem});
    }
    if (allNumeric) {
        TypePtr elem = tyInt();
        for (auto& t : ts)
            if (t->kind == Type::Kind::Float) elem = tyFloat();
        ll.checkedAsTensor = true; // 编译器编为 NewTensor（运行时构造）
        return makeTensor(elem, {static_cast<int64_t>(ts.size())});
    }
    if (allTensor) {
        TypePtr elem = ts[0]->params[0];
        for (auto& t : ts) {
            if (!typeEquals(elem, t->params[0])) {
                if (TypePtr u = unifyNumeric(elem, t->params[0])) elem = u;
            }
        }
        std::vector<int64_t> sh;
        sh.push_back(static_cast<int64_t>(ts.size()));
        for (int64_t d : rowShape) sh.push_back(d);
        ll.checkedAsTensor = true; // 编译器编为 NewTensor（运行时构造）
        return makeTensor(elem, sh);
    }

    // List[T]：元素类型统一
    TypePtr elem = ts[0];
    for (size_t i = 1; i < ts.size(); ++i) {
        if (typeEquals(elem, ts[i])) continue;
        if (TypePtr u = unifyNumeric(elem, ts[i])) {
            elem = u;
            continue;
        }
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "列表元素类型不一致：" + typeToString(elem) + " 与 " +
                        typeToString(ts[i]));
    }
    return makeType(Type::Kind::List, {elem});
}

TypePtr TypeChecker::checkDictLit(const Expr& e) {
    auto& dl = static_cast<const DictLit&>(e);
    if (dl.pairs.empty()) {
        // 空 {} 在 Set[T] 上下文 → 空集合（单一事实源 checkedAsSet，编译器
        // 据此编 NewSet(0)）；否则按注解推断为字典。返回新建 Set 类型——
        // 注解表持有，不能共享 setTarget_ 指向的注解本体所有权。
        if (setTarget_ && setTarget_->params.size() >= 1) {
            dl.checkedAsSet = true;
            return makeType(Type::Kind::Set, {setTarget_->params[0]});
        }
        // 空 {} 在 Dict[K,V] 上下文同理返回新建 Dict 类型：字面量节点必须
        // 登记真实 Dict 类型——AOT genDictLit 以节点类型定槽位 tag，若落
        // tyUnknown 则 ktag 误判 i64，str 键哈希退化为指针比较（跨对象
        // 查键全灭，VM/AOT 双模不一致）。
        if (dictTarget_ && dictTarget_->params.size() >= 2)
            return makeType(Type::Kind::Dict,
                            {dictTarget_->params[0], dictTarget_->params[1]});
        // 无注解上下文：Dict[Unknown,Unknown] 占位，由后续写入/实参/
        // 返回值精化键值类型（收尾仍未知则报 E2002）。
        return makeType(Type::Kind::Dict, {tyUnknown(), tyUnknown()});
    }

    std::vector<TypePtr> ks, vs;
    for (const auto& p : dl.pairs) {
        ks.push_back(checkExpr(*p.first));
        vs.push_back(checkExpr(*p.second));
    }
    // 键类型：限标量（int/float/bool/str），跨对统一（int∘float → float）。
    TypePtr kt = tyErr();
    for (size_t i = 0; i < ks.size(); ++i) {
        TypePtr k = ks[i];
        if (k->kind == Type::Kind::Err) return tyErr(); // 级联抑制
        if (k->kind != Type::Kind::Int && k->kind != Type::Kind::Float &&
            k->kind != Type::Kind::Bool && k->kind != Type::Kind::Str)
            return fail(Errc::E2001_TypeMismatch, dl.pairs[i].first->range,
                        "字典键必须是 int/float/bool/str，实际为 " +
                            typeToString(k));
        if (kt->kind == Type::Kind::Err) {
            kt = k;
        } else if (!typeEquals(kt, k)) {
            if (TypePtr u = unifyNumeric(kt, k)) {
                kt = u;
            } else {
                return fail(Errc::E2001_TypeMismatch, e.range,
                            "字典键类型不一致：" + typeToString(kt) + " 与 " +
                                typeToString(k));
            }
        }
    }
    // 值类型：跨对统一（int∘float → float；容器值需类型一致）。
    TypePtr vt = tyErr();
    for (size_t i = 0; i < vs.size(); ++i) {
        TypePtr v = vs[i];
        if (v->kind == Type::Kind::Err) return tyErr();
        if (vt->kind == Type::Kind::Err) {
            vt = v;
        } else if (!typeEquals(vt, v)) {
            if (TypePtr u = unifyNumeric(vt, v)) {
                vt = u;
            } else {
                return fail(Errc::E2001_TypeMismatch, e.range,
                            "字典值类型不一致：" + typeToString(vt) + " 与 " +
                                typeToString(v) +
                                "；Dict 值须同质（如 {\"a\": 1, \"b\": 2}），"
                                "异质载荷请拆容器或统一转换后再存");
            }
        }
    }
    // Dict 上下文偏好（单发旗标消费）：赋值/缺省值目标为 Dict[K,V] 时
    // 键/值统一对齐注解——字面量自推断 Dict{float,int} 与变量注解
    // Dict{float,float} 槽位语义脱节是双模漂移源；对齐后字面量注解 =
    // 变量注解（读取侧按变量注解位还原），被提升的 int 键/值打 Q3 标记。
    if (dictTarget_) {
        const Type& tg = *dictTarget_;
        if (isConvertible(kt, tg.params[0])) {
            if (tg.params[0]->kind == Type::Kind::Float)
                for (size_t i = 0; i < ks.size(); ++i)
                    if (ks[i]->kind == Type::Kind::Int)
                        promoteF64_.insert(dl.pairs[i].first.get());
            kt = tg.params[0];
        }
        if (isConvertible(vt, tg.params[1])) {
            if (tg.params[1]->kind == Type::Kind::Float)
                for (size_t i = 0; i < vs.size(); ++i)
                    if (vs[i]->kind == Type::Kind::Int)
                        promoteF64_.insert(dl.pairs[i].second.get());
            vt = tg.params[1];
        }
    }
    // 统一为 float 的 int 键/值做提升标记（Q3 边界，与 ListLit 同规则；
    // VM 路径插 IntToFloat，AOT genDictLit 按元素种类统一）。
    if (kt->kind == Type::Kind::Float)
        for (size_t i = 0; i < ks.size(); ++i)
            if (ks[i]->kind == Type::Kind::Int)
                promoteF64_.insert(dl.pairs[i].first.get());
    if (vt->kind == Type::Kind::Float)
        for (size_t i = 0; i < vs.size(); ++i)
            if (vs[i]->kind == Type::Kind::Int)
                promoteF64_.insert(dl.pairs[i].second.get());
    return makeType(Type::Kind::Dict, {kt, vt});
}

TypePtr TypeChecker::checkSetLit(const Expr& e) {
    auto& sl = static_cast<const SetLit&>(e);
    if (sl.elems.empty()) return tyUnknown(); // 防御：解析器不产空 SetLit

    std::vector<TypePtr> ts;
    for (const auto& el : sl.elems) ts.push_back(checkExpr(*el));
    // 元素类型：限标量（int/float/bool/str），跨元素统一（int∘float → float；
    // 语义与 Dict 键一致：数值跨类型同键去重，故元素类型必须可统一）。
    TypePtr et = tyErr();
    for (size_t i = 0; i < ts.size(); ++i) {
        TypePtr t = ts[i];
        if (t->kind == Type::Kind::Err) return tyErr(); // 级联抑制
        if (t->kind != Type::Kind::Int && t->kind != Type::Kind::Float &&
            t->kind != Type::Kind::Bool && t->kind != Type::Kind::Str)
            return fail(Errc::E2001_TypeMismatch, sl.elems[i]->range,
                        "集合元素必须是 int/float/bool/str，实际为 " +
                            typeToString(t));
        if (et->kind == Type::Kind::Err) {
            et = t;
        } else if (!typeEquals(et, t)) {
            if (TypePtr u = unifyNumeric(et, t)) {
                et = u;
            } else {
                return fail(Errc::E2001_TypeMismatch, e.range,
                            "集合元素类型不一致：" + typeToString(et) + " 与 " +
                                typeToString(t) +
                                "；Set 元素须同质（如 {1, 2, 3}），异质元素"
                                "请拆分容器或统一转换后再存");
            }
        }
    }
    // Set 上下文偏好（单发旗标消费）：赋值/缺省值目标为 Set[T] 时元素统一
    // 对齐注解——对齐后字面量注解 = 变量注解，被提升的 int 元素打 Q3 标记。
    if (setTarget_ && setTarget_->params.size() >= 1 &&
        isConvertible(et, setTarget_->params[0])) {
        if (setTarget_->params[0]->kind == Type::Kind::Float)
            for (size_t i = 0; i < ts.size(); ++i)
                if (ts[i]->kind == Type::Kind::Int)
                    promoteF64_.insert(sl.elems[i].get());
        et = setTarget_->params[0];
    }
    // 统一为 float 的 int 元素做提升标记（Q3 边界，与 Dict 键同规则；
    // VM 路径插 IntToFloat，AOT genSetLit 按元素种类统一）。
    if (et->kind == Type::Kind::Float)
        for (size_t i = 0; i < ts.size(); ++i)
            if (ts[i]->kind == Type::Kind::Int)
                promoteF64_.insert(sl.elems[i].get());
    return makeType(Type::Kind::Set, {et});
}

TypePtr TypeChecker::checkTupleLit(const Expr& e) {
    auto& tl = static_cast<const TupleLit&>(e);
    // 异构定长不可变：逐元素检查，元素白名单 = int/float/bool/str/None/
    // 嵌套元组。容器与 Result 元素显式拒绝——AOT 位槽 ABI 下 Tensor 的
    // tInt/tRank 元数据经位槽编解码会静默丢失，Result/Dict 的打印本就
    // AOT unsupported（拒绝即文档，与 0.9.3 Set 元素白名单同哲学）。
    std::vector<TypePtr> ts;
    bool hasErr = false;
    for (const auto& el : tl.elems) {
        TypePtr t = checkExpr(*el);
        switch (t->kind) {
        case Type::Kind::Int:
        case Type::Kind::Float:
        case Type::Kind::Bool:
        case Type::Kind::Str:
        case Type::Kind::None:
        case Type::Kind::Tuple:
            break;
        case Type::Kind::Err:
            hasErr = true; // 级联抑制：先收集完剩余元素诊断再整体报 Err
            continue;
        default:
            fail(Errc::E1007_UnsupportedFeature, el->range,
                 "元组元素类型不支持 " + typeToString(t) +
                     "（仅限 int/float/bool/str/None 与嵌套元组）");
            hasErr = true;
            continue;
        }
        ts.push_back(std::move(t));
    }
    if (hasErr) return tyErr();
    // Tuple 上下文偏好（0.9.9 批3①）：目标注解逐位对齐——int 元素遇
    // float 位打提升标记（Q3，promoteF64_ 单一事实源）并改写推断位；
    // 对齐后字面量注解 = 变量注解，AOT genTupleLit 按位消费防错位。
    if (tupleTarget_ && tupleTarget_->params.size() == ts.size()) {
        for (size_t i = 0; i < ts.size(); ++i) {
            if (tupleTarget_->params[i]->kind == Type::Kind::Float &&
                ts[i]->kind == Type::Kind::Int) {
                promoteF64_.insert(tl.elems[i].get());
                ts[i] = tyFloat();
            }
        }
    }
    return makeType(Type::Kind::Tuple, std::move(ts));
}

TypePtr TypeChecker::checkIndex(const Expr& e) {
    auto& ix = static_cast<const Index&>(e);
    TypePtr base = checkExpr(*ix.base);
    if (base->kind == Type::Kind::Err) return tyErr();
    // 切片 s[a:b]（0.9.9）：基型限 str/List；边界非空槽必须 int；返回
    // 基型本身（List[T]→List[T] 浅拷贝、Str→Str）。缺省端（nullptr）不查。
    if (ix.isSlice) {
        if (base->kind != Type::Kind::Str && base->kind != Type::Kind::List)
            return fail(Errc::E2001_TypeMismatch, e.range,
                        "仅 str 与 List 支持 s[a:b] 切片，实际为 " +
                            typeToString(base) + "（张量请用 reshape，"
                            "dict/Set 无序无切片）");
        for (const auto& idx : ix.indices) {
            if (!idx) continue; // 缺省端
            TypePtr it = checkExpr(*idx);
            if (it->kind != Type::Kind::Int && it->kind != Type::Kind::Err)
                return fail(Errc::E2001_TypeMismatch, idx->range,
                            "切片边界必须是 int，实际为 " +
                                typeToString(it));
        }
        return base;
    }
    if (base->kind == Type::Kind::Dict) {
        // 字典下标 = 键（K 类型，可 int→float 放宽）；其余容器仍限 int 下标。
        if (ix.indices.size() != 1)
            return fail(Errc::E2004_ShapeMismatch, e.range,
                        "dict 只支持单下标");
        TypePtr it = checkExpr(*ix.indices[0]);
        if (it->kind != Type::Kind::Err &&
            !isConvertible(it, base->params[0]))
            return fail(Errc::E2001_TypeMismatch, ix.indices[0]->range,
                        "字典键类型不匹配：需要 " +
                            typeToString(base->params[0]) + "，实际为 " +
                            typeToString(it));
        return base->params[1];
    }
    if (base->kind == Type::Kind::Set)
        return fail(Errc::E2001_TypeMismatch, e.range,
                    "Set 不支持下标（集合无序；成员判定用 'in'，遍历用 for）");
    if (base->kind == Type::Kind::Tuple) {
        // 元组定长：下标限编译期整数字面量，越界静态拒绝（运行期无任何
        // panic 路径——VM normIndex / AOT list_get_i 的越界分支均不可达）。
        if (ix.indices.size() != 1)
            return fail(Errc::E2004_ShapeMismatch, e.range, "元组只支持单下标");
        const Expr& ie = *ix.indices[0];
        if (ie.kind != Expr::Kind::IntLit)
            return fail(Errc::E2001_TypeMismatch, ie.range,
                        "元组下标必须是编译期整数字面量");
        const int64_t i = static_cast<const IntLit&>(ie).value;
        const int64_t n = static_cast<int64_t>(base->params.size());
        if (i >= n)
            return fail(Errc::E2004_ShapeMismatch, e.range,
                        "元组下标越界：长度 " + std::to_string(n) + "，实际 " +
                            std::to_string(i));
        return base->params[static_cast<size_t>(i)];
    }
    for (const auto& idx : ix.indices) {
        TypePtr it = checkExpr(*idx);
        if (it->kind != Type::Kind::Int && it->kind != Type::Kind::Err)
            fail(Errc::E2001_TypeMismatch, idx->range,
                 "下标必须是 int，实际为 " + typeToString(it));
    }

    if (base->kind == Type::Kind::Tensor) {
        size_t n = ix.indices.size();
        if (n > base->shape.size()) {
            return fail(Errc::E2004_ShapeMismatch, e.range,
                        "下标数量 " + std::to_string(n) + " 超过张量秩 " +
                            std::to_string(base->shape.size()));
        }
        std::vector<int64_t> rest(base->shape.begin() +
                                      static_cast<std::ptrdiff_t>(n),
                                  base->shape.end());
        if (rest.empty()) return base->params[0]; // 全索引 → 标量元素
        return makeTensor(base->params[0], rest);
    }
    if (base->kind == Type::Kind::List) {
        if (ix.indices.size() != 1) {
            return fail(Errc::E2004_ShapeMismatch, e.range,
                        "List 只支持单下标");
        }
        return base->params[0];
    }
    if (base->kind == Type::Kind::Str) return tyStr();
    return fail(Errc::E2001_TypeMismatch, ix.base->range,
                "该类型不支持下标访问：" + typeToString(base));
}

TypePtr TypeChecker::checkAttr(const Expr& e) {
    auto& at = static_cast<const Attr&>(e);
    TypePtr base = checkExpr(*at.base);
    if (base->kind == Type::Kind::Err) return tyErr();

    // Result 消费方法（错误即值的显式出口）。
    if (base->kind == Type::Kind::Result) {
        if (at.name == "is_ok" || at.name == "is_err")
            return makeFunction({}, tyBool());
        if (at.name == "unwrap" || at.name == "unwrap_or")
            return makeFunction({base->params[0]}, base->params[0]);
        return fail(Errc::E2003_UnknownName, e.range,
                    "Result 只有 is_ok / is_err / unwrap / unwrap_or 四个成员，无 '" +
                        at.name + "'");
    }

    // Option 消费方法（0.9.9 批3）：与 Result 同构（Option[T] ≡
    // Result[T,None]），执行侧映射同一 Method / lang.result_* 通道，
    // 方法名 is_some 区分语义面。
    if (base->kind == Type::Kind::Option) {
        if (at.name == "is_some" || at.name == "is_none")
            return makeFunction({}, tyBool());
        if (at.name == "unwrap" || at.name == "unwrap_or")
            return makeFunction({base->params[0]}, base->params[0]);
        return fail(Errc::E2003_UnknownName, e.range,
                    "Option 只有 is_some / is_none / unwrap / unwrap_or 四个成员，无 "
                        "'" + at.name + "'");
    }

    // str 成员方法（0.9.4）：纯函数签名表（唯一事实源）；实现分派在
    // VM callValueMethod 与 AOT lang.str_*（kLowerings 降级），panic 消息
    // 三方逐字一致。upper/lower 仅 ASCII；下标/查找按 UTF-8 字节。
    if (base->kind == Type::Kind::Str) {
        if (at.name == "upper" || at.name == "lower" || at.name == "trim" ||
            at.name == "trim_start" || at.name == "trim_end")
            return makeFunction({}, tyStr());
        if (at.name == "split")
            return makeFunction({tyStr()},
                                makeType(Type::Kind::List, {tyStr()}));
        if (at.name == "join")
            return makeFunction({makeType(Type::Kind::List, {tyStr()})},
                                tyStr());
        if (at.name == "replace")
            return makeFunction({tyStr(), tyStr()}, tyStr());
        if (at.name == "slice")
            return makeFunction({tyInt(), tyInt()}, tyStr());
        if (at.name == "find" || at.name == "index")
            return makeFunction({tyStr()}, tyInt());
        if (at.name == "contains" || at.name == "startswith" ||
            at.name == "endswith")
            return makeFunction({tyStr()}, tyBool());
        // 0.9.9：子串非重叠计数（空子串 = len+1，与 Python 一致）；复用
        // ListCount 枚举按 self.kind 分派（Contains/Index 先例）。
        if (at.name == "count") return makeFunction({tyStr()}, tyInt());
    }

    // List 成员方法（0.9.4）：contains/index/count，实参 = 元素类型
    // （int 键遇 float 列表经 isConvertible 放行，VM/AOT 各自提升 f64，
    // 与 'in' 运算符同一唯一放宽）。
    if (base->kind == Type::Kind::List && !base->params.empty()) {
        TypePtr elem = base->params[0];
        if (at.name == "contains") return makeFunction({elem}, tyBool());
        if (at.name == "index" || at.name == "count")
            return makeFunction({elem}, tyInt());
        // List.append(v) -> None：原地尾插（共享语义所有别名可见），
        // 非纯（修改状态，不进 @pure 白名单）。元素域 str/int/float/bool
        // （0.9.8 扩展标量全域；VM List 为 vector<Value> 天然异构，AOT
        // 按元素 VK 分发 list_append_* 四入口，空列表首插定型 etag）；
        // 批 9 F-21 增 Tuple（句柄 ptrtoint 存 i64 槽，items() 同先例）。
        if (at.name == "append") {
            Type::Kind k = elem->kind;
            // Unknown：空列表占位，元素类型由本次调用精化（checkCall）。
            if (k == Type::Kind::Str || k == Type::Kind::Int ||
                k == Type::Kind::Float || k == Type::Kind::Bool ||
                k == Type::Kind::Tuple || k == Type::Kind::Unknown)
                return makeFunction({elem}, tyNone());
        }
    }

    // Json 成员方法（0.9.4 M 专项）：动态 JSON 值的只读取值面。全部纯
    // 函数；at(i) 数组下标越界 panic、get(k) 对象缺键返回 null（两分：
    // 下标越界是编程错误，缺键是数据常态）。
    if (base->kind == Type::Kind::Json) {
        TypePtr retJson = makeType(Type::Kind::Json);
        if (at.name == "is_null" || at.name == "is_bool" ||
            at.name == "is_int" || at.name == "is_float" ||
            at.name == "is_str" || at.name == "is_list" ||
            at.name == "is_obj")
            return makeFunction({}, tyBool());
        if (at.name == "as_int") return makeFunction({}, tyInt());
        if (at.name == "as_float") return makeFunction({}, tyFloat());
        if (at.name == "as_bool") return makeFunction({}, tyBool());
        if (at.name == "as_str") return makeFunction({}, tyStr());
        if (at.name == "at") return makeFunction({tyInt()}, retJson);
        if (at.name == "get") return makeFunction({tyStr()}, retJson);
        if (at.name == "len") return makeFunction({}, tyInt());
        if (at.name == "keys")
            return makeFunction({},
                                makeType(Type::Kind::List, {tyStr()}));
        if (at.name == "dump") return makeFunction({}, tyStr());
    }

    // Dict 成员方法（0.9.9）：键/值/项视图（插入序）+ get_or（缺键取缺省
    // 值，不 panic——Result.unwrap_or 同哲学，恒 2 实参）。键值限标量域：
    // 容器/句柄 K/V 不进分支 → 落到未知成员报错（AOT 句柄槽无类型标签，
    // 双模同拒，与 print 容器值字典同防御哲学）。
    if (base->kind == Type::Kind::Dict && base->params.size() >= 2) {
        TypePtr kt = base->params[0], vt = base->params[1];
        // 空字典占位：键类型未定时只放行 contains（不返回依赖 V 的视图）。
        if (kt->kind == Type::Kind::Unknown && at.name == "contains")
            return makeFunction({tyUnknown()}, tyBool());
        if (isScalarType(kt) && isScalarType(vt)) {
            if (at.name == "keys")
                return makeFunction({}, makeType(Type::Kind::List, {kt}));
            if (at.name == "values")
                return makeFunction({}, makeType(Type::Kind::List, {vt}));
            if (at.name == "items")
                return makeFunction(
                    {},
                    makeType(Type::Kind::List,
                             {makeType(Type::Kind::Tuple, {kt, vt})}));
            if (at.name == "contains") return makeFunction({kt}, tyBool());
            if (at.name == "get") return makeFunction({kt, vt}, vt);
        }
    }

    // Set 成员方法（0.9.9）：add 原地插入（NaN panic 消息与构造同款）。
    if (base->kind == Type::Kind::Set && !base->params.empty()) {
        TypePtr et = base->params[0];
        if (isScalarType(et)) {
            if (at.name == "add") return makeFunction({et}, tyNone());
            if (at.name == "contains") return makeFunction({et}, tyBool());
        }
    }

    // 模块成员访问：foo.func / foo.CONST / 包路径中间段 a.b（0.9.6）。
    if (base->kind == Type::Kind::Module) {
        auto it = imports_.find(base->name);
        if (it != imports_.end()) {
            const ModuleSymbols& ms = *it->second;
            if (auto f = ms.funcs.find(at.name); f != ms.funcs.end()) {
                if (!checkVis(f->second.vis, ms.name, at.name, e.range))
                    return tyErr();
                std::vector<TypePtr> ps = f->second.params;
                ps.push_back(f->second.ret);
                return std::make_shared<Type>(Type::Kind::Function, ps);
            }
            if (auto g = ms.globals.find(at.name); g != ms.globals.end()) {
                // 模块级全局无装饰器挂点，恒 public（0.9.6 L 节）。
                return g->second;
            }
        }
        // 宽容路径重写（0.9.6）：import a.b 绑定首段 a → 真名 a.b，规范
        // 书写 a.b.CONST 的中间段 b 与真名尾段重合——重合段透传同一
        // Module 类型。与 qualifiedModuleName 同序（重写优先于下钻）。
        std::string suffix = '.' + at.name;
        if (base->name.size() > suffix.size() &&
            base->name.compare(base->name.size() - suffix.size(),
                               suffix.size(), suffix) == 0)
            return base;
        // 子模块链：base.name 与成员名拼接的全名在 registry 中存在即返回
        // Module 类型（下一段访问继续进入本分支逐级解析，支撑 a.b.c()）。
        std::string sub = base->name + "." + at.name;
        if (imports_.count(sub)) {
            auto mod = std::make_shared<Type>(Type::Kind::Module);
            mod->name = sub;
            return mod;
        }
        // F-19：同包多模块合并命名空间——子模块链缺失时回退包根（首段）
        // 重试：import std.autodiff + import std.nn 后 "std" 首段绑定指向
        // 首个导入模块，兄弟模块 std.nn 经此可达。回退目标必须真实导入过
        //（imports_），无幻影访问。
        size_t dot = base->name.find('.');
        if (dot != std::string::npos) {
            std::string alt = base->name.substr(0, dot) + '.' + at.name;
            if (imports_.count(alt)) {
                auto mod = std::make_shared<Type>(Type::Kind::Module);
                mod->name = alt;
                return mod;
            }
        }
        return fail(Errc::E2003_UnknownName, e.range,
                    "模块 '" + base->name + "' 没有成员 '" + at.name + "'");
    }

    return fail(Errc::E2003_UnknownName, e.range,
                "类型 " + typeToString(base) + " 没有成员 '" + at.name + "'");
}

} // namespace scourge
