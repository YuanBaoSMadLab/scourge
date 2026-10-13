// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge AOT — 进程内 MLIR → LLVM IR 转换实现（接口见 AOT/InProcess.hpp）
// ----------------------------------------------------------------------------
//  等价替换原两步子进程（mlir-opt --convert-to-llvm → mlir-translate
//  --mlir-to-llvmir）：parseSourceString → LangToLinalgPass（覆盖集张量
//  op → linalg）→ LangToLLVMPass（其余 lang op → @sg_* 调用）→
//  LinalgToLoops → SCFToCF → ConvertToLLVMPass（one-shot，memref/index/
//  arith/cf/func 经 ConvertToLLVMPatternInterface 全量降级）→
//  translateModuleToLLVMIR，诊断经 ScopedDiagnosticHandler 聚合返回。
//  方言注册与 LangToLinalg/LangToLLVM 产出的方言集一一对应（builtin/func/
//  arith/cf/llvm/memref/scf/linalg/lang）；one-shot 的依赖方言经 registry
//  扩展按需加载（与 mlir-opt 行为一致）。注意：SCF/Linalg 无 ToLLVM
//  接口，须先经独立转换 pass 降级；memref.expand_shape 本树无 ToLLVM
//  降级，LangToLinalg 一律用 reinterpret_cast。
//  注意：静态链接 E:/llvm 自建树的 Release MLIR 库，CRT 匹配考虑，进程内
//  MLIR 路径建议使用 Release 构建目录。
// ============================================================================
#include "scourge/AOT/InProcess.hpp"

#include <memory>
#include <system_error>

#include "mlir/Conversion/Passes.h"
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
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include "scourge/AOT/LangOps.h"
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
                    mlir::memref::MemRefDialect, mlir::scf::SCFDialect>();
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

    // 阶段 2a：lang.* 覆盖集张量 op（带静态形状属性）→ linalg（部分转换；
    // 未覆盖形态——int 张量、'/'、动态形状——保持 lang，交 LangToLLVM 兜底）
    // 阶段 2b：剩余 lang.* 高层 op → @sg_* 运行时调用（全量转换；未覆盖的
    // lang op 会使 applyFullConversion 失败——禁止静默残留）
    // 阶段 2c：linalg → scf（loops）→ cf（SCF/Linalg 无 ToLLVM 接口）
    // 阶段 2d：one-shot MLIR → LLVM dialect（等价 mlir-opt --convert-to-llvm）
    mlir::PassManager pm(&ctx);
    pm.addPass(scourge::aot::createLangToLinalgPass());
    pm.addPass(scourge::aot::createLangToLLVMPass());
    pm.addPass(mlir::createConvertLinalgToLoopsPass());
    pm.addPass(mlir::createSCFToControlFlowPass());
    pm.addPass(mlir::createConvertToLLVMPass());
    if (mlir::failed(pm.run(module->getOperation()))) {
        diag = diagBuf.empty() ? "MLIR → LLVM dialect 转换失败（无诊断）"
                               : diagBuf;
        return false;
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
