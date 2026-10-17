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
//  标量整数 add/sub/mul/neg 特例：IR 内联展开（llvm.sadd/ssub/smul.with.
//  overflow + 条件 panic 块）而非 @sg_* 调用——纯标量热路径的跨 TU 函数
//  调用 clang 无法内联，是 AOT 标量性能落后手写 C 的全部来源（实测
//  bench_loop 11 倍差距；内联后与 C 持平）。溢出消息文本与 VM doBinOp /
//  运行时 sg_*_i64 逐字一致，双模 panic 输出逐字节相同。
//
//  模块级补插全部 @sg_* 外部声明（kExterns 表搬家；未用符号无副作用）。
//  全量转换（applyFullConversion）：任何未覆盖的 lang op 都会使转换显式失败，
//  禁止静默残留——与"张量静默降级禁止"同一纪律。
// ============================================================================
#include "scourge/AOT/LangToLLVM.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include <array>

#include "mlir/Dialect/Affine/IR/AffineOps.h" // v23 快照无独立 Affine.h；方言声明随此头提供
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h" // cf::CondBranchOp（方言头只含声明）
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinDialect.h"
#include "mlir/IR/BuiltinOps.h" // UnrealizedConversionCastOp（thread_spawn 桥）
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "scourge/AOT/LangLowerings.hpp"
#include "scourge/AOT/LangOps.h"

#include "scourge/AOT/GpuCover.hpp"
#include "scourge/Common/Capability.hpp"

namespace scourge {
namespace aot {

namespace {

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

/// 形参侧类型码展开：'b' → i32。MS x64 下 i1 形参经 8 位子寄存器传递且
/// 不保证高位清零，而 C 运行时形参是 int（读全 32 位寄存器）——残留垃圾
/// 会把 false 读成 true（实测：xor 取反后的布尔经 sg_print_bool 暴露）。
/// 降级层统一 zext 到 i32 对齐 C ABI；返回方向 C 写 EAX 自动零扩展、
/// 调用方按 i1 读 AL，无垃圾问题——返回码保持 'b'→i1。
mlir::Type argTypeForCode(mlir::MLIRContext* ctx, char c) {
    return c == 'b' ? mlir::Builder(ctx).getI32Type() : typeForCode(ctx, c);
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
// panic 上下文共享辅助（LangCallRewrite / LangInlineIntRewrite 共用）
// ============================================================================

/// func → 入口块 (fn, file) 寄存器缓存（单线程转换，pattern 生命周期内有效）。
using PanicCtxCache =
    llvm::DenseMap<mlir::Operation*, std::pair<mlir::Value, mlir::Value>>;

/// 入口字符串全局：NUL 结尾的 i8 数组（internal constant，与原
/// MLIRGen::internStr 产物等价）。
void createStrGlobal(mlir::OpBuilder& b, mlir::Location loc,
                     const std::string& sym, const std::string& text) {
    mlir::MLIRContext* ctx = b.getContext();
    auto i8Ty = b.getI8Type();
    auto arrTy = mlir::LLVM::LLVMArrayType::get(i8Ty, text.size() + 1);
    b.create<mlir::LLVM::GlobalOp>(loc, arrTy, /*isConstant=*/true,
                                   mlir::LLVM::Linkage::Internal, sym,
                                   mlir::StringAttr::get(ctx, text + '\0'));
}

/// 恢复 op 所在函数的 panic 上下文寄存器（fn/file 入口块缓存 + 行号常量）。
mlir::LogicalResult
ensurePanicCtx(mlir::Operation* op, mlir::ConversionPatternRewriter& rewriter,
               PanicCtxCache& cache, mlir::Value& fnVal, mlir::Value& fileVal,
               mlir::Value& lineVal) {
    auto loc = mlir::dyn_cast<mlir::FileLineColLoc>(op->getLoc());
    auto fn = op->getParentOfType<mlir::func::FuncOp>();
    if (!loc || !fn) {
        op->emitError("lang op 缺少 FileLineColLoc 或父函数，"
                      "无法恢复 panic 上下文");
        return mlir::failure();
    }

    auto it = cache.find(fn.getOperation());
    if (it == cache.end()) {
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
        it = cache.try_emplace(fn.getOperation(), f, fl).first;
    }
    fnVal = it->second.first;
    fileVal = it->second.second;
    lineVal = rewriter.create<mlir::arith::ConstantOp>(
        op->getLoc(), rewriter.getI64Type(),
        rewriter.getI64IntegerAttr(static_cast<int64_t>(loc.getLine())));
    return mlir::success();
}

// ============================================================================
// 降级 pattern：标量整数 addi/subi/muli/negi 的 IR 内联展开
// ----------------------------------------------------------------------------
// 纯标量热路径（循环/递归体逐行执行）的跨 TU @sg_* 调用 clang 无法内联，
// 在 op 层直接展开为溢出 intrinsic + 条件分支 panic 块（panic 块仍调
// @sg_panic，fn/file/line 上下文与消息文本双模逐字一致）。取负的溢出
// 条件是 v == INT64_MIN（与 VM doNeg / sg_neg_i64 一致），结果 0 - v 在
// 非溢出路径无 UB。GPU 覆盖集不含标量算术（仅张量 op），无路径冲突。
// ============================================================================

/// ExtractValueOp 位置常量：{0}=结果，{1}=溢出标志。
constexpr int64_t kPosRes[] = {0};
constexpr int64_t kPosFlag[] = {1};

class LangInlineIntRewrite : public mlir::ConversionPattern {
public:
    explicit LangInlineIntRewrite(mlir::MLIRContext* ctx)
        : mlir::ConversionPattern(mlir::Pattern::MatchAnyOpTypeTag{}, 10, ctx) {
    }

    mlir::LogicalResult
    matchAndRewrite(mlir::Operation* op, mlir::ArrayRef<mlir::Value> operands,
                    mlir::ConversionPatternRewriter& rewriter) const override {
        const Kind kind = kindOf(op->getName().getStringRef());
        if (kind == Kind::Other)
            return mlir::failure(); // 非目标 op：让位 LangCallRewrite（低 benefit）
        // affine.for 循环体内禁止内联展开：本 pattern 依赖 splitBlock +
        // createBlock 建溢出 panic 分支，而 affine.for 要求 region 至多
        // 1 块（曾致 verify "expects region #0 to have 0 or 1 blocks"）。
        // 回退 LangCallRewrite 的 @sg_add_i64 等调用路径——张量路径
        // sg_tiop 同一实现，panic 消息与触发点零分叉（仅省一次跨 TU
        // 内联的优化机会，循环体逐元素语义不变）。
        if (op->getParentOfType<mlir::affine::AffineForOp>())
            return mlir::failure();
        mlir::Location loc = op->getLoc();
        mlir::MLIRContext* ctx = op->getContext();
        mlir::Type i64Ty = rewriter.getI64Type();
        mlir::Type i1Ty = rewriter.getI1Type();

        // 框架已把插入点置于 op 前——先取所在块与位置（后续 panic 辅助会
        // 挪动插入点；cf::AssertOp 降级同款结构）
        mlir::Block* opBlock = rewriter.getInsertionBlock();
        mlir::Block::iterator opPosition = rewriter.getInsertionPoint();

        // panic 上下文与溢出消息全局（消息常量在模块级；AddressOf 必须
        // 在函数入口块——与 fn/file 寄存器同模型，use 与定义同 region）
        mlir::Value fnVal, fileVal, lineVal;
        if (failed(ensurePanicCtx(op, rewriter, ctxCache_, fnVal, fileVal,
                                  lineVal)))
            return mlir::failure();
        mlir::Value msgVal = ensureMsgGlobal(op, rewriter, kind);

        // 分裂：opPosition 之后的一切（含 op 本体）进 contBlock；op 由
        // replaceOp 删除，其结果的使用替换为 opBlock 内的新计算值
        //（opBlock 经 cond_br 支配 contBlock，支配关系成立）
        mlir::Block* contBlock = rewriter.splitBlock(opBlock, opPosition);

        // panic 块（call @sg_panic + unreachable，region 末尾追加）
        mlir::Block* panicBlock = rewriter.createBlock(opBlock->getParent());
        rewriter.create<mlir::func::CallOp>(
            loc, "sg_panic", mlir::TypeRange{},
            mlir::ValueRange{fnVal, fileVal, lineVal, msgVal});
        rewriter.create<mlir::LLVM::UnreachableOp>(loc);

        // 溢出计算 + 标志（opBlock 尾部；随后 cond_br 接管终结符位置）
        rewriter.setInsertionPointToEnd(opBlock);
        mlir::Value flag; // true → panic
        mlir::Value res;  // 运算结果
        switch (kind) {
        case Kind::Add:
        case Kind::Sub:
        case Kind::Mul: {
            auto structTy = mlir::LLVM::LLVMStructType::getLiteral(
                ctx, {i64Ty, i1Ty}, /*isPacked=*/false);
            mlir::Value ovf;
            switch (kind) {
            case Kind::Add:
                ovf = rewriter.create<mlir::LLVM::SAddWithOverflowOp>(
                    loc, structTy, operands[0], operands[1]);
                break;
            case Kind::Sub:
                ovf = rewriter.create<mlir::LLVM::SSubWithOverflowOp>(
                    loc, structTy, operands[0], operands[1]);
                break;
            default:
                ovf = rewriter.create<mlir::LLVM::SMulWithOverflowOp>(
                    loc, structTy, operands[0], operands[1]);
                break;
            }
            res = rewriter.create<mlir::LLVM::ExtractValueOp>(loc, i64Ty, ovf,
                                                              kPosRes);
            flag = rewriter.create<mlir::LLVM::ExtractValueOp>(loc, i1Ty, ovf,
                                                               kPosFlag);
            break;
        }
        case Kind::Neg: {
            auto minC = rewriter.create<mlir::arith::ConstantOp>(
                loc, i64Ty,
                rewriter.getI64IntegerAttr(
                    std::numeric_limits<int64_t>::min()));
            auto zero = rewriter.create<mlir::arith::ConstantOp>(
                loc, i64Ty, rewriter.getI64IntegerAttr(0));
            flag = rewriter.create<mlir::LLVM::ICmpOp>(
                loc, i1Ty, mlir::LLVM::ICmpPredicate::eq, operands[0], minC);
            res = rewriter.create<mlir::LLVM::SubOp>(loc, i64Ty, zero,
                                                     operands[0]);
            break;
        }
        case Kind::Other:
            break; // 入口已排除，不可达
        }

        // 条件分支：溢出 → panic 块；否则 → contBlock
        rewriter.create<mlir::cf::CondBranchOp>(loc, flag, panicBlock,
                                                contBlock);
        rewriter.replaceOp(op, res);
        return mlir::success();
    }

private:
    enum class Kind { Add, Sub, Mul, Neg, Other };

    /// 溢出消息全局：模块级补插 NUL 结尾常量（未用即缺），并返回函数入口
    /// 块内的 AddressOf 寄存器（按 func × 消息缓存，与 fn/file 同模型）。
    mlir::Value ensureMsgGlobal(mlir::Operation* op,
                                mlir::ConversionPatternRewriter& rewriter,
                                Kind kind) const {
        const unsigned idx = static_cast<unsigned>(kind);
        auto loc = op->getLoc();
        auto fn = op->getParentOfType<mlir::func::FuncOp>();
        auto it = msgCache_.find(fn.getOperation());
        if (it == msgCache_.end())
            it = msgCache_.try_emplace(fn.getOperation()).first;
        mlir::MLIRContext* ctx = op->getContext();
        if (!it->second[idx]) {
            mlir::ModuleOp mod = fn->getParentOfType<mlir::ModuleOp>();
            mlir::Type ptrTy = mlir::LLVM::LLVMPointerType::get(ctx);
            {
                mlir::OpBuilder::InsertionGuard guard(rewriter);
                rewriter.setInsertionPointToStart(mod.getBody());
                if (!mlir::SymbolTable::lookupSymbolIn(mod, msgSym(kind)))
                    createStrGlobal(rewriter, loc, msgSym(kind), msgText(kind));
            }
            rewriter.setInsertionPointToStart(&fn.getBody().front());
            it->second[idx] = rewriter.create<mlir::LLVM::AddressOfOp>(
                loc, ptrTy, mlir::FlatSymbolRefAttr::get(ctx, msgSym(kind)));
        }
        return it->second[idx];
    }

    static Kind kindOf(llvm::StringRef name) {
        if (name == "lang.addi") return Kind::Add;
        if (name == "lang.subi") return Kind::Sub;
        if (name == "lang.muli") return Kind::Mul;
        if (name == "lang.negi") return Kind::Neg;
        return Kind::Other;
    }

    /// 溢出消息全局符号（模块级确定性命名，两函数共享同一常量）。
    static std::string msgSym(Kind k) {
        switch (k) {
        case Kind::Add: return ".sg_msg_addov";
        case Kind::Sub: return ".sg_msg_subov";
        case Kind::Mul: return ".sg_msg_mulov";
        case Kind::Neg: return ".sg_msg_negov";
        default: return "";
        }
    }

    /// 溢出消息文本（与 VM doBinOp / doNeg / 运行时 sg_*_i64 逐字一致）。
    static std::string msgText(Kind k) {
        switch (k) {
        case Kind::Add: return "整数加法溢出";
        case Kind::Sub: return "整数减法溢出";
        case Kind::Mul: return "整数乘法溢出";
        case Kind::Neg: return "整数取负溢出";
        default: return "";
        }
    }

    mutable PanicCtxCache ctxCache_;
    /// func → 四条溢出消息的入口 AddressOf 寄存器（[Add,Sub,Mul,Neg]）。
    mutable llvm::DenseMap<mlir::Operation*, std::array<mlir::Value, 4>>
        msgCache_;
};

/// 门禁 what 字符串（"read_text" 等）：模块级全局去重 + 调用点 AddressOf。
/// dom 不下 IR——C 侧 sg_cap_check 按 bit 查 Builder 生成的域表（单一事实源
/// 经 kAllCaps 生成）。AddressOf 就地创建（支配调用点），LLVM 优化去重。
mlir::Value capWhatAddr(mlir::Operation* op,
                        mlir::ConversionPatternRewriter& rewriter,
                        const std::string& what) {
    mlir::ModuleOp mod =
        op->getParentOfType<mlir::func::FuncOp>()
            ->getParentOfType<mlir::ModuleOp>();
    mlir::Location loc = op->getLoc();
    mlir::MLIRContext* ctx = op->getContext();
    std::string sym = ".sg_capw_" + what;
    {
        mlir::OpBuilder::InsertionGuard guard(rewriter);
        rewriter.setInsertionPointToStart(mod.getBody());
        if (!mlir::SymbolTable::lookupSymbolIn(mod, sym))
            createStrGlobal(rewriter, loc, sym, what);
    }
    mlir::Type ptrTy = mlir::LLVM::LLVMPointerType::get(ctx);
    return rewriter.create<mlir::LLVM::AddressOfOp>(
        loc, ptrTy, mlir::FlatSymbolRefAttr::get(ctx, sym));
}

// ============================================================================
// 降级 pattern：每个 lang op → func.call @sg_*
// ============================================================================

class LangCallRewrite : public mlir::ConversionPattern {
public:
    explicit LangCallRewrite(mlir::MLIRContext* ctx, bool gpuMode)
        : mlir::ConversionPattern(mlir::Pattern::MatchAnyOpTypeTag{}, 1, ctx),
          gpuMode_(gpuMode) {}

    mlir::LogicalResult
    matchAndRewrite(mlir::Operation* op, mlir::ArrayRef<mlir::Value> operands,
                    mlir::ConversionPatternRewriter& rewriter) const override {
        const OpLower* l = findLowering(op->getName().getStringRef());
        if (!l) {
            op->emitError("未注册降级规则的 lang op");
            return mlir::failure();
        }
        // CUDA 第 1 层：GPU 覆盖集 op 换发 @sg_gpu_*（ABI 与 @sg_* 一致；
        // 运行时内部做就绪判定，不可用即回退 CPU 实现）。符号派生：
        // "sg_<rest>" → "sg_gpu_<rest>"。
        llvm::StringRef sgSym(l->sg);
        std::string gpuCallee;
        if (gpuMode_ && gpu::covers(op)) {
            gpuCallee = "sg_gpu_";
            gpuCallee += l->sg + 3;
            sgSym = gpuCallee;
        }
        mlir::Location loc = op->getLoc();

        // panic 上下文（fn/file/line）恢复（与内联 pattern 共享基建）。
        // 能力行也需上下文——sg_cap_check 拒绝时 panic 输出与 VM panicHere
        // 逐字一致（hasCtx=false 的能力行由 MLIRGen 挂 opLoc 保障）。
        mlir::Value fnVal, fileVal, lineVal;
        if ((l->cap || l->hasCtx) &&
            failed(ensurePanicCtx(op, rewriter, ctxCache_, fnVal, fileVal,
                                  lineVal)))
            return mlir::failure();

        // 能力运行时门禁（0.9.7 第二道）：@sg_caps 位掩码查位，未授权即
        // panic（消息由 C 侧拼装，与 VM capPanicMsg 逐字一致）。检查期
        // E5004 是第一道；此处拦截绕过检查器的执行路径（纵深防御）。
        if (l->cap) {
            Cap cap;
            if (!capFromName(l->cap, cap)) {
                op->emitError("降级表能力域名非法：" + std::string(l->cap));
                return mlir::failure();
            }
            rewriter.create<mlir::func::CallOp>(
                loc, "sg_cap_check", mlir::TypeRange{},
                mlir::ValueRange{
                    rewriter.create<mlir::arith::ConstantOp>(
                        loc, rewriter.getI64Type(),
                        rewriter.getI64IntegerAttr(
                            static_cast<int64_t>(capBit(cap)))),
                    capWhatAddr(op, rewriter, l->lang + 5), fnVal, fileVal,
                    lineVal});
        }

        mlir::SmallVector<mlir::Value> args;
        if (std::strcmp(l->sg, "sg_panic") == 0) {
            // sg_panic ABI：上下文位于 msg 之前
            args = {fnVal, fileVal, lineVal, operands[0]};
        } else {
            // bool 形参（externArgs 码 'b'）zext 到 i32 对齐 C ABI（见
            // argTypeForCode：MS x64 i1 传参高位不保证清零）。operands 与
            // externArgs 前段逐位对应；ctx 三元组（p/p/i）无 'b' 码。
            for (size_t k = 0; k < operands.size(); ++k) {
                if (l->externArgs[k] == 'b')
                    args.push_back(rewriter.create<mlir::arith::ExtUIOp>(
                        loc, rewriter.getI32Type(), operands[k]));
                else
                    args.push_back(operands[k]);
            }
            if (l->hasCtx) {
                args.push_back(fnVal);
                args.push_back(fileVal);
                args.push_back(lineVal);
            }
        }

        auto call = rewriter.create<mlir::func::CallOp>(
            loc, sgSym, op->getResultTypes(), args);
        rewriter.replaceOp(op, call.getResults());
        return mlir::success();
    }

private:
    const bool gpuMode_; // CUDA 第 1 层：GPU 覆盖集换发 @sg_gpu_*
    mutable PanicCtxCache ctxCache_;
};

// ============================================================================
// 降级 pattern：lang.extern_call → sg_extern_resolve + LLVM 间接调用
// ----------------------------------------------------------------------------
// @extern 动态库调用（0.9.4 H 专项）。MS x64 ABI 的 shadow space/寄存器
// 分配/对齐交给 LLVM 后端；解析失败/符号缺失 panic 语义在 sg_extern_resolve
// （消息与 VMExtern.cpp 逐字一致，上下文 fn/file/line 由 Location 恢复）；
// str 返回经 sg_extern_wrap_str 深拷贝（外部串生命周期不归运行时管理）。
// 附加约定（与 VM thunk 一致）：int 形参传 i64（32 位形参读低 32 位兼容）；
// float 仅 double 形参；bool 传 i1；32 位 int 返回读全 RAX 安全（x86-64
// 写 EAX 自动零扩展）。
// ============================================================================
class LangExternCallRewrite : public mlir::ConversionPattern {
public:
    explicit LangExternCallRewrite(mlir::MLIRContext* ctx)
        : mlir::ConversionPattern(mlir::Pattern::MatchAnyOpTypeTag{}, 5, ctx) {}

    mlir::LogicalResult
    matchAndRewrite(mlir::Operation* op, mlir::ArrayRef<mlir::Value> operands,
                    mlir::ConversionPatternRewriter& rewriter) const override {
        if (op->getName().getStringRef() != "lang.extern_call")
            return mlir::failure(); // 让位其他 pattern
        if (operands.size() < 2) {
            op->emitError("lang.extern_call 缺少 dll/sym 操作数");
            return mlir::failure();
        }
        mlir::Location loc = op->getLoc();
        mlir::MLIRContext* ctx = op->getContext();
        mlir::Type ptrTy = mlir::LLVM::LLVMPointerType::get(ctx);

        // 解析（带 panic 上下文）：sg_extern_resolve(dll, sym, fn, file, line)
        mlir::Value fnVal, fileVal, lineVal;
        if (failed(ensurePanicCtx(op, rewriter, ctxCache_, fnVal, fileVal,
                                  lineVal)))
            return mlir::failure();
        auto resolve = rewriter.create<mlir::func::CallOp>(
            loc, "sg_extern_resolve", mlir::TypeRange{ptrTy},
            mlir::ValueRange{operands[0], operands[1], fnVal, fileVal,
                             lineVal});

        // fflush 缓冲统一（0.9.9，F-4）：间接调用前冲刷宿主 stdout——外部
        // CRT 的 stdout 缓冲与宿主独立，先落盘我方既有输出，交错序确定。
        rewriter.create<mlir::func::CallOp>(loc, "sg_fflush_stdout",
                                            mlir::TypeRange{},
                                            mlir::ValueRange{});
        // 间接调用：函数指针为首操作数。CallOp 带 AttrSizedOperandSegments
        // （callee_operands / op_bundle_operands 两段），须显式给段大小——
        // 泛型 builder 不会自动分段。
        mlir::SmallVector<mlir::Value> callArgs(operands.begin() + 2,
                                                operands.end());
        callArgs.insert(callArgs.begin(), resolve.getResult(0));
        mlir::SmallVector<mlir::Type> retTys(op->getResultTypes());
        auto call = rewriter.create<mlir::LLVM::CallOp>(
            loc, mlir::TypeRange(retTys), callArgs,
            mlir::ArrayRef<mlir::NamedAttribute>{});
        // AttrSizedOperandSegments：泛型 build 不分段，补 callee_operands
        // 段大小（op_bundle_operands 恒 0）；验证在转换完成后进行。
        call->setAttr("operandSegmentSizes",
                      rewriter.getDenseI32ArrayAttr(
                          {static_cast<int32_t>(callArgs.size()), 0}));
        call->setAttr("op_bundle_sizes", rewriter.getDenseI32ArrayAttr({}));
        // 变参（0.9.6 O 节）：lang 层 {variadic} 标记 → 间接 call 挂
        // var_callee_type，LLVM 后端按 MS x64 varargs 发射（浮点位模式进
        // GP 位置槽 + xmm 位置槽 + AL），与 VM thunk 位置式装载双模一致；
        // 定参调用不带标记，LLVM 定参 ABI（浮点仅 xmm 序数槽）。
        if (op->getAttr("variadic")) {
            mlir::SmallVector<mlir::Type> argTys;
            for (size_t k = 2; k < operands.size(); ++k)
                argTys.push_back(operands[k].getType());
            mlir::Type retTy = retTys.empty()
                                   ? mlir::Type(mlir::LLVM::LLVMVoidType::get(
                                         op->getContext()))
                                   : retTys[0];
            call->setAttr("var_callee_type",
                         mlir::TypeAttr::get(mlir::LLVM::LLVMFunctionType::get(
                             retTy, argTys, /*isVarArg=*/true)));
        }

        // str 返回（ptr 结果）：经 sg_extern_wrap_str 深拷贝。
        if (!retTys.empty() &&
            mlir::isa<mlir::LLVM::LLVMPointerType>(retTys[0])) {
            auto wrapped = rewriter.create<mlir::func::CallOp>(
                loc, "sg_extern_wrap_str", mlir::TypeRange{ptrTy},
                mlir::ValueRange{call.getResult()});
            rewriter.replaceOp(op, wrapped.getResults());
            return mlir::success();
        }
        rewriter.replaceOp(op, call.getResults());
        return mlir::success();
    }

private:
    mutable PanicCtxCache ctxCache_;
};

// ============================================================================
// 降级 pattern：lang.thread_spawn → sg_thread_spawn（真并发 M1 特写）
// ----------------------------------------------------------------------------
// sg_target 属性指向当前模块目标函数（@sg_<module>__<fn>）。函数地址获取
// 不用 LLVM::AddressOfOp——彼时被引函数仍是 func::FuncOp（func 方言在本
// pass 合法），AddressOf 的符号域不保证跨方言；改用 func.constant（func
// 方言原生引用，零校验风险）+ unrealized bridge 对齐 !llvm.ptr ABI：
// 后续 one-shot ConvertToLLVM 的 FuncToLLVM 把 func.constant 降为
// llvm.mlir.addressof（彼时被引函数已是 llvm.func），bridge 随类型一致
// 自行消解（管线 2h 阶段另有死 cast 清扫兜底）。
// ============================================================================
class LangThreadSpawnRewrite : public mlir::ConversionPattern {
public:
    explicit LangThreadSpawnRewrite(mlir::MLIRContext* ctx)
        : mlir::ConversionPattern(mlir::Pattern::MatchAnyOpTypeTag{}, 5,
                                  ctx) {}

    mlir::LogicalResult
    matchAndRewrite(mlir::Operation* op, mlir::ArrayRef<mlir::Value> operands,
                    mlir::ConversionPatternRewriter& rewriter) const override {
        if (op->getName().getStringRef() != "lang.thread_spawn")
            return mlir::failure(); // 非目标 op：让位通用查表 pattern
        auto ts =
            op->getAttrOfType<mlir::FlatSymbolRefAttr>("sg_target");
        if (!ts) {
            op->emitError("lang.thread_spawn 缺少 sg_target 符号属性");
            return mlir::failure();
        }
        mlir::Location loc = op->getLoc();
        mlir::MLIRContext* ctx = op->getContext();
        mlir::Type ptrTy = mlir::LLVM::LLVMPointerType::get(ctx);

        // panic 上下文 + thread 域能力门禁（与 LangCallRewrite 同套路）。
        mlir::Value fnVal, fileVal, lineVal;
        if (failed(ensurePanicCtx(op, rewriter, ctxCache_, fnVal, fileVal,
                                  lineVal)))
            return mlir::failure();
        rewriter.create<mlir::func::CallOp>(
            loc, "sg_cap_check", mlir::TypeRange{},
            mlir::ValueRange{
                rewriter.create<mlir::arith::ConstantOp>(
                    loc, rewriter.getI64Type(),
                    rewriter.getI64IntegerAttr(
                        static_cast<int64_t>(capBit(Cap::Thread)))),
                capWhatAddr(op, rewriter, "thread_spawn"), fnVal, fileVal,
                lineVal});

        // 目标函数 → 函数指针（func.constant 原生引用 + unrealized bridge）。
        mlir::ModuleOp mod = op->getParentOfType<mlir::func::FuncOp>()
                                 ->getParentOfType<mlir::ModuleOp>();
        auto callee = mlir::dyn_cast_or_null<mlir::func::FuncOp>(
            mlir::SymbolTable::lookupSymbolIn(mod, ts.getAttr()));
        if (!callee) {
            op->emitError("thread_spawn 目标符号不存在：" +
                          ts.getValue().str());
            return mlir::failure();
        }
        mlir::Value fptr = rewriter.create<mlir::func::ConstantOp>(
            loc, callee.getFunctionType(), ts);
        auto bridge = rewriter.create<mlir::UnrealizedConversionCastOp>(
            loc, mlir::TypeRange{ptrTy}, mlir::ValueRange{fptr});

        auto call = rewriter.create<mlir::func::CallOp>(
            loc, "sg_thread_spawn", op->getResultTypes(),
            mlir::ValueRange{operands[0], bridge.getOutputs()[0], fnVal,
                             fileVal, lineVal});
        rewriter.replaceOp(op, call.getResults());
        return mlir::success();
    }

private:
    mutable PanicCtxCache ctxCache_;
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

    explicit LangToLLVMPass(bool gpuMode, uint64_t capMask)
        : gpuMode_(gpuMode), capMask_(capMask) {}

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
        //    一致——全量插入，未用符号无副作用）。gpuMode 下补插
        //    @sg_gpu_* 覆盖集声明（ABI 与对应 @sg_* 一致）。
        declareRuntimeExterns(mod, ctx, gpuMode_, capMask_);

        // 2) 全量转换：lang 非法；builtin/func/arith/cf/llvm/memref/linalg
        //    /affine 合法——memref/linalg/affine 是前序降级 pass（LangToAffine/
        //    LangToLinalg）的产物，留给管线后段（linalg-to-loops / lower-affine
        //    / one-shot convert-to-llvm）消费。注意：未注册的 op 不算 legal，
        //    full conversion 会对它硬失败（"failed to legalize"），必须显式
        //    列出全部 passthrough 方言。
        mlir::ConversionTarget target(*ctx);
        target.addLegalDialect<mlir::BuiltinDialect, mlir::func::FuncDialect,
                               mlir::arith::ArithDialect,
                               mlir::cf::ControlFlowDialect,
                               mlir::LLVM::LLVMDialect,
                               mlir::memref::MemRefDialect,
                               mlir::linalg::LinalgDialect,
                               mlir::affine::AffineDialect>();
        target.addIllegalDialect<::scourge::aot::lang::LangDialect>();

        mlir::RewritePatternSet patterns(ctx);
        // 标量 int add/sub/mul/neg 优先 IR 内联（benefit 10，非目标 op 返回
        // failure 自动让位）；@extern 间接调用与 thread_spawn 特写次之
        //（benefit 5，抢在查表 LangCallRewrite 之前——kLowerings 均无此二
        // op，落到它会硬失败）；其余 lang op 走 @sg_* 运行时调用。
        patterns.add<LangInlineIntRewrite>(ctx);
        patterns.add<LangExternCallRewrite>(ctx);
        patterns.add<LangThreadSpawnRewrite>(ctx);
        patterns.add<LangCallRewrite>(ctx, gpuMode_);
        if (mlir::failed(
                mlir::applyFullConversion(mod, target, std::move(patterns))))
            signalPassFailure();
    }

private:
    const bool gpuMode_; // CUDA 第 1 层：覆盖集换发 @sg_gpu_* 并补插声明
    const uint64_t capMask_; // 能力授予掩码（@sg_caps 注入值）

    static void declareRuntimeExterns(mlir::ModuleOp mod,
                                      mlir::MLIRContext* ctx, bool gpuMode,
                                      uint64_t capMask) {
        mlir::OpBuilder b(ctx);
        mlir::Location loc = mod.getLoc();
        mlir::OpBuilder::InsertionGuard guard(b);
        b.setInsertionPointToStart(mod.getBody());
        for (const OpLower& l : langLowerings()) {
            mlir::SmallVector<mlir::Type> argTys;
            for (const char* c = l.externArgs; *c; ++c)
                argTys.push_back(argTypeForCode(ctx, *c));
            mlir::SmallVector<mlir::Type> retTys;
            if (l.ret != 'v') retTys.push_back(typeForCode(ctx, l.ret));
            auto fn = b.create<mlir::func::FuncOp>(
                loc, l.sg, mlir::FunctionType::get(ctx, argTys, retTys));
            fn.setVisibility(mlir::SymbolTable::Visibility::Private);
        }
        // @extern 支撑运行时（H 专项）：解析器 ABI (dll, sym, fn, file, line)
        // ——panic 上下文由 LangExternCallRewrite 手动 ensurePanicCtx 后显式
        // 传入（hasCtx=false），失败消息与 VMExtern.cpp 逐字一致；wrap_str
        // 深拷贝外部返回串。
        static constexpr OpLower kExternSupport[] = {
            {"lang.extern_resolve", "sg_extern_resolve", "pp", "ppppi", 'p',
             false},
            {"lang.extern_wrap_str", "sg_extern_wrap_str", "p", "p", 'p',
             false},
            // 能力运行时门禁（0.9.7）：ABI (bit, what, fn, file, line) =
            // "ipppi"。bit/what 由 LangCallRewrite 前置生成；sg_extern_resolve
            // 亦调用（ffi 域入口查位）。
            {"lang.cap_check", "sg_cap_check", "", "ipppi", 'v', false},
            // fflush 缓冲统一（0.9.9，F-4）：外部 CRT（msvcrt.dll 等）与宿主
            // UCRT 的 stdout 缓冲独立，重定向下全缓冲时交错序不确定——
            // 每个 @extern 调用点前置冲刷宿主缓冲。VMExtern.cpp doExtern
            // 同点同语义，双模一致。
            {"lang.fflush", "sg_fflush_stdout", "", "", 'v', false},
            // 真并发 M1：thread_spawn 特写（LangThreadSpawnRewrite）——ABI
            // (name, fnptr, fn, file, line) = "ppppi"，fnptr 由 func.constant
            // 取自 sg_target 指向的目标函数（bridge 后即 !llvm.ptr）。其余
            // 8 内建在 kLowerings。
            {"lang.thread_spawn", "sg_thread_spawn", "p", "ppppi", 'i',
             false},
        };
        for (const OpLower& l : kExternSupport) {
            mlir::SmallVector<mlir::Type> argTys;
            for (const char* c = l.externArgs; *c; ++c)
                argTys.push_back(argTypeForCode(ctx, *c));
            mlir::SmallVector<mlir::Type> retTys;
            if (l.ret != 'v') retTys.push_back(typeForCode(ctx, l.ret));
            auto fn = b.create<mlir::func::FuncOp>(
                loc, l.sg, mlir::FunctionType::get(ctx, argTys, retTys));
            fn.setVisibility(mlir::SymbolTable::Visibility::Private);
        }
        // @sg_caps：程序能力授予掩码（会话层对依赖闭包求并，builder 传入）
        // ——external constant 全局进 main.obj，rt.c 侧 extern 读取（rt.obj
        // 保持程序无关，缓存键不失效）。external 链接必需：internal 符号
        // 无法跨 TU 被 rt.obj 引用。
        b.create<mlir::LLVM::GlobalOp>(
            loc, b.getI64Type(), /*isConstant=*/true,
            mlir::LLVM::Linkage::External, "sg_caps",
            b.getI64IntegerAttr(static_cast<int64_t>(capMask)));
        if (!gpuMode) return;
        // GPU 覆盖集声明（sg_<rest> → sg_gpu_<rest>，ABI 逐一相同）。名单
        // 单一事实源在 GpuCover.hpp（kGpuCoveredLang / coveredLangName），
        // 与 covers 判定同源——此处查表，禁止平行维护名单。
        for (const OpLower& l : langLowerings()) {
            if (!gpu::coveredLangName(l.lang))
                continue;
            std::string sym = "sg_gpu_";
            sym += l.sg + 3;
            auto fn = b.create<mlir::func::FuncOp>(
                loc, sym, typeForLowering(ctx, l));
            fn.setVisibility(mlir::SymbolTable::Visibility::Private);
        }
    }

    /// 按 OpLower 构造函数类型（与上方主循环的展开等价的辅助）。
    static mlir::FunctionType typeForLowering(mlir::MLIRContext* ctx,
                                              const OpLower& l) {
        mlir::SmallVector<mlir::Type> argTys;
        for (const char* c = l.externArgs; *c; ++c)
            argTys.push_back(argTypeForCode(ctx, *c));
        mlir::SmallVector<mlir::Type> retTys;
        if (l.ret != 'v') retTys.push_back(typeForCode(ctx, l.ret));
        return mlir::FunctionType::get(ctx, argTys, retTys);
    }
};

} // namespace

std::unique_ptr<mlir::Pass> createLangToLLVMPass(bool gpuMode,
                                                 uint64_t capMask) {
    return std::make_unique<LangToLLVMPass>(gpuMode, capMask);
}

} // namespace aot
} // namespace scourge
