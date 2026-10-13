// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT Lang 方言 → Linalg 降级 pass（lang.* → linalg.*）
// ----------------------------------------------------------------------------
//  AOT 步骤 3：张量计算类 lang op 的原生 MLIR 降级（接口见 LangToLinalg.hpp）。
//
//  数据桥（与 sg_* 句柄世界零拷贝衔接，3a-2）：
//  * 输入：func.call @sg_tensor_data(句柄) 直取数据指针 → 手构 memref
//    descriptor（allocated=aligned=数据指针，offset=0，静态行主
//    sizes/strides，布局与 LLVMTypeConverter::getMemRefDescriptorFields
//    unpackAggregates=false 逐字段一致）→ unrealized_conversion_cast
//    包装为 memref 视图，零拷贝。arena 内存纪律下无 free，句柄值 SSA
//    活性覆盖消费全程，只读别名安全；下游 one-shot ConvertToLLVM 对该
//    cast 穿透直取 descriptor 本体（框架 materialization 同型机制）。
//  * 输出：memref.alloc（1D 连续）→ reinterpret_cast → linalg outs 写入 →
//    memref.extract_aligned_pointer_as_index 取存储指针 →
//    func.call @sg_tensor_adopt(新句柄, 指针) 零拷贝接管 → 新句柄
//    （@sg_tensor_alloc arena 分配，与 sg_* 内存纪律一致）返回句柄域。
//
//  形状常量数组（sg_tensor_alloc 参数）以 llvm.mlir.global internal
//  constant 数组 + llvm.addressof 提供（按形状去重，与 panic 上下文的
//  字符串全局同一机制）。
//
//  linalg 均为 memref 语义（outs in-place），结果内存由 memref.alloc
//  malloc 且程序生命周期持有（AOT 运行时无 free，与 sg_xmalloc arena 一致）。
// ============================================================================
#include "scourge/AOT/LangToLinalg.hpp"

#include <cassert>

#include "mlir/Dialect/Arith/IR/Arith.h"
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

namespace scourge {
namespace aot {

namespace {

using namespace mlir;

// ============================================================================
//  运行时桥符号（本 pass 补插声明；定义在 src/Tool/Builder.cpp）
// ============================================================================
struct RtSyms {
    static constexpr const char* kData = "sg_tensor_data";  // (ptr) -> ptr
    static constexpr const char* kAlloc =
        "sg_tensor_alloc"; // (i32, i64, ptr) -> ptr
    static constexpr const char* kAdopt = "sg_tensor_adopt"; // (ptr, ptr) -> ()
};

Type ptrTy(MLIRContext* ctx) { return LLVM::LLVMPointerType::get(ctx); }

/// 模块级补插桥函数外部声明（private；重复运行幂等）。
void declareBridgeExterns(ModuleOp mod) {
    MLIRContext* ctx = mod.getContext();
    OpBuilder b(ctx);
    Location loc = mod.getLoc();
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(mod.getBody());
    auto declare = [&](const char* name, ArrayRef<Type> args,
                       ArrayRef<Type> rets) {
        if (SymbolTable::lookupSymbolIn(mod, name)) return;
        auto fn = b.create<func::FuncOp>(loc, name,
                                         FunctionType::get(ctx, args, rets));
        fn.setVisibility(SymbolTable::Visibility::Private);
    };
    Type p = ptrTy(ctx);
    Type i32 = b.getI32Type();
    Type i64 = b.getI64Type();
    declare(RtSyms::kData, {p}, {p});
    declare(RtSyms::kAlloc, {i32, i64, p}, {p});
    declare(RtSyms::kAdopt, {p, p}, {});
}

// ============================================================================
//  形状常量数组全局：llvm.mlir.global internal constant [rank x i64] +
//  llvm.addressof（按形状去重）。
// ============================================================================
Value shapeAddr(ConversionPatternRewriter& rewriter, Location loc,
                ModuleOp mod, ArrayRef<int64_t> shape) {
    MLIRContext* ctx = rewriter.getContext();
    std::string sym = ".sg_tshape_";
    for (int64_t d : shape) sym += std::to_string(d) + "x";
    if (!sym.empty() && sym.back() == 'x') sym.pop_back();

    Operation* found = SymbolTable::lookupSymbolIn(mod, sym);
    if (!found) {
        OpBuilder::InsertionGuard guard(rewriter);
        rewriter.setInsertionPointToStart(mod.getBody());
        auto arrTy = LLVM::LLVMArrayType::get(rewriter.getI64Type(),
                                              static_cast<unsigned>(shape.size()));
        SmallVector<Attribute> dims;
        for (int64_t d : shape)
            dims.push_back(rewriter.getI64IntegerAttr(d));
        rewriter.create<LLVM::GlobalOp>(
            loc, arrTy, /*isConstant=*/true, LLVM::Linkage::Internal, sym,
            ArrayAttr::get(ctx, dims));
    }
    return rewriter.create<LLVM::AddressOfOp>(loc, ptrTy(ctx),
                                              FlatSymbolRefAttr::get(ctx, sym));
}

// ============================================================================
//  桥实现
// ============================================================================

/// 总元素数（静态形状）。
int64_t numel(ArrayRef<int64_t> shape) {
    int64_t n = 1;
    for (int64_t d : shape) n *= d;
    return n;
}

MemRefType flat1D(MLIRContext* ctx, ArrayRef<int64_t> shape, Type elem) {
    return MemRefType::get({numel(shape)}, elem);
}

MemRefType shaped(MLIRContext* ctx, ArrayRef<int64_t> shape, Type elem) {
    return MemRefType::get(shape, elem);
}

/// 1D memref → 多维连续视图（reinterpret_cast，零拷贝；静态 offset 0 /
/// 行主 strides）。注：本 LLVM 树 memref.expand_shape 无 ToLLVM 降级路径，
/// reinterpret_cast 走经典 ReinterpretCastOpLowering，完备可靠。
Value viewOf(ConversionPatternRewriter& rewriter, Location loc, Value flat,
             ArrayRef<int64_t> shape) {
    auto srcTy = cast<MemRefType>(flat.getType());
    auto dstTy = MemRefType::get(shape, srcTy.getElementType());
    // 行主 strides：stride[i] = prod(shape[i+1..])
    SmallVector<int64_t> strides(shape.size(), 1);
    for (int i = static_cast<int>(shape.size()) - 2; i >= 0; --i)
        strides[i] = strides[i + 1] * shape[i + 1];
    SmallVector<OpFoldResult> sizes, strds;
    for (int64_t d : shape) sizes.push_back(rewriter.getIndexAttr(d));
    for (int64_t s : strides) strds.push_back(rewriter.getIndexAttr(s));
    return rewriter.create<memref::ReinterpretCastOp>(
        loc, dstTy, flat, OpFoldResult(rewriter.getIndexAttr(0)), sizes, strds);
}

/// 裸数据指针 → memref 静态视图：手构 memref descriptor
/// {allocated, aligned, offset=0, sizes[], strides[]}（与
/// LLVMTypeConverter::getMemRefDescriptorFields unpackAggregates=false
/// 布局逐字段一致的字面量 struct）+ unrealized_conversion_cast 包装。
Value descViewOf(ConversionPatternRewriter& rewriter, Location loc, Value p,
                 ArrayRef<int64_t> shape, Type elem) {
    MLIRContext* ctx = rewriter.getContext();
    Type idx = rewriter.getI64Type();
    Type arrTy = LLVM::LLVMArrayType::get(idx, shape.size());
    auto descTy = LLVM::LLVMStructType::getLiteral(
        ctx, {ptrTy(ctx), ptrTy(ctx), idx, arrTy, arrTy});

    Value d = rewriter.create<LLVM::UndefOp>(loc, descTy);
    auto ins = [&](Value into, Value v, ArrayRef<int64_t> pos) {
        d = rewriter.create<LLVM::InsertValueOp>(loc, into, v, pos);
    };
    ins(d, p, {0}); // allocated
    ins(d, p, {1}); // aligned
    Value zero = rewriter.create<arith::ConstantOp>(
        loc, idx, rewriter.getI64IntegerAttr(0));
    ins(d, zero, {2}); // offset

    // 行主连续 sizes/strides：stride[i] = prod(shape[i+1..])
    SmallVector<int64_t> strides(shape.size(), 1);
    for (int i = static_cast<int>(shape.size()) - 2; i >= 0; --i)
        strides[i] = strides[i + 1] * shape[i + 1];
    Value szArr = rewriter.create<LLVM::UndefOp>(loc, arrTy);
    Value stArr = rewriter.create<LLVM::UndefOp>(loc, arrTy);
    for (auto [k, dim] : llvm::enumerate(shape)) {
        Value sz = rewriter.create<arith::ConstantOp>(
            loc, idx, rewriter.getI64IntegerAttr(dim));
        szArr = rewriter.create<LLVM::InsertValueOp>(
            loc, szArr, sz, ArrayRef<int64_t>{static_cast<int64_t>(k)});
    }
    for (auto [k, st] : llvm::enumerate(strides)) {
        Value c = rewriter.create<arith::ConstantOp>(
            loc, idx, rewriter.getI64IntegerAttr(st));
        stArr = rewriter.create<LLVM::InsertValueOp>(
            loc, stArr, c, ArrayRef<int64_t>{static_cast<int64_t>(k)});
    }
    d = rewriter.create<LLVM::InsertValueOp>(loc, d, szArr, ArrayRef<int64_t>{3});
    d = rewriter.create<LLVM::InsertValueOp>(loc, d, stArr, ArrayRef<int64_t>{4});

    return rewriter.create<UnrealizedConversionCastOp>(
                      loc, shaped(ctx, shape, elem), ValueRange{d})
        .getResult(0);
}

/// 输入桥（零拷贝）：句柄 → 数据指针 → descriptor 视图。
/// arena 内存纪律下无 free，句柄值 SSA 活性覆盖消费全程，只读别名安全。
Value viewInput(ConversionPatternRewriter& rewriter, Location loc,
                Value handle, ArrayRef<int64_t> shape, Type f64) {
    MLIRContext* ctx = rewriter.getContext();
    Value p = rewriter.create<func::CallOp>(
        loc, RtSyms::kData, TypeRange{ptrTy(ctx)}, ValueRange{handle})
                  .getResult(0);
    return descViewOf(rewriter, loc, p, shape, f64);
}

struct OutputBuf {
    Value handle;   ///< sg 句柄（arena，结果值）
    Value flat;     ///< 1D memref.alloc（拷贝/取指针用）
    Value view;     ///< 多维 identity 视图（linalg outs）
};

/// 输出侧准备：新句柄（sg_tensor_alloc）+ 结果 memref（1D alloc + expand）。
OutputBuf allocOutput(ConversionPatternRewriter& rewriter, Location loc,
                      ModuleOp mod, ArrayRef<int64_t> shape, Type f64) {
    MLIRContext* ctx = rewriter.getContext();
    Type p = ptrTy(ctx);
    Type i32 = rewriter.getI32Type();
    Type i64 = rewriter.getI64Type();
    Value isF64 = rewriter.create<arith::ConstantOp>(
        loc, i32, rewriter.getI32IntegerAttr(0));
    Value rank = rewriter.create<arith::ConstantOp>(
        loc, i64, rewriter.getI64IntegerAttr(
                      static_cast<int64_t>(shape.size())));
    Value shp = shapeAddr(rewriter, loc, mod, shape);
    Value h = rewriter.create<func::CallOp>(loc, RtSyms::kAlloc,
                                            TypeRange{p},
                                            ValueRange{isF64, rank, shp})
                  .getResult(0);
    Value flat = rewriter.create<memref::AllocOp>(loc, flat1D(ctx, shape, f64));
    return OutputBuf{h, flat, viewOf(rewriter, loc, flat, shape)};
}

/// 输出桥收尾：memref 存储指针 → sg_tensor_adopt 挂进句柄（零拷贝）。
void adoptOutput(ConversionPatternRewriter& rewriter, Location loc, Value h,
                 Value flat) {
    MLIRContext* ctx = rewriter.getContext();
    Value ap = rewriter.create<memref::ExtractAlignedPointerAsIndexOp>(
        loc, rewriter.getIndexType(), flat);
    Value ai = rewriter.create<arith::IndexCastUIOp>(loc, rewriter.getI64Type(),
                                                     ap);
    Value aptr = rewriter.create<LLVM::IntToPtrOp>(loc, ptrTy(ctx), ai);
    rewriter.create<func::CallOp>(loc, RtSyms::kAdopt, TypeRange{},
                                  ValueRange{h, aptr});
}

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
        rewriter.create<linalg::MatmulOp>(loc, ValueRange{a, b},
                                          ValueRange{out.view});
        adoptOutput(rewriter, loc, out.handle, out.flat);
        rewriter.replaceOp(op, out.handle);
        return success();
    }

    /// generic 逐元素构造（tt/ts/st 共用）：0/1/2 → addf/subf/mulf。
    /// block args：可索引输入元素在前、输出元素在后（标量输入经 region
    /// 捕获，不占 arg）；yield 值数 = 输出数。
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
        const bool isTT = name == "lang.tensor_binop_tt";
        const bool isST = name == "lang.tensor_binop_st";
        Value tensorOp = isTT ? operands[1] : (isST ? operands[4] : operands[1]);
        Value scalar = isTT ? Value() : operands[isST ? 2 : 4];

        Value a = viewInput(rewriter, loc, tensorOp, shape, f64);
        OutputBuf out = allocOutput(rewriter, loc, mod, shape, f64);

        AffineMap idMap = AffineMap::getMultiDimIdentityMap(
            static_cast<unsigned>(shape.size()), ctx);
        AffineMap emptyMap = AffineMap::get(ctx);
        SmallVector<AffineMap> maps;
        SmallVector<utils::IteratorType> iters(shape.size(),
                                               utils::IteratorType::parallel);
        SmallVector<Value> ins;
        if (isTT) { // 张量-张量：args = [a_elem, b_elem, out_elem]
            Value b = viewInput(rewriter, loc, operands[2], shape, f64);
            maps = {idMap, idMap, idMap};
            ins = {a, b};
        } else if (isST) { // 标量在左：subf(vf, t)；args = [t_elem, out_elem]
            maps = {emptyMap, idMap, idMap};
            ins = {scalar, a};
        } else { // ts 张量在左：subf(t, vf)；args = [t_elem, out_elem]
            maps = {idMap, emptyMap, idMap};
            ins = {a, scalar};
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

        declareBridgeExterns(mod);

        // partial conversion：lang 方言默认 legal（留给 LangToLLVM 兜底），
        // 仅覆盖集 op 在携带完整形状属性时动态 illegal → 由
        // LangLinalgRewrite 消费。
        ConversionTarget target(*ctx);
        target.addLegalDialect<BuiltinDialect, func::FuncDialect,
                               arith::ArithDialect, cf::ControlFlowDialect,
                               LLVM::LLVMDialect, memref::MemRefDialect,
                               linalg::LinalgDialect>();
        target.addLegalDialect<
            ::scourge::aot::lang::LangDialect>();
        // 覆盖集 op 仅在携带完整 linalg 形状属性时非法（动态合法回调：
        // 缺任一属性 → legal，由 LangToLLVM 兜底 sg_*；属性齐全 → 本 pass
        // 消费。panic 语义零分叉）。
        auto missingAttrs = [](std::initializer_list<llvm::StringRef> names) {
            return [names](Operation* op) {
                for (llvm::StringRef n : names)
                    if (!op->hasAttr(n)) return true; // 未覆盖形态 → legal
                return false; // 属性齐全 → 非法 → 降级
            };
        };
        target.addDynamicallyLegalOp<lang::TensorMatmulOp>(
            missingAttrs({"lhs_shape", "rhs_shape"}));
        target.addDynamicallyLegalOp<lang::TensorBinopTTOp>(
            missingAttrs({"kind", "shape"}));
        target.addDynamicallyLegalOp<lang::TensorBinopTSOp>(
            missingAttrs({"kind", "shape"}));
        target.addDynamicallyLegalOp<lang::TensorBinopSTOp>(
            missingAttrs({"kind", "shape"}));
        target.addDynamicallyLegalOp<lang::TensorSumFOp>(
            missingAttrs({"shape"}));
        target.addDynamicallyLegalOp<lang::TensorTransposeOp>(
            missingAttrs({"shape"}));

        RewritePatternSet patterns(ctx);
        patterns.add<LangLinalgRewrite>(ctx);
        if (failed(applyPartialConversion(mod, target, std::move(patterns))))
            signalPassFailure();
    }
};

} // namespace

std::unique_ptr<Pass> createLangToLinalgPass() {
    return std::make_unique<LangToLinalgPass>();
}

} // namespace aot
} // namespace scourge
