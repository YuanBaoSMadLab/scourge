// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge AOT — 进程内 MLIR → LLVM IR 转换实现（接口见 AOT/InProcess.hpp）
// ----------------------------------------------------------------------------
//  等价替换原两步子进程（mlir-opt --convert-to-llvm → mlir-translate
//  --mlir-to-llvmir）：parseSourceString → LangToAffinePass（逐元素/归约/
//  转置覆盖集 → affine 循环）→ LangToLinalgPass（matmul 等其余覆盖集
//  张量 op → linalg）→ LangToLLVMPass（其余 lang op → @sg_* 调用）→
//  LinalgToLoops → LowerAffine → SCFToCF → ConvertToLLVMPass（one-shot，
//  memref/index/arith/cf/func 经 ConvertToLLVMPatternInterface 全量降级）→
//  无用户 unrealized_conversion_cast 清理（桥 cast 被框架穿透消费后失去
//  用户但 op 在 one-shot target 合法，会残留至 translate）→
//  translateModuleToLLVMIR，诊断经 ScopedDiagnosticHandler 聚合返回。
//  方言注册与 LangToAffine/LangToLinalg/LangToLLVM 产出的方言集一一对应
//  （builtin/func/arith/cf/llvm/memref/scf/affine/linalg/lang）；one-shot
//  的依赖方言经 registry 扩展按需加载（与 mlir-opt 行为一致）。注意：
//  SCF/Linalg/Affine 无 ToLLVM 接口，须先经独立转换 pass 降级；
//  memref.expand_shape 本树无 ToLLVM 降级，桥一律用 reinterpret_cast。
//  注意：静态链接 E:/llvm 自建树的 Release MLIR 库，CRT 匹配考虑，进程内
//  MLIR 路径建议使用 Release 构建目录。
// ============================================================================
#include "scourge/AOT/InProcess.hpp"

#include <memory>
#include <system_error>

#include "mlir/Conversion/Passes.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h" // 上游此快照无独立 Affine.h；方言声明经 AffineOpsDialect.h.inc 随此头提供
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h" // 上游此快照不提供独立方言头；方言声明经 FuncOpsDialect.h.inc 随此头提供
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/Passes.h" // createConvertLinalgToLoopsPass（Linalg Passes 组）
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/BuiltinDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/InitAllExtensions.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/ModuleTranslation.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include "scourge/AOT/LangOps.h"
#include "scourge/AOT/LangToAffine.hpp"
#include "scourge/AOT/LangToLLVM.hpp"
#include "scourge/AOT/LangToLinalg.hpp"

namespace scourge {
namespace aot {

bool convertToLLVMInProcess(const std::string& mlirText,
                            const std::string& llPath, std::string& diag) {
    mlir::DialectRegistry registry;
    registry.insert<mlir::BuiltinDialect, mlir::func::FuncDialect,
                    mlir::arith::ArithDialect, mlir::cf::ControlFlowDialect,
                    mlir::LLVM::LLVMDialect, mlir::linalg::LinalgDialect,
                    mlir::affine::AffineDialect, mlir::memref::MemRefDialect,
                    mlir::scf::SCFDialect>();
    // 自研 lang 方言（Scourge 高层语义 op，LangOps.td 单一事实源）。
    registry.insert<scourge::aot::lang::LangDialect>();
    // 全量方言扩展注册（等价 mlir-opt）：func 等方言承诺的
    // ConvertToLLVMPatternInterface 与内联扩展由此落位，缺注册会触发
    // "promised ... but never implemented" 的 LLVM ERROR。
    mlir::registerAllExtensions(registry);
    mlir::registerConvertToLLVMDependentDialectLoading(registry);
    mlir::registerLLVMDialectTranslation(registry);
    // builtin.module 的 LLVM 翻译接口（LLVM 15+ 自 llvm 翻译中独立成方言接口）
    mlir::registerBuiltinDialectTranslation(registry);

    mlir::MLIRContext ctx(registry, mlir::MLIRContext::Threading::DISABLED);

    // 诊断聚合：parse / pass / translate 的错误统一收集进 diagBuf。
    std::string diagBuf;
    mlir::ScopedDiagnosticHandler handler(&ctx, [&](mlir::Diagnostic& d) {
        llvm::raw_string_ostream os(diagBuf);
        os << d.str() << "\n";
        return mlir::success(); // 消费诊断，不向默认 handler 传播
    });

    auto module = mlir::parseSourceString<mlir::ModuleOp>(
        mlirText, mlir::ParserConfig(&ctx), "scourge");
    if (!module) {
        diag = diagBuf.empty() ? "MLIR 解析失败（无诊断）" : diagBuf;
        return false;
    }

    // 阶段 2a：lang.* 逐元素/归约/转置覆盖集（带静态形状属性）→ affine
    // 循环（部分转换；未覆盖形态——int 张量、'/'、动态形状——保持 lang）
    // 阶段 2b：lang.* 张量计算 op（matmul 等）→ linalg（部分转换，其余
    // 覆盖分支作回退）
    // 阶段 2c：剩余 lang.* 高层 op → @sg_* 运行时调用（全量转换；未覆盖的
    // lang op 会使 applyFullConversion 失败——禁止静默残留）
    // 阶段 2d：linalg → scf（loops）（SCF/Linalg 无 ToLLVM 接口）
    // 阶段 2e：affine → scf/memref/arith（lower-affine）
    // 阶段 2f：scf → cf
    // 阶段 2g：one-shot MLIR → LLVM dialect（等价 mlir-opt --convert-to-llvm）
    mlir::PassManager pm(&ctx);
    pm.addPass(scourge::aot::createLangToAffinePass());
    pm.addPass(scourge::aot::createLangToLinalgPass());
    pm.addPass(scourge::aot::createLangToLLVMPass());
    pm.addPass(mlir::createConvertLinalgToLoopsPass());
    pm.addPass(mlir::createLowerAffinePass());
    pm.addPass(mlir::createSCFToControlFlowPass());
    pm.addPass(mlir::createConvertToLLVMPass());
    if (mlir::failed(pm.run(module->getOperation()))) {
        diag = diagBuf.empty() ? "MLIR → LLVM dialect 转换失败（无诊断）"
                               : diagBuf;
        return false;
    }

    // 阶段 2h：清除转换后残留的死 cast。one-shot 对桥 cast 的消费者
    // （memref.load 等）降级时，框架穿透复用既有 unrealized_conversion_cast
    // （反向 materialization，直取 descriptor 本体），cast 随即失去全部用户；
    // 而该 op 在 one-shot target 中合法（框架自身 materialization 所需），
    // 不会被转换清理，残留至 translate 阶段将因无 LLVM 翻译接口而失败。
    // 纯 op 无副作用，循环清除无用户实例（链式死 cast 迭代至不动点）。
    for (;;) {
        llvm::SmallVector<mlir::Operation*> dead;
        module->walk([&](mlir::UnrealizedConversionCastOp cast) {
            if (cast.use_empty()) dead.push_back(cast);
        });
        if (dead.empty()) break;
        for (mlir::Operation* op : dead) op->erase();
    }

    // 阶段 3：LLVM dialect → LLVM IR（等价 mlir-translate --mlir-to-llvmir）
    llvm::LLVMContext llvmCtx;
    std::unique_ptr<llvm::Module> llvmModule = mlir::translateModuleToLLVMIR(
        module->getOperation(), llvmCtx, "ScourgeModule");
    if (!llvmModule) {
        diag = diagBuf.empty() ? "LLVM dialect → LLVM IR 翻译失败（无诊断）"
                               : diagBuf;
        return false;
    }

    if (llvm::verifyModule(*llvmModule, &llvm::errs())) {
        diag = "LLVM IR 校验失败";
        return false;
    }

    std::error_code ec;
    llvm::raw_fd_ostream out(llPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        diag = "写出 LLVM IR 失败：" + ec.message();
        return false;
    }
    llvmModule->print(out, nullptr);
    diag = diagBuf; // 成功路径上携带非致命诊断（通常为空），由调用方取舍
    return true;
}

} // namespace aot
} // namespace scourge
