// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT MLIR 生成器实现（SG-AST → 文本 MLIR）
// ----------------------------------------------------------------------------
//  生成模型：
//  * 单一 MLIR 模块容纳全部依赖层序模块；MLIR 符号解析与顺序无关。
//  * 每个函数一份入口块（空 label）：常量、llvm.alloca、参数 store 全部
//    提升到入口块，随后的 body 块自 ^bb1 起——支配关系天然成立。
//  * 函数内变量复刻 Compiler 的渐进式局部槽：赋值时建槽、读取先局部后
//    全局，语义与字节码后端零分叉。
//  * panic 语义（溢出/除零/断言失败等）全部下沉到内嵌 C 运行时 sg_* 函数，
//    消息文本与 VM::doBinOp / doBuiltin 逐字一致。
//  * Result / List 与 Tensor 同构的堆句柄模型：lang.result_* / lang.list_*
//    op（LangToLLVM 的 kLowerings 降级）+ Value::lElem 记录元素/载荷 VK。
//  * try/except 是唯一的 func.call 直连运行时例外（控制流设施，与
//    llvm.alloca 同类）：sg_try_push/sg_try_pop 蹦床 + 生成代码直调 CRT
//    @_setjmp（SgTry.jb 首成员 → 句柄即 jmp_buf*；setjmp 必须在生成函数
//    自身活跃帧调用，包装帧方案会被同深度兄弟调用覆盖致死帧），处理器栈
//    簿记与 VM catches_ 逐一对应（EnterTry/ExitTry/recover
//    /doReturn 清理/break-continue 显式 ExitTry 穿越）。
//  * 不进入 AOT 的构造显式 E1007，无静默回退（全局容器/句柄槽自 0.9.10
//    起开放：undef 零值 + <top> 动态初始化，F-15）。
// ============================================================================
//
//  分块（按功能模块与生命周期，共享模型见 MLIRGenDetail.hpp）：
//    MLIRGen.cpp     本文件——驱动与模块装配
//    MLIRGenFunc.cpp 函数生命周期    MLIRGenExpr.cpp 表达式求值
//    MLIRGenLit.cpp  字面量与容器    MLIRGenCall.cpp  调用与内建
//    MLIRGenStmt.cpp 语句与控制流
// ============================================================================
#include "scourge/AOT/MLIRGen.hpp"

#include "scourge/AOT/MLIRGenDetail.hpp"

namespace scourge {
namespace aot {
namespace mlirgen {

// ============================================================================
// 模块级
// ============================================================================

void Gen::precollectGlobals() {
    // 遍历全部模块：登记全局槽并发射零值定义；函数签名 VK 预检。
    for (const auto& name : s_.modulesInDependencyOrder(entry_)) {
        const Module* m = s_.loadedModule(name);
        auto syms = s_.registry().find(name);
        if (!m || syms == s_.registry().end()) continue;
        curFile_ = s_.loadedFileId(name);
        auto& gm = globals_[name];

        for (const auto& stmt : m->body) {
            if (stmt->kind == Stmt::Kind::Assign) {
                auto& a = static_cast<const Assign&>(*stmt);
                if (a.target->kind != Expr::Kind::Name) continue;
                auto& n = static_cast<const Name&>(*a.target);
                auto gt = syms->second->globals.find(n.name);
                if (gt == syms->second->globals.end()) continue;
                VK vk = vkOfType(gt->second);
                if (vk == VK::Bad)
                    unsupported(a.target->range,
                                "全局变量 '" + name + "." + n.name + "' 的类型");
                GlobalSlot g{vk, globalSym(name, n.name)};
                // 容器/句柄元数据从注册表类型回填（loadGlobal 读出后
                // list_get_*/张量分派依赖；与函数参数回填同构）。
                const TypePtr& gt2 = gt->second;
                if (vk == VK::List && gt2 && gt2->kind == Type::Kind::List &&
                    !gt2->params.empty())
                    g.lElem = static_cast<uint8_t>(vkOfType(gt2->params[0]));
                if (vk == VK::Result && gt2 && !gt2->params.empty())
                    g.lElem = static_cast<uint8_t>(vkOfType(gt2->params[0]));
                if (vk == VK::Tensor && gt2 && gt2->kind == Type::Kind::Tensor) {
                    if (!gt2->params.empty())
                        g.tInt = gt2->params[0]->kind != Type::Kind::Float;
                    g.tRank = static_cast<uint8_t>(gt2->shape.size());
                }
                gm.emplace(n.name, g);
                switch (vk) {
                case VK::Int:
                case VK::Range:
                    globalDefs_.push_back("llvm.mlir.global internal " +
                                          globalSym(name, n.name) + "(0 : i64) : i64");
                    break;
                case VK::Float:
                    globalDefs_.push_back("llvm.mlir.global internal " +
                                          globalSym(name, n.name) + "(0.0 : f64) : f64");
                    break;
                case VK::Bool:
                    globalDefs_.push_back("llvm.mlir.global internal " +
                                          globalSym(name, n.name) + "(false) : i1");
                    break;
                case VK::Str: {
                    // LLVM dialect 中 ptr 全局的初始化器必须是同类型引用：
                    // 字面量初始值直接引用其字符串常量，否则引用空串占位
                    //（真正的赋值由 <top> 中的初始 store 完成）。
                    std::string init = internStr("");
                    if (a.value->kind == Expr::Kind::StrLit)
                        init = internStr(
                            static_cast<const StrLit&>(*a.value).strValue);
                    globalDefs_.push_back("llvm.mlir.global internal " +
                                          globalSym(name, n.name) + "(" + init +
                                          ") : !llvm.ptr");
                    break;
                }
                case VK::Tensor:
                case VK::Result:
                case VK::List:
                case VK::Dict:
                case VK::Set:
                case VK::Tuple:
                case VK::Json:
                    // 句柄全局：undef 零值定义（先写后读由执行序保证——
                    // 全部 <top> 先于 main；F-15：容器/张量字面量常量表
                    // 是 AI 高频写法，不再拒绝）。
                    globalDefs_.push_back("llvm.mlir.global internal " +
                                          globalSym(name, n.name) +
                                          "() : !llvm.ptr");
                    break;
                default:
                    break; // None：无存储
                }
            } else if (stmt->kind == Stmt::Kind::FuncDef) {
                auto& fd = static_cast<const FuncDef&>(*stmt);
                auto f = syms->second->funcs.find(fd.name);
                if (f == syms->second->funcs.end()) continue;
                // 泛型模板（批 9-③）：无具体 VK 可检，调用点按实例发射。
                if (f->second.hasUnannotated) continue;
                // async def 按普通 def 发射（await 恒等透传，Task 语义
                // 随 P2 并发落地；与 BC Compiler 声明遍同构，双模一致）。
                for (const auto& p : f->second.params)
                    if (vkOfType(p) == VK::Bad)
                        unsupported(fd.nameRange,
                                    "函数 '" + name + "." + fd.name + "' 的参数类型");
                if (vkOfType(f->second.ret) == VK::Bad)
                    unsupported(fd.nameRange,
                                "函数 '" + name + "." + fd.name + "' 的返回类型");
            }
        }
    }
}

void Gen::genModule(const std::string& name) {
    const Module* m = s_.loadedModule(name);
    if (!m) return;
    curModule_ = name;
    curFile_ = s_.loadedFileId(name);
    curFilePath_ = s_.loadedModulePath(name);
    curGlobals_ = &globals_[name];
    // import 绑定表（0.9.6 L 节）：键集合与解析序和 bc::Compiler 声明遍 1
    // 同构——非 from：全限定名 + 首段 + 末段宽容（或仅别名）；from：按
    // 函数 → 全局 → 子模块序展开（全未命中由检查器报 E5003）。
    modAlias_.clear();
    curFromGlobals_.clear();
    curFromFuncs_.clear();
    for (const auto& stmt : m->body) {
        if (stmt->kind != Stmt::Kind::Import) continue;
        auto& im = static_cast<const Import&>(*stmt);
        const std::string full = im.module();
        if (im.isFrom) {
            auto rit = s_.registry().find(full);
            for (const auto& item : im.items) {
                const std::string& bind =
                    item.alias.empty() ? item.name : item.alias;
                if (rit != s_.registry().end()) {
                    if (rit->second->funcs.count(item.name)) {
                        curFromFuncs_.emplace(bind,
                                              std::make_pair(full, item.name));
                        continue;
                    }
                    if (rit->second->globals.count(item.name)) {
                        curFromGlobals_.emplace(
                            bind, std::make_pair(full, item.name));
                        continue;
                    }
                }
                std::string sub = full + "." + item.name;
                if (s_.registry().count(sub)) modAlias_.emplace(bind, sub);
            }
            continue;
        }
        modAlias_.emplace(full, full);
        if (!im.alias.empty()) {
            modAlias_.emplace(im.alias, full); // import a.b as c：仅别名
            continue;
        }
        // Python 式首段绑定 + 末段宽容（冲突由检查器 E2007 把关）。
        if (!im.path.empty()) modAlias_.emplace(im.path.front(), full);
        if (im.path.size() > 1) modAlias_.emplace(im.path.back(), full);
    }

    // 遍一：函数；遍二：<top> 顶层赋值（与字节码两遍同构）。
    for (const auto& stmt : m->body)
        if (stmt->kind == Stmt::Kind::FuncDef)
            genFunction(*m, static_cast<const FuncDef&>(*stmt));
    genTopAssigns(*m);
}

void Gen::genFunction(const Module& m, const FuncDef& f) {
    auto ft = s_.registry().find(m.name);
    if (ft == s_.registry().end()) return;
    auto sig = ft->second->funcs.find(f.name);
    if (sig == ft->second->funcs.end()) return;
    if (sig->second.hasUnannotated)
        return; // 泛型模板（批 9-③）：不发射本体，调用点按实例发射
    if (sig->second.isExtern)
        return; // @extern：函数体不发射（调用点走 lang.extern_call，H 专项）

    deferStack_.clear(); // Gen 跨函数复用（含 extern 早退路径）；
                         // 控制流块均显式终结，函数体后当前块是唯一
                         // 隐式返回点
    std::vector<std::pair<std::string, VK>> params;
    for (size_t k = 0; k < f.params.size(); ++k)
        params.emplace_back(f.params[k].name, vkOfType(sig->second.params[k]));

    // MLIR 签名（None 形参无 SSA 表示，从签名中剔除，与调用点一致）。
    std::string sigTys;
    {
        bool first = true;
        for (size_t k = 0; k < params.size(); ++k) {
            if (params[k].second == VK::None) continue;
            if (!first) sigTys += ", ";
            first = false;
            sigTys += "%a" + std::to_string(k) + ": " + mlirTy(params[k].second);
        }
    }
    VK retVK = vkOfType(sig->second.ret);
    std::string symLine =
        "func.func " + funcSym(m.name, f.name) +
        (sigTys.empty() ? "()" : "(" + sigTys + ")") +
        (retVK == VK::None ? "" : " -> " + std::string(mlirTy(retVK)));

    startFunction(symLine, m.name + "." + f.name, retVK, /*isTop=*/false, params);
    // 张量参数：从签名回填 dtype/rank（allocSlot 只建槽与零值）
    for (size_t k = 0; k < f.params.size(); ++k) {
        if (params[k].second != VK::Tensor) continue;
        auto it = locals_.find(f.params[k].name);
        const TypePtr& pt = sig->second.params[k];
        if (it != locals_.end() && pt && pt->kind == Type::Kind::Tensor) {
            if (!pt->params.empty())
                it->second.tInt = pt->params[0]->kind != Type::Kind::Float;
            it->second.tRank = static_cast<uint8_t>(pt->shape.size());
        }
    }
    // List 参数：从签名回填元素 VK——list_get_*/append 的降级分派依赖
    // lElem，函数参数是跨函数边界后唯一可靠注解来源（缺失会把 List[str]
    // 参数误当 Int 元素发射，list_get_i 读 char** 全乱）。
    for (size_t k = 0; k < f.params.size(); ++k) {
        if (params[k].second != VK::List) continue;
        auto it = locals_.find(f.params[k].name);
        const TypePtr& pt = sig->second.params[k];
        if (it != locals_.end() && pt && pt->kind == Type::Kind::List &&
            !pt->params.empty())
            it->second.lElem = static_cast<uint8_t>(vkOfType(pt->params[0]));
    }
    deferStack_.clear(); // Gen 跨函数复用，先清残留（控制流块均显式终结，
                         // 函数体后当前块是唯一隐式返回点）
    genStmtList(f.body);
    emitDefers(); // 函数尾隐式 return 前展开全部 defer
    finishFunction();
}

void Gen::genTopAssigns(const Module& m) {
    startFunction("func.func " + topSym(m.name) + "()", "<top " + m.name + ">",
                  VK::None, /*isTop=*/true, {});
    deferStack_.clear(); // 顶层无 defer（检查器已限），防跨函数残留
    for (const auto& stmt : m.body)
        if (stmt->kind == Stmt::Kind::Assign) genStmt(*stmt);
    finishFunction();
}

void Gen::genMain() {
    const Module* em = s_.loadedModule(entry_);
    auto et = s_.registry().find(entry_);
    const FuncSig* mainSig = nullptr;
    SourceRange mainRange;
    if (et != s_.registry().end()) {
        auto f = et->second->funcs.find("main");
        if (f != et->second->funcs.end()) mainSig = &f->second;
    }
    if (em) {
        for (const auto& stmt : em->body)
            if (stmt->kind == Stmt::Kind::FuncDef &&
                static_cast<const FuncDef&>(*stmt).name == "main")
                mainRange = static_cast<const FuncDef&>(*stmt).nameRange;
    }

    if (mainSig && !mainSig->params.empty())
        unsupported(mainRange, "入口 main 不能带参数");

    curFile_ = s_.loadedFileId(entry_);
    std::string body;
    for (const auto& name : s_.modulesInDependencyOrder(entry_))
        body += "    func.call " + topSym(name) + "() : () -> ()\n";

    if (mainSig) {
        VK mainVK = vkOfType(mainSig->ret);
        if (mainVK == VK::Int) {
            body += "    %r = func.call " + funcSym(entry_, "main") +
                    "() : () -> i64\n";
            body += "    %c = arith.trunci %r : i64 to i32\n";
            body += "    func.return %c : i32\n";
        } else {
            std::string retTy =
                mainVK == VK::None ? "()" : std::string(mlirTy(mainVK));
            body += "    func.call " + funcSym(entry_, "main") + "() : () -> " +
                    retTy + "\n";
            body += "    %z = arith.constant 0 : i32\n";
            body += "    func.return %z : i32\n";
        }
    } else {
        // 与 VM::run 一致：无 main 静默跳过，退出码 0。
        body += "    %z = arith.constant 0 : i32\n";
        body += "    func.return %z : i32\n";
    }
    funcDefs_.push_back("  func.func @main() -> i32 {\n" + body + "  }\n");
}

void Gen::assemble(std::string& out) {
    out += "module {\n";
    out += "  // Scourge AOT 生成：入口 '" + entry_ + "'，模块 ";
    {
        bool first = true;
        for (const auto& name : s_.modulesInDependencyOrder(entry_)) {
            if (!first) out += ", ";
            first = false;
            out += name;
        }
    }
    out += "\n";
    if (!emptyStrDef_.empty()) out += "  " + emptyStrDef_ + "\n";
    for (const auto& d : strDefs_) out += "  " + d + "\n";
    for (const auto& d : globalDefs_) out += "  " + d + "\n";
    for (const auto& d : tensorConstDefs_) out += "  " + d + "\n";
    // try 蹦床：func.call 直连运行时（控制流设施例外——setjmp 双返回无法
    // 表达为无副作用 lang op；与 llvm.alloca 同类）。
    if (tryDeclsEmitted_) {
        out += "  func.func private @sg_try_push() -> !llvm.ptr\n";
        out += "  func.func private @sg_try_pop(!llvm.ptr) -> ()\n";
        // @_setjmp 双参 ABI（jmp_buf, frameaddress(0)）由 clang 前端
        // intrinsic 特判引入；@llvm.frameaddress.p0 经声明调用，LLVM 后端
        // 按 intrinsic 展开（无需库符号）。returns_twice 调用点属性由
        // InProcess 在 translate 后程序化补齐（LangRef 要求 call-site
        // 属性，声明 passthrough 仅作冗余标注）。
        out += "  func.func private @_setjmp(!llvm.ptr, !llvm.ptr) -> i32 "
               "attributes {passthrough = [\"returns_twice\"]}\n";
        out += "  func.func private @llvm.frameaddress.p0(i32) -> !llvm.ptr\n";
    }
    // 内嵌 C 运行时 @sg_* 外部声明已由 LangToLLVM pass 模块级补插
    // （lang 层不出现运行时符号——AOT 步骤 2 分层纪律）。
    for (const auto& f : funcDefs_) out += f;
    out += "}\n";
}

bool Gen::run(std::string& out) {
    try {
        precollectGlobals();
        for (const auto& name : s_.modulesInDependencyOrder(entry_))
            genModule(name);
        genMain();
        assemble(out);
    } catch (const GenAbort&) {
        return false;
    }
    return !s_.diag().hasErrors();
}
} // namespace mlirgen

// ============================================================================
// 对外入口
// ============================================================================

bool generateMLIR(Session& session, const std::string& entry, std::string& out) {
    mlirgen::Gen gen(session, entry, session.exprTypes());
    return gen.run(out);
}

} // namespace aot
} // namespace scourge
