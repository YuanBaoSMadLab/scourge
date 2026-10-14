// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT Lang 方言 → Linalg 降级 pass（lang.* → linalg.*）
// ----------------------------------------------------------------------------
//  AOT 步骤 3：张量计算类 lang op 的原生 MLIR 降级（接口见 LangToLinalg.hpp）。
//
//  分工（3b 起）：LangToAffine 先行消费覆盖集中的逐元素/标量广播/sum/
//  transpose（降为 affine 循环）；本 pass 消费 matmul，并保留其余覆盖分支
//  作为回退——pass 顺序互换时语义仍正确（降级产物字节等价）。
//
//  数据桥（与 sg_* 句柄世界零拷贝衔接，3a-2 抽出为共享 TensorBridge.hpp）：
//  * 输入：func.call @sg_tensor_data(句柄) 直取数据指针 → 手构 memref
//    descriptor → unrealized_conversion_cast 包装为 memref 视图，零拷贝；
//  * 输出：memref.alloc（1D 连续）→ reinterpret_cast → linalg outs 写入 →
//    取存储指针 → func.call @sg_tensor_adopt(新句柄, 指针) 零拷贝接管。
//
//  linalg 均为 memref 语义（outs in-place），结果内存由 memref.alloc
//  malloc 且程序生命周期持有（AOT 运行时无 free，与 sg_xmalloc arena 一致）。
// ============================================================================
#include "scourge/AOT/LangToLinalg.hpp"

#include <cassert>

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h" // v23 快照无独立 Affine.h；方言声明随此头提供
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "scourge/AOT/LangOps.h"

#include "scourge/AOT/TensorBridge.hpp"
#include "scourge/AOT/GpuCover.hpp"

namespace scourge {
namespace aot {

namespace {

using namespace mlir;
using namespace bridge;

// ============================================================================
//  降级 pattern：带形状属性的覆盖集 lang op → linalg
// ============================================================================

class LangLinalgRewrite : public ConversionPattern {
public:
    explicit LangLinalgRewrite(MLIRContext* ctx)
        : ConversionPattern(Pattern::MatchAnyOpTypeTag{}, /*benefit=*/2, ctx) {}

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
        SmallVector<int64_t> lhsShape, rhsShape;
        if (auto a = op->getAttrOfType<DenseI64ArrayAttr>("lhs_shape"))
            llvm::append_range(lhsShape, a.asArrayRef());
        if (auto a = op->getAttrOfType<DenseI64ArrayAttr>("rhs_shape"))
            llvm::append_range(rhsShape, a.asArrayRef());
        int64_t kind = op->getAttrOfType<IntegerAttr>("kind")
                           ? op->getAttrOfType<IntegerAttr>("kind").getInt()
                           : -1;

        llvm::StringRef name = op->getName().getStringRef();
        if (name == "lang.tensor_matmul") {
            return lowerMatmul(op, operands, rewriter, loc, mod, ctx, f64,
                               lhsShape, rhsShape);
        }
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
        op->emitError("LangToLinalg：意外的 lang op（动态合法性失配）");
        return failure();
    }

private:
    LogicalResult
    lowerMatmul(Operation* op, ArrayRef<Value> operands,
                ConversionPatternRewriter& rewriter, Location loc, ModuleOp mod,
                MLIRContext* ctx, Type f64, ArrayRef<int64_t> lShape,
                ArrayRef<int64_t> rShape) const {
        if (lShape.size() != 2 || rShape.size() != 2) {
            op->emitError("LangToLinalg：matmul 形状属性必须二维");
            return failure();
        }
        Value a = viewInput(rewriter, loc, operands[0], lShape, f64);
        Value b = viewInput(rewriter, loc, operands[1], rShape, f64);
        SmallVector<int64_t> outShape = {lShape[0], rShape[1]};
        OutputBuf out = allocOutput(rewriter, loc, mod, outShape, f64);
        // linalg.matmul 语义为 C += A@B（outs 承载累加初值），而输出缓冲
        // 是未初始化 memref.alloc——须先 fill 0，否则首轮 k 把垃圾读进
        // 累加器（曾致 p_mm 全 NaN）。逐元素/转置路径全量覆写无此问题。
        Value zero = rewriter.create<arith::ConstantOp>(
            loc, f64, rewriter.getF64FloatAttr(0.0));
        rewriter.create<linalg::FillOp>(loc, ValueRange{zero},
                                        ValueRange{out.view});
        rewriter.create<linalg::MatmulOp>(loc, ValueRange{a, b},
                                          ValueRange{out.view});
        adoptOutput(rewriter, loc, out.handle, out.flat);
        rewriter.replaceOp(op, out.handle);
        return success();
    }

    /// generic 逐元素构造（tt/ts/st 共用）：0/1/2 → addf/subf/mulf。
    /// block args：张量输入元素在前、输出元素在后；标量操作数经 body
    /// 闭包捕获，不进 ins（linalg generic 的 ins 仅接受 shaped 值，
    /// 标量进 ins 会使 pattern 构造失败而回退 sg_*）；yield 值数 = 输出数。
    LogicalResult
    lowerBinop(Operation* op, ArrayRef<Value> operands,
               ConversionPatternRewriter& rewriter, Location loc, ModuleOp mod,
               MLIRContext* ctx, Type f64, ArrayRef<int64_t> shape,
               int64_t kind, llvm::StringRef name) const {
        if (kind < 0 || kind > 2 || shape.empty()) {
            op->emitError("LangToLinalg：binop 缺少 kind/shape 属性或 kind "
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
        Value tensorOp = isTT ? operands[1] : (isST ? operands[4] : operands[1]);
        Value scalar = isTT ? Value() : operands[isST ? 3 : 4];

        Value a = viewInput(rewriter, loc, tensorOp, shape, f64);
        OutputBuf out = allocOutput(rewriter, loc, mod, shape, f64);

        AffineMap idMap = AffineMap::getMultiDimIdentityMap(
            static_cast<unsigned>(shape.size()), ctx);
        SmallVector<AffineMap> maps;
        SmallVector<utils::IteratorType> iters(shape.size(),
                                               utils::IteratorType::parallel);
        SmallVector<Value> ins;
        if (isTT) { // 张量-张量：args = [a_elem, b_elem, out_elem]
            Value b = viewInput(rewriter, loc, operands[2], shape, f64);
            maps = {idMap, idMap, idMap};
            ins = {a, b};
        } else { // ts/st：标量在 body 闭包内捕获，args = [t_elem, out_elem]
            maps = {idMap, idMap};
            ins = {a};
        }

        auto body = [&](OpBuilder& nb, Location l, ValueRange args) {
            Value x = isTT   ? args[0]
                      : isST ? scalar
                             : args[0];
            Value y = isTT ? args[1] : (isST ? args[0] : scalar);
            Value r;
            if (kind == 0)
                r = nb.create<arith::AddFOp>(l, x, y);
            else if (kind == 1)
                r = nb.create<arith::SubFOp>(l, x, y);
            else
                r = nb.create<arith::MulFOp>(l, x, y);
            nb.create<linalg::YieldOp>(l, ValueRange{r});
        };
        rewriter.create<linalg::GenericOp>(loc, TypeRange{}, ins,
                                           ValueRange{out.view}, maps, iters,
                                           body);
        adoptOutput(rewriter, loc, out.handle, out.flat);
        rewriter.replaceOp(op, out.handle);
        return success();
    }

    LogicalResult
    lowerSum(Operation* op, ArrayRef<Value> operands,
             ConversionPatternRewriter& rewriter, Location loc, ModuleOp mod,
             MLIRContext* ctx, Type f64, ArrayRef<int64_t> shape) const {
        Value in = viewInput(rewriter, loc, operands[0], shape, f64);
        auto accTy = MemRefType::get({}, f64);
        Value acc = rewriter.create<memref::AllocaOp>(loc, accTy);
        Value zero = rewriter.create<arith::ConstantOp>(
            loc, f64, rewriter.getF64FloatAttr(0.0));
        rewriter.create<memref::StoreOp>(loc, zero, acc, ValueRange{});

        AffineMap idMap = AffineMap::getMultiDimIdentityMap(
            static_cast<unsigned>(shape.size()), ctx);
        AffineMap emptyMap = AffineMap::get(ctx);
        SmallVector<AffineMap> maps = {idMap, emptyMap};
        SmallVector<utils::IteratorType> iters(
            shape.size(), utils::IteratorType::reduction);
        auto body = [&](OpBuilder& nb, Location l, ValueRange args) {
            // args = [张量元素, 归约累加值]
            Value r = nb.create<arith::AddFOp>(l, args[0], args[1]);
            nb.create<linalg::YieldOp>(l, ValueRange{r});
        };
        rewriter.create<linalg::GenericOp>(loc, TypeRange{},
                                           ValueRange{in}, ValueRange{acc},
                                           maps, iters, body);
        Value res = rewriter.create<memref::LoadOp>(loc, f64, acc,
                                                    ValueRange{});
        rewriter.replaceOp(op, res);
        return success();
    }

    LogicalResult
    lowerTranspose(Operation* op, ArrayRef<Value> operands,
                   ConversionPatternRewriter& rewriter, Location loc, ModuleOp mod,
                   MLIRContext* ctx, Type f64,
                   ArrayRef<int64_t> shape) const {
        if (shape.size() != 2) {
            op->emitError("LangToLinalg：transpose 形状属性必须二维");
            return failure();
        }
        Value in = viewInput(rewriter, loc, operands[0], shape, f64);
        SmallVector<int64_t> outShape = {shape[1], shape[0]};
        OutputBuf out = allocOutput(rewriter, loc, mod, outShape, f64);
        rewriter.create<linalg::TransposeOp>(loc, in, out.view,
                                             ArrayRef<int64_t>{1, 0});
        adoptOutput(rewriter, loc, out.handle, out.flat);
        rewriter.replaceOp(op, out.handle);
        return success();
    }
};

// ============================================================================
//  pass 主体
// ============================================================================

class LangToLinalgPass
    : public PassWrapper<LangToLinalgPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LangToLinalgPass)

    explicit LangToLinalgPass(bool gpuMode) : gpuMode_(gpuMode) {}

    StringRef getArgument() const override { return "scourge-lang-to-linalg"; }
    StringRef getDescription() const override {
        return "Scourge Lang 张量计算 op（带静态形状属性）降级为 linalg";
    }

    /// 本 pass 创建 linalg/memref/llvm op，依赖方言须经 PassManager
    /// 预先加载（否则 OpBuilder 建零 op 时方言未注册崩溃）。
    void getDependentDialects(DialectRegistry& registry) const override {
        registry.insert<linalg::LinalgDialect, memref::MemRefDialect,
                        func::FuncDialect, LLVM::LLVMDialect>();
    }

    void runOnOperation() override {
        ModuleOp mod = getOperation();
        MLIRContext* ctx = &getContext();
        const bool gpuMode = gpuMode_;

        declareBridgeExterns(mod);

        // partial conversion：lang/builtin/func/arith/cf/llvm/memref/linalg
        // /affine 均合法（affine 是 LangToAffine 先行产物，留给后段
        // lower-affine；未注册的 op 不算 legal，会被 MatchAny pattern
        // 白白尝试并污染诊断——必须显式列出全部 passthrough 方言），
        // 仅覆盖集 op 在携带完整形状属性时动态 illegal → 由
        // LangLinalgRewrite 消费。
        ConversionTarget target(*ctx);
        target.addLegalDialect<BuiltinDialect, func::FuncDialect,
                               arith::ArithDialect, cf::ControlFlowDialect,
                               LLVM::LLVMDialect, memref::MemRefDialect,
                               linalg::LinalgDialect,
                               affine::AffineDialect>();
        target.addLegalDialect<
            ::scourge::aot::lang::LangDialect>();
        // 覆盖集 op 仅在携带完整 linalg 形状属性时非法（动态合法回调：
        // 缺任一属性 → legal，由 LangToLLVM 兜底 sg_*；属性齐全 → 本 pass
        // 消费。panic 语义零分叉）。注意：initializer_list 是临时数组
        // 视图，须在 missingAttrs 存活期内拷贝为值语义容器再捕获，否则
        // 内层 lambda 读悬垂内存（曾致 hasAttr 误判 → 覆盖集全部漏判
        // legal → 静默回退 sg_*）。
        auto missingAttrs = [gpuMode](std::initializer_list<llvm::StringRef> names) {
            llvm::SmallVector<llvm::StringRef, 4> ns(names);
            return [ns = std::move(ns), gpuMode](Operation* op) {
                if (gpuMode && gpu::covers(op)) return true; // GPU 接管 → 跳过
                for (llvm::StringRef n : ns)
                    if (!op->hasAttr(n)) return true; // 未覆盖形态 → legal
                return false; // 属性齐全 → 非法 → 降级
            };
        };
        // 3b 覆盖面扩展（int/混合 dtype、kind 3-6 除法族）由 LangToAffine
        // 先行消费；本 pass 只保留 f64 kind 0-2/sum_f/f64 transpose 形态
        // 作为回退——带 int dtype 属性（lhs_int/rhs_int/t_int/s_int 非零）
        // 的 op 视为 legal 跳过（固定管线顺序下不可达，纯防御）。
        auto f64Only = [](Operation* op) {
            for (const char* n : {"lhs_int", "rhs_int", "t_int", "s_int"}) {
                auto a = op->getAttrOfType<IntegerAttr>(n);
                if (a && a.getInt() != 0) return true; // 非 f64 形态 → legal
            }
            return false;
        };
        auto linalgMissing = [gpuMode, f64Only](std::initializer_list<llvm::StringRef> names) {
            llvm::SmallVector<llvm::StringRef, 4> ns(names);
            return [ns = std::move(ns), f64Only, gpuMode](Operation* op) {
                if (gpuMode && gpu::covers(op)) return true; // GPU 接管 → 跳过
                for (llvm::StringRef n : ns)
                    if (!op->hasAttr(n)) return true; // 未覆盖形态 → legal
                return f64Only(op); // int 形态 → legal
            };
        };
        // N1 根治：CPU 模式 matmul 不再降级 linalg.matmul——linalg-to-loops
        // 展开的 i-j-p 外积循环里 a/b/c 均为不透明 SgTensor 指针，clang -O2
        // 因指针别名放弃向量化（store-to-load 依赖链），大尺寸 AOT 实测慢于
        // VM（256² 1.60× / 512² 1.35× / 1024² 1.38×）。保持 legal → 由
        // LangToLLVM 兜底 @sg_tensor_matmul（rt 侧 i-k-j 行流式 + restrict，
        // 对每个 c[i][j] 的累加序仍按 p 递增，与 VM 逐位一致）。GPU 模式
        // 判定不变（covers → legal，兜底 @sg_gpu_tensor_matmul）。
        // lowerMatmul 分支保留作防御性回退（当前管线不可达）。
        if (gpuMode)
            target.addDynamicallyLegalOp<lang::TensorMatmulOp>(
                missingAttrs({"lhs_shape", "rhs_shape"}));
        target.addDynamicallyLegalOp<lang::TensorBinopTTOp>(
            linalgMissing({"kind", "shape"}));
        target.addDynamicallyLegalOp<lang::TensorBinopTSOp>(
            linalgMissing({"kind", "shape"}));
        target.addDynamicallyLegalOp<lang::TensorBinopSTOp>(
            linalgMissing({"kind", "shape"}));
        target.addDynamicallyLegalOp<lang::TensorSumFOp>(
            missingAttrs({"shape"}));
        target.addDynamicallyLegalOp<lang::TensorTransposeOp>(
            linalgMissing({"shape"}));

        RewritePatternSet patterns(ctx);
        patterns.add<LangLinalgRewrite>(ctx);
        if (failed(applyPartialConversion(mod, target, std::move(patterns))))
            signalPassFailure();
    }

private:
    const bool gpuMode_; // CUDA 第 1 层：GPU 覆盖集跳过 linalg 降级
};

} // namespace

std::unique_ptr<Pass> createLangToLinalgPass(bool gpuMode) {
    return std::make_unique<LangToLinalgPass>(gpuMode);
}

} // namespace aot
} // namespace scourge
