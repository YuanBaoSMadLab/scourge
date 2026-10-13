// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT Lang 方言降级 pass（lang.* → @sg_* 运行时调用）
// ----------------------------------------------------------------------------
//  AOT 步骤 2：把 MLIRGen 产出的 Lang 高层语义 op 一一还原为对内嵌 C 运行时
//  @sg_* 函数的 func.call（ABI 与字节码 VM::doBinOp / doBuiltin 语义逐字一致）。
//
//  panic 上下文恢复：lang op 不携带 fn/file/line 操作数——
//  * fn：父 func 符号反解显示名（模块/函数名恒为语言标识符，符号化无损；
//    @sg_top_m → "<top m>"，@sg_m__f → "m.f"）；
//  * file / line：op 的 FileLineColLoc（MLIRGen 在每个带 panic ABI 的 lang op
//    文本尾部挂 loc("<file>":<line>:1)）；
//  * fn/file 字符串全局（llvm.mlir.global internal constant）按 func 缓存，
//    addressof 插入函数入口块——复刻原 MLIRGen panicCtx 的入口寄存器模型。
//
//  模块级补插全部 @sg_* 外部声明（kExterns 表搬家；未用符号无副作用）。
//  全量转换（applyFullConversion）：任何未覆盖的 lang op 都会使转换显式失败，
//  禁止静默残留——与"张量静默降级禁止"同一纪律。
// ============================================================================
#include "scourge/AOT/LangToLLVM.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "scourge/AOT/LangOps.h"

namespace scourge {
namespace aot {

namespace {

// ============================================================================
// 降级表：lang op → @sg_* 运行时符号（单一事实源，兼作声明补插表）
// ----------------------------------------------------------------------------
//  类型码：i=i64  f=f64  I=i32  b=i1  p=!llvm.ptr；返回码同 + 'v'=无返回。
//  args      ：lang 层形参（与 LangOps.td 的操作数一一对应）；
//  externArgs：sg_* 完整 ABI（hasCtx 时 = args + "ppi"，panic 例外——
//              sg_panic(fn, file, line, msg) 的上下文位于 msg 之前）。
// ============================================================================
struct OpLower {
    const char* lang;   ///< 全名 "lang.<mnemonic>"
    const char* sg;     ///< 运行时符号（不含 @）
    const char* args;   ///< lang 层形参类型码
    const char* externArgs; ///< sg_* 完整 ABI 类型码
    char ret;           ///< 返回类型码
    bool hasCtx;        ///< 调用是否追加 panic 上下文 (fn, file, line)
};

const OpLower kLowerings[] = {
    // ---- 标量整数算术 ----
    {"lang.addi", "sg_add_i64", "ii", "iippi", 'i', true},
    {"lang.subi", "sg_sub_i64", "ii", "iippi", 'i', true},
    {"lang.muli", "sg_mul_i64", "ii", "iippi", 'i', true},
    {"lang.divi", "sg_div_i64", "ii", "iippi", 'f', true},
    {"lang.floordivi", "sg_floordiv_i64", "ii", "iippi", 'i', true},
    {"lang.modi", "sg_mod_i64", "ii", "iippi", 'i', true},
    {"lang.powi", "sg_ipow_i64", "ii", "iippi", 'i', true},
    {"lang.negi", "sg_neg_i64", "i", "ippi", 'i', true},
    // ---- 标量浮点算术 ----
    {"lang.divf", "sg_fdiv_f64", "ff", "ffppi", 'f', true},
    {"lang.floordivf", "sg_floordiv_f64", "ff", "ffppi", 'f', true},
    {"lang.modf", "sg_fmod_f64", "ff", "ffppi", 'f', true},
    {"lang.powf", "sg_pow_f64", "ff", "ffppi", 'f', true},
    // ---- 字符串 ----
    {"lang.str_concat", "sg_str_concat", "pp", "pp", 'p', false},
    {"lang.str_repeat", "sg_str_repeat", "pi", "pippi", 'p', true},
    {"lang.str_cmp", "sg_str_cmp", "pp", "pp", 'i', false},
    {"lang.str_contains", "sg_str_contains", "pp", "pp", 'I', false},
    {"lang.str_len", "sg_len_str", "p", "p", 'i', false},
    {"lang.str_from_i64", "sg_str_from_i64", "i", "i", 'p', false},
    {"lang.str_from_f64", "sg_str_from_f64", "f", "f", 'p', false},
    {"lang.str_from_bool", "sg_str_from_bool", "b", "b", 'p', false},
    {"lang.str_from_tensor", "sg_str_from_tensor", "p", "p", 'p', false},
    // ---- 数学内建 ----
    {"lang.abs_i", "sg_abs_i64", "i", "i", 'i', false},
    {"lang.abs_f", "sg_abs_f64", "f", "f", 'f', false},
    {"lang.sqrt", "sg_sqrt_f64", "f", "f", 'f', false},
    {"lang.exp", "sg_exp_f64", "f", "f", 'f', false},
    {"lang.log", "sg_log_f64", "f", "f", 'f', false},
    {"lang.now", "sg_now", "", "", 'f', false},
    // ---- 打印 ----
    {"lang.print_i64", "sg_print_i64", "i", "i", 'v', false},
    {"lang.print_f64", "sg_print_f64", "f", "f", 'v', false},
    {"lang.print_bool", "sg_print_bool", "b", "b", 'v', false},
    {"lang.print_str", "sg_print_cstr", "p", "p", 'v', false},
    {"lang.print_none", "sg_print_none", "", "", 'v', false},
    {"lang.print_space", "sg_print_space", "", "", 'v', false},
    {"lang.print_nl", "sg_print_nl", "", "", 'v', false},
    {"lang.print_tensor", "sg_print_tensor", "p", "p", 'v', false},
    // ---- panic（上下文在 msg 之前的唯一特例） ----
    {"lang.panic", "sg_panic", "p", "ppip", 'v', true},
    // ---- 张量 ----
    {"lang.tensor_empty", "sg_tensor_empty", "", "", 'p', false},
    {"lang.tensor_from_i64", "sg_tensor_from_i64", "ip", "ip", 'p', false},
    {"lang.tensor_from_f64", "sg_tensor_from_f64", "ip", "ip", 'p', false},
    {"lang.tensor_concat", "sg_tensor_concat", "ip", "ipppi", 'p', true},
    {"lang.tensor_binop_tt", "sg_tensor_binop_tt", "Ipp", "Ippppi", 'p', true},
    {"lang.tensor_binop_ts", "sg_tensor_binop_ts", "IpIif", "IpIifppi", 'p',
     true},
    {"lang.tensor_binop_st", "sg_tensor_binop_st", "IIifp", "IIifpppi", 'p',
     true},
    {"lang.tensor_matmul", "sg_tensor_matmul", "pp", "ppppi", 'p', true},
    {"lang.tensor_get_i", "sg_tensor_get_i", "pip", "pipppi", 'i', true},
    {"lang.tensor_get_f", "sg_tensor_get_f", "pip", "pipppi", 'f', true},
    {"lang.tensor_slice", "sg_tensor_slice", "pip", "pipppi", 'p', true},
    {"lang.tensor_set_i", "sg_tensor_set_i", "pipi", "pipippi", 'v', true},
    {"lang.tensor_set_f", "sg_tensor_set_f", "pipf", "pipfppi", 'v', true},
    {"lang.tensor_sum_i", "sg_tensor_sum_i", "p", "pppi", 'i', true},
    {"lang.tensor_sum_f", "sg_tensor_sum_f", "p", "p", 'f', false},
    {"lang.tensor_un", "sg_tensor_un", "Ip", "Ipppi", 'p', true},
    {"lang.tensor_reduce_i", "sg_tensor_reduce_i", "Ipp", "Ippppi", 'i', true},
    {"lang.tensor_reduce_f", "sg_tensor_reduce_f", "Ipp", "Ippppi", 'f', true},
    {"lang.tensor_transpose", "sg_tensor_transpose", "p", "pppi", 'p', true},
    {"lang.tensor_reshape", "sg_tensor_reshape", "pip", "pipppi", 'p', true},
};

const OpLower* findLowering(mlir::StringRef name) {
    for (const OpLower& l : kLowerings)
        if (name == l.lang) return &l;
    return nullptr;
}

mlir::Type typeForCode(mlir::MLIRContext* ctx, char c) {
    mlir::Builder b(ctx);
    switch (c) {
    case 'i': return b.getI64Type();
    case 'f': return b.getF64Type();
    case 'I': return b.getI32Type();
    case 'b': return b.getI1Type();
    case 'p': return mlir::LLVM::LLVMPointerType::get(ctx);
    default: return {};
    }
}

/// 符号净化：非 [A-Za-z0-9_] → '_'；发生过替换时追加确定性 FNV 短哈希防碰撞
///（与 MLIRGen::san 同构；文件路径 → 全局符号后缀用）。
std::string sanSym(const std::string& name) {
    bool clean = true;
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            clean = false;
            break;
        }
    if (clean) return name;
    uint32_t h = 2166136261u;
    for (char raw : name) {
        h ^= static_cast<unsigned char>(raw);
        h *= 16777619u;
    }
    std::string out;
    for (char c : name)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c
                                                                        : '_';
    char buf[16];
    std::snprintf(buf, sizeof(buf), "_h%08x", h);
    return out + buf;
}

/// 从 func 符号反解 panic 显示名（模块/函数名恒为语言标识符 → 符号化无损）。
std::string deriveDisp(const std::string& sym) {
    const std::string kTop = "sg_top_";
    if (sym.rfind(kTop, 0) == 0)
        return "<top " + sym.substr(kTop.size()) + ">";
    const std::string kFn = "sg_";
    if (sym.rfind(kFn, 0) == 0) {
        size_t p = sym.rfind("__");
        if (p != std::string::npos && p >= kFn.size())
            return sym.substr(kFn.size(), p - kFn.size()) + "." +
                   sym.substr(p + 2);
    }
    return sym;
}

// ============================================================================
// 降级 pattern：每个 lang op → func.call @sg_*
// ============================================================================

class LangCallRewrite : public mlir::ConversionPattern {
public:
    explicit LangCallRewrite(mlir::MLIRContext* ctx)
        : mlir::ConversionPattern(mlir::Pattern::MatchAnyOpTypeTag{}, 1, ctx) {}

    mlir::LogicalResult
    matchAndRewrite(mlir::Operation* op, mlir::ArrayRef<mlir::Value> operands,
                    mlir::ConversionPatternRewriter& rewriter) const override {
        const OpLower* l = findLowering(op->getName().getStringRef());
        if (!l) {
            op->emitError("未注册降级规则的 lang op");
            return mlir::failure();
        }
        mlir::Location loc = op->getLoc();

        // panic 上下文（fn/file/line）恢复
        mlir::Value fnVal, fileVal, lineVal;
        if (l->hasCtx &&
            failed(ensurePanicCtx(op, rewriter, fnVal, fileVal, lineVal)))
            return mlir::failure();

        mlir::SmallVector<mlir::Value> args;
        if (std::strcmp(l->sg, "sg_panic") == 0) {
            // sg_panic ABI：上下文位于 msg 之前
            args = {fnVal, fileVal, lineVal, operands[0]};
        } else {
            args.append(operands.begin(), operands.end());
            if (l->hasCtx) {
                args.push_back(fnVal);
                args.push_back(fileVal);
                args.push_back(lineVal);
            }
        }

        auto call = rewriter.create<mlir::func::CallOp>(
            loc, mlir::StringRef(l->sg), op->getResultTypes(), args);
        rewriter.replaceOp(op, call.getResults());
        return mlir::success();
    }

private:
    /// 恢复 op 所在函数的 panic 上下文寄存器（fn/file 入口块缓存 + 行号常量）。
    mlir::LogicalResult
    ensurePanicCtx(mlir::Operation* op,
                   mlir::ConversionPatternRewriter& rewriter, mlir::Value& fnVal,
                   mlir::Value& fileVal, mlir::Value& lineVal) const {
        auto loc = mlir::dyn_cast<mlir::FileLineColLoc>(op->getLoc());
        auto fn = op->getParentOfType<mlir::func::FuncOp>();
        if (!loc || !fn) {
            op->emitError("lang op 缺少 FileLineColLoc 或父函数，"
                          "无法恢复 panic 上下文");
            return mlir::failure();
        }

        auto it = ctxCache_.find(fn.getOperation());
        if (it == ctxCache_.end()) {
            mlir::MLIRContext* ctx = op->getContext();
            mlir::ModuleOp mod = fn->getParentOfType<mlir::ModuleOp>();
            mlir::Type ptrTy = mlir::LLVM::LLVMPointerType::get(ctx);
            std::string fnName = deriveDisp(fn.getName().str());
            std::string fileName = loc.getFilename().getValue().str();
            std::string fnSym = ".sg_pfn_" + fn.getName().str();
            std::string fileSym = ".sg_pfile_" + sanSym(fileName);

            mlir::OpBuilder::InsertionGuard guard(rewriter);
            rewriter.setInsertionPointToStart(mod.getBody());
            if (!mlir::SymbolTable::lookupSymbolIn(mod, fileSym))
                createStrGlobal(rewriter, loc, fileSym, fileName);
            if (!mlir::SymbolTable::lookupSymbolIn(mod, fnSym))
                createStrGlobal(rewriter, loc, fnSym, fnName);
            rewriter.setInsertionPointToStart(&fn.getBody().front());
            mlir::Value f = rewriter.create<mlir::LLVM::AddressOfOp>(
                loc, ptrTy, mlir::FlatSymbolRefAttr::get(ctx, fnSym));
            mlir::Value fl = rewriter.create<mlir::LLVM::AddressOfOp>(
                loc, ptrTy, mlir::FlatSymbolRefAttr::get(ctx, fileSym));
            it = ctxCache_.try_emplace(fn.getOperation(), f, fl).first;
        }
        fnVal = it->second.first;
        fileVal = it->second.second;
        lineVal = rewriter.create<mlir::arith::ConstantOp>(
            op->getLoc(), rewriter.getI64Type(),
            rewriter.getI64IntegerAttr(static_cast<int64_t>(loc.getLine())));
        return mlir::success();
    }

    /// 入口字符串全局：NUL 结尾的 i8 数组（internal constant，与原
    /// MLIRGen::internStr 产物等价）。
    void createStrGlobal(mlir::ConversionPatternRewriter& rewriter,
                         mlir::Location loc, const std::string& sym,
                         const std::string& text) const {
        mlir::MLIRContext* ctx = rewriter.getContext();
        auto i8Ty = rewriter.getI8Type();
        auto arrTy = mlir::LLVM::LLVMArrayType::get(i8Ty, text.size() + 1);
        rewriter.create<mlir::LLVM::GlobalOp>(
            loc, arrTy, /*isConstant=*/true, mlir::LLVM::Linkage::Internal, sym,
            mlir::StringAttr::get(ctx, text + '\0'));
    }

    /// func → 入口块 (fn, file) 寄存器缓存（单线程转换，pattern 生命周期内有效）。
    mutable llvm::DenseMap<mlir::Operation*,
                           std::pair<mlir::Value, mlir::Value>>
        ctxCache_;
};

} // namespace

// ============================================================================
// pass 主体
// ============================================================================

namespace {

class LangToLLVMPass
    : public mlir::PassWrapper<LangToLLVMPass,
                               mlir::OperationPass<mlir::ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LangToLLVMPass)

    mlir::StringRef getArgument() const override {
        return "scourge-lang-to-llvm";
    }
    mlir::StringRef getDescription() const override {
        return "Scourge Lang 方言降级为 @sg_* 运行时调用";
    }

    void runOnOperation() override {
        mlir::ModuleOp mod = getOperation();
        mlir::MLIRContext* ctx = &getContext();

        // 1) 模块级补插 @sg_* 外部声明（kExterns 表搬家；与原 MLIRGen 行为
        //    一致——全量插入，未用符号无副作用）。
        declareRuntimeExterns(mod, ctx);

        // 2) 全量转换：lang 非法，builtin/func/arith/cf/llvm 合法。
        mlir::ConversionTarget target(*ctx);
        target.addLegalDialect<mlir::BuiltinDialect, mlir::func::FuncDialect,
                               mlir::arith::ArithDialect,
                               mlir::cf::ControlFlowDialect,
                               mlir::LLVM::LLVMDialect>();
        target.addIllegalDialect<::scourge::aot::lang::LangDialect>();

        mlir::RewritePatternSet patterns(ctx);
        patterns.add<LangCallRewrite>(ctx);
        if (mlir::failed(
                mlir::applyFullConversion(mod, target, std::move(patterns))))
            signalPassFailure();
    }

private:
    static void declareRuntimeExterns(mlir::ModuleOp mod,
                                      mlir::MLIRContext* ctx) {
        mlir::OpBuilder b(ctx);
        mlir::Location loc = mod.getLoc();
        mlir::OpBuilder::InsertionGuard guard(b);
        b.setInsertionPointToStart(mod.getBody());
        for (const OpLower& l : kLowerings) {
            mlir::SmallVector<mlir::Type> argTys;
            for (const char* c = l.externArgs; *c; ++c)
                argTys.push_back(typeForCode(ctx, *c));
            mlir::SmallVector<mlir::Type> retTys;
            if (l.ret != 'v') retTys.push_back(typeForCode(ctx, l.ret));
            auto fn = b.create<mlir::func::FuncOp>(
                loc, l.sg, mlir::FunctionType::get(ctx, argTys, retTys));
            fn.setVisibility(mlir::SymbolTable::Visibility::Private);
        }
    }
};

} // namespace

std::unique_ptr<mlir::Pass> createLangToLLVMPass() {
    return std::make_unique<LangToLLVMPass>();
}

} // namespace aot
} // namespace scourge
