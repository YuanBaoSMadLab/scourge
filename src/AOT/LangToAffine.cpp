// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT Lang 方言 → Affine 降级 pass（lang.* → affine.*）
// ----------------------------------------------------------------------------
//  AOT 步骤 3b：覆盖集子集（f64 全静态形状的逐元素 tt/ts/st kind 0-2、
//  标量双向广播、sum_f、2D transpose）降级为 affine 结构化循环：
//  affine.for（常量上下界，buildAffineLoopNest）+ affine.load/affine.store。
//  affine op 由管线后段的 createLowerAffinePass 统一降为 scf/memref/arith，
//  再经既有 scf-to-cf 与 one-shot ConvertToLLVM 收尾——本 pass 不触碰
//  LLVM dialect（descriptor 桥除外，见 TensorBridge.hpp）。
//
//  输入/输出桥与 LangToLinalg 完全共用（TensorBridge.hpp 单一事实源）：
//  输入零拷贝 descriptor 视图，输出 1D alloc + reinterpret_cast + adopt。
//
//  sum 累加器用 0-d memref.alloc（堆，arena 纪律）：sum 常驻用户热循环，
//  memref.alloca 会逐迭代栈增长；8 字节/次的堆增长与张量 op 的 arena
//  分配同一纪律。
// ============================================================================
#include "scourge/AOT/LangToAffine.hpp"

#include <cassert>

#include "mlir/Dialect/Affine/IR/AffineOps.h" // 上游此快照无独立 Affine.h；方言声明经 AffineOpsDialect.h.inc 随此头提供
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "scourge/AOT/LangOps.h"

#include "TensorBridge.hpp"

namespace scourge {
namespace aot {

namespace {

using namespace mlir;
using namespace bridge;

/// f64 双目算术：kind 0/1/2 → addf/subf/mulf（与 LangToLinalg 一致）。
inline Value fbin(int64_t kind, OpBuilder& b, Location l, Value x, Value y) {
    if (kind == 0) return b.create<arith::AddFOp>(l, x, y);
    if (kind == 1) return b.create<arith::SubFOp>(l, x, y);
    return b.create<arith::MulFOp>(l, x, y);
}

// ============================================================================
//  降级 pattern：带形状属性的覆盖集 lang op → affine 循环
// ============================================================================

class LangAffineRewrite : public ConversionPattern {
public:
    explicit LangAffineRewrite(MLIRContext* ctx)
        : ConversionPattern(Pattern::MatchAnyOpTypeTag{}, /*benefit=*/3, ctx) {}

    LogicalResult
    matchAndRewrite(Operation* op, ArrayRef<Value> operands,
                    ConversionPatternRewriter& rewriter) const override {
        // 本 pass 只消费带形状属性的覆盖集 op（ConversionTarget 动态
        // illegal 保证进入此处即有属性）
        Location loc = op->getLoc();
        ModuleOp mod = op->getParentOfType<ModuleOp>();
        MLIRContext* ctx = rewriter.getContext();
        Type f64 = rewriter.getF64Type();

        SmallVector<int64_t> shape;
        if (auto a = op->getAttrOfType<DenseI64ArrayAttr>("shape"))
            llvm::append_range(shape, a.asArrayRef());
        int64_t kind = op->getAttrOfType<IntegerAttr>("kind")
                           ? op->getAttrOfType<IntegerAttr>("kind").getInt()
                           : -1;

        llvm::StringRef name = op->getName().getStringRef();
        if (name == "lang.tensor_binop_tt" || name == "lang.tensor_binop_ts" ||
            name == "lang.tensor_binop_st") {
            return lowerBinop(op, operands, rewriter, loc, mod, ctx, f64,
                              shape, kind, name);
        }
        if (name == "lang.tensor_sum_f")
            return lowerSum(op, operands, rewriter, loc, mod, ctx, f64, shape);
        if (name == "lang.tensor_transpose")
            return lowerTranspose(op, operands, rewriter, loc, mod, ctx, f64,
                                  shape);
        op->emitError("LangToAffine：意外的 lang op（动态合法性失配）");
        return failure();
    }

private:
    /// 逐元素 binop（tt/ts/st 共用）：常量界仿射循环嵌套，行主序遍历。
    /// 累加顺序与 sg_* 运行时一致（同为行主单遍），浮点逐位等价。
    LogicalResult
    lowerBinop(Operation* op, ArrayRef<Value> operands,
               ConversionPatternRewriter& rewriter, Location loc, ModuleOp mod,
               MLIRContext* ctx, Type f64, ArrayRef<int64_t> shape,
               int64_t kind, llvm::StringRef name) const {
        if (kind < 0 || kind > 2 || shape.empty()) {
            op->emitError("LangToAffine：binop 缺少 kind/shape 属性或 kind "
                          "超出覆盖集");
            return failure();
        }
        // 操作数（LangOps.td）：
        //   tt: (kind, lhs, rhs)      ts: (kind, lhs, isInt, si, sf)
        //   st: (kind, isInt, si, sf, rhs)
        // 覆盖形态由 MLIRGen 保证标量为 float（isInt=0）→ 标量取 sf 槽位
        //（si 槽位在 float 标量下恒为 0 哑元，不可用）。
        const bool isTT = name == "lang.tensor_binop_tt";
        const bool isST = name == "lang.tensor_binop_st";
        Value tensorA = isTT ? operands[1] : (isST ? operands[4] : operands[1]);
        Value scalar = isTT ? Value() : operands[isST ? 3 : 4];

        Value a = viewInput(rewriter, loc, tensorA, shape, f64);
        Value b = isTT ? viewInput(rewriter, loc, operands[2], shape, f64)
                       : Value();
        OutputBuf out = allocOutput(rewriter, loc, mod, shape, f64);

        SmallVector<int64_t> lbs(shape.size(), 0), steps(shape.size(), 1);
        affine::buildAffineLoopNest(
            rewriter, loc, lbs, SmallVector<int64_t>(shape), steps,
            [&](OpBuilder& nb, Location l, ValueRange ivs) {
                Value x, y;
                if (isTT) {
                    x = nb.create<affine::AffineLoadOp>(l, a, ivs);
                    y = nb.create<affine::AffineLoadOp>(l, b, ivs);
                } else if (isST) { // 标量在左：s op t
                    x = scalar;
                    y = nb.create<affine::AffineLoadOp>(l, a, ivs);
                } else { // ts：t op 标量
                    x = nb.create<affine::AffineLoadOp>(l, a, ivs);
                    y = scalar;
                }
                Value r = fbin(kind, nb, l, x, y);
                nb.create<affine::AffineStoreOp>(l, r, out.view, ivs);
            });

        adoptOutput(rewriter, loc, out.handle, out.flat);
        rewriter.replaceOp(op, out.handle);
        return success();
    }

    /// sum 归约：0-d memref.alloc 累加器 + 行主单遍仿射嵌套，加数顺序
    ///（张量元素 + 累加值）与 sg_tensor_sum_f / linalg 路径一致，浮点
    /// 逐位等价。
    LogicalResult
    lowerSum(Operation* op, ArrayRef<Value> operands,
             ConversionPatternRewriter& rewriter, Location loc, ModuleOp mod,
             MLIRContext* ctx, Type f64, ArrayRef<int64_t> shape) const {
        Value in = viewInput(rewriter, loc, operands[0], shape, f64);
        Value acc = rewriter.create<memref::AllocOp>(
            loc, MemRefType::get({}, f64));
        Value zero = rewriter.create<arith::ConstantOp>(
            loc, f64, rewriter.getF64FloatAttr(0.0));
        rewriter.create<memref::StoreOp>(loc, zero, acc, ValueRange{});

        SmallVector<int64_t> lbs(shape.size(), 0), steps(shape.size(), 1);
        affine::buildAffineLoopNest(
            rewriter, loc, lbs, SmallVector<int64_t>(shape), steps,
            [&](OpBuilder& nb, Location l, ValueRange ivs) {
                Value v = nb.create<memref::LoadOp>(l, acc, ValueRange{});
                Value x = nb.create<affine::AffineLoadOp>(l, in, ivs);
                Value s = nb.create<arith::AddFOp>(l, x, v);
                nb.create<memref::StoreOp>(l, s, acc, ValueRange{});
            });
        Value res = rewriter.create<memref::LoadOp>(loc, acc, ValueRange{});
        rewriter.replaceOp(op, res);
        return success();
    }

    /// 2D 转置：out[i,j] = in[j,i]（纯数据搬移，顺序无关）。
    LogicalResult
    lowerTranspose(Operation* op, ArrayRef<Value> operands,
                   ConversionPatternRewriter& rewriter, Location loc, ModuleOp mod,
                   MLIRContext* ctx, Type f64,
                   ArrayRef<int64_t> shape) const {
        if (shape.size() != 2) {
            op->emitError("LangToAffine：transpose 形状属性必须二维");
            return failure();
        }
        Value in = viewInput(rewriter, loc, operands[0], shape, f64);
        SmallVector<int64_t> outShape = {shape[1], shape[0]};
        OutputBuf out = allocOutput(rewriter, loc, mod, outShape, f64);
        // 迭代空间 = 输出形状 {m,n}：out[i,j] = in[j,i]，i 走 out 行
        //（0..shape[1]），j 走 out 列（0..shape[0]）。界误用输入形状会让
        // 末行漏写 + 末列越界写（曾致 p_tr 第 3 行读出未初始化内存）。
        affine::buildAffineLoopNest(
            rewriter, loc, SmallVector<int64_t>{0, 0},
            SmallVector<int64_t>{shape[1], shape[0]},
            SmallVector<int64_t>{1, 1},
            [&](OpBuilder& nb, Location l, ValueRange ivs) {
                Value v = nb.create<affine::AffineLoadOp>(
                    l, in, ValueRange{ivs[1], ivs[0]});
                nb.create<affine::AffineStoreOp>(l, v, out.view, ivs);
            });
        adoptOutput(rewriter, loc, out.handle, out.flat);
        rewriter.replaceOp(op, out.handle);
        return success();
    }
};

// ============================================================================
//  pass 主体
// ============================================================================

class LangToAffinePass
    : public PassWrapper<LangToAffinePass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LangToAffinePass)

    StringRef getArgument() const override { return "scourge-lang-to-affine"; }
    StringRef getDescription() const override {
        return "Scourge Lang 张量逐元素/归约/转置 op（带静态形状属性）"
               "降级为 affine 循环";
    }

    /// 本 pass 创建 affine/memref/arith/llvm op，依赖方言须经 PassManager
    /// 预先加载（否则 OpBuilder 建零 op 时方言未注册崩溃）。
    void getDependentDialects(DialectRegistry& registry) const override {
        registry.insert<affine::AffineDialect, memref::MemRefDialect,
                        func::FuncDialect, arith::ArithDialect,
                        LLVM::LLVMDialect>();
    }

    void runOnOperation() override {
        ModuleOp mod = getOperation();
        MLIRContext* ctx = &getContext();

        declareBridgeExterns(mod);

        // partial conversion：lang 方言默认 legal（留给 LangToLinalg /
        // LangToLLVM 兜底），仅覆盖集 op 在携带完整形状属性时动态
        // illegal → 由 LangAffineRewrite 消费。
        ConversionTarget target(*ctx);
        target.addLegalDialect<BuiltinDialect, func::FuncDialect,
                               arith::ArithDialect, cf::ControlFlowDialect,
                               LLVM::LLVMDialect, memref::MemRefDialect,
                               affine::AffineDialect>();
        target.addLegalDialect<::scourge::aot::lang::LangDialect>();
        // 覆盖集 op 仅在携带完整形状属性时非法（动态合法回调：缺任一
        // 属性 → legal，由后段兜底；属性齐全 → 本 pass 消费。panic 语义
        // 零分叉）。注意：initializer_list 是临时数组视图，须在
        // missingAttrs 存活期内拷贝为值语义容器再捕获，否则内层 lambda
        // 读悬垂内存（曾致 hasAttr 误判 → 覆盖集全部漏判 legal → 静默
        // 回退 sg_*）。
        auto missingAttrs = [](std::initializer_list<llvm::StringRef> names) {
            llvm::SmallVector<llvm::StringRef, 4> ns(names);
            return [ns = std::move(ns)](Operation* op) {
                for (llvm::StringRef n : ns)
                    if (!op->hasAttr(n)) return true; // 未覆盖形态 → legal
                return false; // 属性齐全 → 非法 → 降级
            };
        };
        target.addDynamicallyLegalOp<lang::TensorBinopTTOp, lang::TensorBinopTSOp,
                                     lang::TensorBinopSTOp>(
            missingAttrs({"kind", "shape"}));
        target.addDynamicallyLegalOp<lang::TensorSumFOp>(
            missingAttrs({"shape"}));
        target.addDynamicallyLegalOp<lang::TensorTransposeOp>(
            missingAttrs({"shape"}));

        RewritePatternSet patterns(ctx);
        patterns.add<LangAffineRewrite>(ctx);
        if (failed(applyPartialConversion(mod, target, std::move(patterns))))
            signalPassFailure();
    }
};

} // namespace

std::unique_ptr<Pass> createLangToAffinePass() {
    return std::make_unique<LangToAffinePass>();
}

} // namespace aot
} // namespace scourge
