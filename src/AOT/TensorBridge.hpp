// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — AOT 张量桥共享设施（LangToLinalg / LangToAffine 共用）
// ----------------------------------------------------------------------------
//  sg_* 句柄世界 ↔ MLIR memref 世界的零拷贝双向桥（AOT 步骤 3a-2 抽出为
//  共享内部头，供 LangToLinalg.cpp 与 LangToAffine.cpp 消费，避免降级
//  pass 间复制粘贴）。
//
//  数据桥纪律（与 sg_* 句柄世界零拷贝衔接）：
//  * 输入：func.call @sg_tensor_data(句柄) 直取数据指针 → 手构 memref
//    descriptor（allocated=aligned=数据指针，offset=0，静态行主
//    sizes/strides，布局与 LLVMTypeConverter::getMemRefDescriptorFields
//    unpackAggregates=false 逐字段一致）→ unrealized_conversion_cast
//    包装为 memref 视图，零拷贝。arena 内存纪律下无 free，句柄值 SSA
//    活性覆盖消费全程，只读别名安全；下游 one-shot ConvertToLLVM 对该
//    cast 穿透直取 descriptor 本体（框架 materialization 同型机制）。
//  * 输出：memref.alloc（1D 连续）→ reinterpret_cast → 写入 →
//    memref.extract_aligned_pointer_as_index 取存储指针 →
//    func.call @sg_tensor_adopt(新句柄, 指针) 零拷贝接管 → 新句柄
//    （@sg_tensor_alloc arena 分配，与 sg_* 内存纪律一致）返回句柄域。
//
//  形状常量数组（sg_tensor_alloc 参数）以 llvm.mlir.global internal
//  constant 数组 + llvm.addressof 提供（按形状去重）。
//
//  桥函数声明/全局补插均幂等（SymbolTable 查重），多个降级 pass 依序
//  运行时重复调用安全。
// ============================================================================
#pragma once

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/SmallVector.h"

namespace scourge {
namespace aot {
namespace bridge {

using namespace mlir;

// ============================================================================
//  运行时桥符号（降级 pass 补插声明；定义在 src/Tool/Builder.cpp）
// ============================================================================
struct RtSyms {
    static constexpr const char* kData = "sg_tensor_data";  // (ptr) -> ptr
    static constexpr const char* kAlloc =
        "sg_tensor_alloc"; // (i32, i64, ptr) -> ptr
    static constexpr const char* kAdopt = "sg_tensor_adopt"; // (ptr, ptr) -> ()
};

inline Type ptrTy(MLIRContext* ctx) { return LLVM::LLVMPointerType::get(ctx); }

/// 模块级补插桥函数外部声明（private；重复运行幂等）。
inline void declareBridgeExterns(ModuleOp mod) {
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
inline Value shapeAddr(ConversionPatternRewriter& rewriter, Location loc,
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
inline int64_t numel(ArrayRef<int64_t> shape) {
    int64_t n = 1;
    for (int64_t d : shape) n *= d;
    return n;
}

inline MemRefType flat1D(MLIRContext* ctx, ArrayRef<int64_t> shape, Type elem) {
    return MemRefType::get({numel(shape)}, elem);
}

inline MemRefType shaped(MLIRContext* ctx, ArrayRef<int64_t> shape, Type elem) {
    return MemRefType::get(shape, elem);
}

/// 1D memref → 多维连续视图（reinterpret_cast，零拷贝；静态 offset 0 /
/// 行主 strides）。注：本 LLVM 树 memref.expand_shape 无 ToLLVM 降级路径，
/// reinterpret_cast 走经典 ReinterpretCastOpLowering，完备可靠。
inline Value viewOf(ConversionPatternRewriter& rewriter, Location loc,
                    Value flat, ArrayRef<int64_t> shape) {
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
inline Value descViewOf(ConversionPatternRewriter& rewriter, Location loc,
                        Value p, ArrayRef<int64_t> shape, Type elem) {
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
inline Value viewInput(ConversionPatternRewriter& rewriter, Location loc,
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
    Value view;     ///< 多维 identity 视图（降级写入目标）
};

/// 输出侧准备：新句柄（sg_tensor_alloc）+ 结果 memref（1D alloc + expand）。
/// 句柄 isInt 由元素类型分派（i64 → int 句柄，f64 → float 句柄；调用点
/// 仅传这两种元素类型）。
inline OutputBuf allocOutput(ConversionPatternRewriter& rewriter, Location loc,
                             ModuleOp mod, ArrayRef<int64_t> shape,
                             Type elem) {
    MLIRContext* ctx = rewriter.getContext();
    Type p = ptrTy(ctx);
    Type i32 = rewriter.getI32Type();
    Type i64 = rewriter.getI64Type();
    const bool isInt = isa<IntegerType>(elem);
    Value isIntC = rewriter.create<arith::ConstantOp>(
        loc, i32, rewriter.getI32IntegerAttr(isInt ? 1 : 0));
    Value rank = rewriter.create<arith::ConstantOp>(
        loc, i64, rewriter.getI64IntegerAttr(
                      static_cast<int64_t>(shape.size())));
    Value shp = shapeAddr(rewriter, loc, mod, shape);
    Value h = rewriter.create<func::CallOp>(loc, RtSyms::kAlloc,
                                            TypeRange{p},
                                            ValueRange{isIntC, rank, shp})
                  .getResult(0);
    Value flat = rewriter.create<memref::AllocOp>(loc, flat1D(ctx, shape, elem));
    return OutputBuf{h, flat, viewOf(rewriter, loc, flat, shape)};
}

/// 输出桥收尾：memref 存储指针 → sg_tensor_adopt 挂进句柄（零拷贝）。
inline void adoptOutput(ConversionPatternRewriter& rewriter, Location loc,
                        Value h, Value flat) {
    MLIRContext* ctx = rewriter.getContext();
    Value ap = rewriter.create<memref::ExtractAlignedPointerAsIndexOp>(
        loc, rewriter.getIndexType(), flat);
    Value ai = rewriter.create<arith::IndexCastUIOp>(loc, rewriter.getI64Type(),
                                                     ap);
    Value aptr = rewriter.create<LLVM::IntToPtrOp>(loc, ptrTy(ctx), ai);
    rewriter.create<func::CallOp>(loc, RtSyms::kAdopt, TypeRange{},
                                  ValueRange{h, aptr});
}

} // namespace bridge
} // namespace aot
} // namespace scourge
