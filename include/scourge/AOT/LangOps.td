// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — Lang 高层语义方言（ODS 定义，AOT 步骤 2）
// ----------------------------------------------------------------------------
//  分层纪律：
//  * MLIRGen 只落到本方言（lang.*）——lang 层不出现任何 @sg_* 运行时符号；
//  * LangToLLVM 降级 pass 把本方言一一还原为内嵌 C 运行时 @sg_* 调用
//    （降级表与声明补插表共用 LangToLLVM.cpp 的 kLowerings 单一事实源）；
//  * 后续 LangToLinalg 等后端 pass 直接消费本层语义。
//
//  设计约定：
//  * op 全集与 77 个 @sg_* 运行时符号一一对应（56 张量/标量/字符串/打印 +
//    21 Result/List；纯算术可降级 op——浮点
//    addf/subf/mulf/negf、全部比较、布尔 andi/ori/xori——保持上游 arith
//    方言，不重复收编；控制流保持 cf 方言；try/except 蹦床为唯一 func.call
//    直连运行时的控制流设施例外，声明由 MLIRGen assemble 补插）。
//  * panic 上下文（fn/file/line）不作为操作数出现：降级时由 op 的
//    Location（FileLineColLoc）与父 func 符号恢复。
//  * 汇编形式：不声明 assemblyFormat，统一使用 MLIR 通用形式
//    （generic form）解析与打印；MLIRGen 产出即通用形式文本。
//  * 类型：标量 i64/f64/i1/i32 + 不透明 !llvm.ptr（字符串与张量句柄共用，
//    与现行 sg_* ABI 一致）；自定义类型留待后续步骤。
// ============================================================================
include "mlir/IR/OpBase.td"

def Lang_Dialect : Dialect {
    let name = "lang";
    let cppNamespace = "::scourge::aot::lang";
    let summary = "Scourge 高层语义方言";
    let description = [{
        Scourge AOT 的高层 op 层：携带完整语义（溢出/除零/越界等 panic、
        字符串、打印、张量运算）。由 LangToLLVM 降级为 sg_* 运行时调用，
        或由后续 LangToLinalg 等后端 pass 消费。
    }];
}

/// !llvm.ptr：字符串与张量句柄共用（与现行 sg_* ABI 一致）。
def Lang_Ptr : Type<
    CPred<"::llvm::isa<::mlir::LLVM::LLVMPointerType>($_self)">,
    "LLVM 指针">;

class Lang_Op<string mnemonic, list<Trait> traits = []>
    : Op<Lang_Dialect, mnemonic, traits>;

// ============================================================================
// 标量整数算术（溢出/除零 panic 语义下沉运行时；'/' 真除恒返回 f64）
// ============================================================================
def Lang_AddiOp      : Lang_Op<"addi"> { let arguments = (ins I64:$lhs, I64:$rhs); let results = (outs I64:$res); }
def Lang_SubiOp      : Lang_Op<"subi"> { let arguments = (ins I64:$lhs, I64:$rhs); let results = (outs I64:$res); }
def Lang_MuliOp      : Lang_Op<"muli"> { let arguments = (ins I64:$lhs, I64:$rhs); let results = (outs I64:$res); }
def Lang_DiviOp      : Lang_Op<"divi"> { let arguments = (ins I64:$lhs, I64:$rhs); let results = (outs F64:$res); }
def Lang_FloordiviOp : Lang_Op<"floordivi"> { let arguments = (ins I64:$lhs, I64:$rhs); let results = (outs I64:$res); }
def Lang_ModiOp      : Lang_Op<"modi"> { let arguments = (ins I64:$lhs, I64:$rhs); let results = (outs I64:$res); }
def Lang_PowiOp      : Lang_Op<"powi"> { let arguments = (ins I64:$lhs, I64:$rhs); let results = (outs I64:$res); }
def Lang_NegiOp      : Lang_Op<"negi"> { let arguments = (ins I64:$val); let results = (outs I64:$res); }

// ============================================================================
// 标量浮点算术（除零/浮点溢出 panic 语义下沉运行时；addf/subf/mulf 用 arith）
// ============================================================================
def Lang_DivfOp      : Lang_Op<"divf"> { let arguments = (ins F64:$lhs, F64:$rhs); let results = (outs F64:$res); }
def Lang_FloordivfOp : Lang_Op<"floordivf"> { let arguments = (ins F64:$lhs, F64:$rhs); let results = (outs F64:$res); }
def Lang_ModfOp      : Lang_Op<"modf"> { let arguments = (ins F64:$lhs, F64:$rhs); let results = (outs F64:$res); }
def Lang_PowfOp      : Lang_Op<"powf"> { let arguments = (ins F64:$lhs, F64:$rhs); let results = (outs F64:$res); }

// ============================================================================
// 字符串运算（sg_str_* ABI）
// ============================================================================
def Lang_StrConcatOp     : Lang_Op<"str_concat"> { let arguments = (ins Lang_Ptr:$lhs, Lang_Ptr:$rhs); let results = (outs Lang_Ptr:$res); }
def Lang_StrRepeatOp     : Lang_Op<"str_repeat"> { let arguments = (ins Lang_Ptr:$str, I64:$count); let results = (outs Lang_Ptr:$res); }
def Lang_StrCmpOp        : Lang_Op<"str_cmp"> { let arguments = (ins Lang_Ptr:$lhs, Lang_Ptr:$rhs); let results = (outs I64:$res); }
def Lang_StrContainsOp   : Lang_Op<"str_contains"> { let arguments = (ins Lang_Ptr:$hay, Lang_Ptr:$needle); let results = (outs I32:$res); }
def Lang_StrLenOp        : Lang_Op<"str_len"> { let arguments = (ins Lang_Ptr:$str); let results = (outs I64:$res); }
def Lang_StrFromI64Op    : Lang_Op<"str_from_i64"> { let arguments = (ins I64:$val); let results = (outs Lang_Ptr:$res); }
def Lang_StrFromF64Op    : Lang_Op<"str_from_f64"> { let arguments = (ins F64:$val); let results = (outs Lang_Ptr:$res); }
def Lang_StrFromBoolOp   : Lang_Op<"str_from_bool"> { let arguments = (ins I1:$val); let results = (outs Lang_Ptr:$res); }
def Lang_StrFromTensorOp : Lang_Op<"str_from_tensor"> { let arguments = (ins Lang_Ptr:$val); let results = (outs Lang_Ptr:$res); }

// ============================================================================
// 数学内建（abs/sqrt/exp/log/now；标量子集）
// ============================================================================
def Lang_AbsIOp  : Lang_Op<"abs_i"> { let arguments = (ins I64:$val); let results = (outs I64:$res); }
def Lang_AbsFOp  : Lang_Op<"abs_f"> { let arguments = (ins F64:$val); let results = (outs F64:$res); }
def Lang_SqrtOp  : Lang_Op<"sqrt"> { let arguments = (ins F64:$val); let results = (outs F64:$res); }
def Lang_ExpOp   : Lang_Op<"exp"> { let arguments = (ins F64:$val); let results = (outs F64:$res); }
def Lang_LogOp   : Lang_Op<"log"> { let arguments = (ins F64:$val); let results = (outs F64:$res); }
def Lang_NowOp   : Lang_Op<"now"> { let arguments = (ins); let results = (outs F64:$res); }

// ============================================================================
// 数学内建 II（v0.9.3：标量三角/双曲/取整/随机；依 IEEE 754，不触碰张量路径）
// 取整族 float→int：NaN/±inf/结果超 int64 范围 panic（上下文由 Location 恢复）；
//   int 输入在 MLIRGen 直接恒等返回，不产生 op。round = 半到偶（nearbyint）。
// random/seed：splitmix64 确定性序列（与 VM 逐位一致），默认状态 0。
// ============================================================================
def Lang_SinOp   : Lang_Op<"sin">   { let arguments = (ins F64:$val); let results = (outs F64:$res); }
def Lang_CosOp   : Lang_Op<"cos">   { let arguments = (ins F64:$val); let results = (outs F64:$res); }
def Lang_TanOp   : Lang_Op<"tan">   { let arguments = (ins F64:$val); let results = (outs F64:$res); }
def Lang_AsinOp  : Lang_Op<"asin">  { let arguments = (ins F64:$val); let results = (outs F64:$res); }
def Lang_AcosOp  : Lang_Op<"acos">  { let arguments = (ins F64:$val); let results = (outs F64:$res); }
def Lang_AtanOp  : Lang_Op<"atan">  { let arguments = (ins F64:$val); let results = (outs F64:$res); }
def Lang_SinhOp  : Lang_Op<"sinh">  { let arguments = (ins F64:$val); let results = (outs F64:$res); }
def Lang_CoshOp  : Lang_Op<"cosh">  { let arguments = (ins F64:$val); let results = (outs F64:$res); }
def Lang_TanhOp  : Lang_Op<"tanh">  { let arguments = (ins F64:$val); let results = (outs F64:$res); }
def Lang_Atan2Op : Lang_Op<"atan2"> { let arguments = (ins F64:$y, F64:$x); let results = (outs F64:$res); }
def Lang_FloorOp : Lang_Op<"floor"> { let arguments = (ins F64:$val); let results = (outs I64:$res); }
def Lang_CeilOp  : Lang_Op<"ceil">  { let arguments = (ins F64:$val); let results = (outs I64:$res); }
def Lang_RoundOp : Lang_Op<"round"> { let arguments = (ins F64:$val); let results = (outs I64:$res); }
def Lang_TruncOp : Lang_Op<"trunc"> { let arguments = (ins F64:$val); let results = (outs I64:$res); }
def Lang_RandomOp : Lang_Op<"random"> { let arguments = (ins); let results = (outs F64:$res); }
def Lang_SeedOp   : Lang_Op<"seed">   { let arguments = (ins I64:$val); let results = (outs); }

// ============================================================================
// 系统编程内建（0.9.4 H 专项）：命令行参数/环境变量/时间日期/进程/信号。
// panic 语义下沉 sg_* 运行时（消息与 VMSys.cpp 逐字一致，双模差分由 verify
// 探针锁定）：localtime/gmtime 转换失败、spawn_wait 启动失败挂上下文；
// 其余纯查询不 panic。日历列表 = [年,月,日,时,分,秒,星期0=周日,年积日(1起)]。
// ============================================================================
def Lang_ArgsOp        : Lang_Op<"args">         { let arguments = (ins); let results = (outs Lang_Ptr:$res); }
def Lang_EnvOp         : Lang_Op<"env">          { let arguments = (ins Lang_Ptr:$name); let results = (outs Lang_Ptr:$res); }
def Lang_HasEnvOp      : Lang_Op<"has_env">      { let arguments = (ins Lang_Ptr:$name); let results = (outs I1:$res); }
def Lang_SetEnvOp      : Lang_Op<"set_env">      { let arguments = (ins Lang_Ptr:$name, Lang_Ptr:$value); let results = (outs I1:$res); }
def Lang_UnsetEnvOp    : Lang_Op<"unset_env">    { let arguments = (ins Lang_Ptr:$name); let results = (outs); }
def Lang_EpochOp       : Lang_Op<"epoch">        { let arguments = (ins); let results = (outs I64:$res); }
def Lang_EpochMsOp     : Lang_Op<"epoch_ms">     { let arguments = (ins); let results = (outs I64:$res); }
def Lang_LocaltimeOp   : Lang_Op<"localtime">    { let arguments = (ins I64:$ep); let results = (outs Lang_Ptr:$res); }
def Lang_GmtimeOp      : Lang_Op<"gmtime">       { let arguments = (ins I64:$ep); let results = (outs Lang_Ptr:$res); }
def Lang_SystemOp      : Lang_Op<"system">       { let arguments = (ins Lang_Ptr:$cmd); let results = (outs I64:$res); }
def Lang_SpawnWaitOp   : Lang_Op<"spawn_wait">   { let arguments = (ins Lang_Ptr:$path, Lang_Ptr:$args); let results = (outs I64:$res); }
def Lang_SignalCaughtOp : Lang_Op<"signal_caught"> { let arguments = (ins); let results = (outs I1:$res); }
def Lang_SignalResetOp : Lang_Op<"signal_reset"> { let arguments = (ins); let results = (outs); }

// ============================================================================
// @extern 真实现（0.9.4 H 专项）：动态库外部函数调用（MS x64 ABI）。
// args = [dll_ptr, sym_ptr, 实参...]；实参类型域限定 int/float/bool/str
// （i64/f64/i1/!llvm.ptr），返回可选（str 返回由降级层经 sg_extern_wrap_str
// 深拷贝）。解析失败/符号缺失 panic 语义下沉 sg_extern_resolve（消息与
// VMExtern.cpp 逐字一致）；dll/sym 为 NUL 结尾窄串直传。
// ============================================================================
def Lang_ExternCallOp : Lang_Op<"extern_call"> {
    let arguments = (ins Variadic<AnyType>:$args);
    let results = (outs Optional<AnyType>:$res);
}

// sorted(List) → 新列表：运行时按 SgList etag 泛型稳定升序（0=i64 1=f64
// 2=str 3=bool；int/float 混合仅 VM 可能，AOT 列表恒同质）。
def Lang_SortedOp : Lang_Op<"sorted"> { let arguments = (ins Lang_Ptr:$list); let results = (outs Lang_Ptr:$res); }

// ============================================================================
// 打印（print 参数逐个分派 + 空格/换行收尾，与 VM print 语义一致）
// ============================================================================
def Lang_PrintI64Op    : Lang_Op<"print_i64"> { let arguments = (ins I64:$val); let results = (outs); }
def Lang_PrintF64Op    : Lang_Op<"print_f64"> { let arguments = (ins F64:$val); let results = (outs); }
def Lang_PrintBoolOp   : Lang_Op<"print_bool"> { let arguments = (ins I1:$val); let results = (outs); }
def Lang_PrintStrOp    : Lang_Op<"print_str"> { let arguments = (ins Lang_Ptr:$val); let results = (outs); }
def Lang_PrintNoneOp   : Lang_Op<"print_none"> { let arguments = (ins); let results = (outs); }
def Lang_PrintSpaceOp  : Lang_Op<"print_space"> { let arguments = (ins); let results = (outs); }
def Lang_PrintNLOp     : Lang_Op<"print_nl"> { let arguments = (ins); let results = (outs); }
def Lang_PrintTensorOp : Lang_Op<"print_tensor"> { let arguments = (ins Lang_Ptr:$val); let results = (outs); }

// ============================================================================
// panic（消息文本与 VM::doBinOp / doBuiltin 逐字一致）
// ============================================================================
def Lang_PanicOp : Lang_Op<"panic"> { let arguments = (ins Lang_Ptr:$msg); let results = (outs); }

// ============================================================================
// 张量运算（SgTensor 句柄为 !llvm.ptr；kind 编码与 sg_* ABI 一致）
// ============================================================================
def Lang_TensorEmptyOp    : Lang_Op<"tensor_empty"> { let arguments = (ins); let results = (outs Lang_Ptr:$res); }
def Lang_TensorFromI64Op  : Lang_Op<"tensor_from_i64"> { let arguments = (ins I64:$count, Lang_Ptr:$elems); let results = (outs Lang_Ptr:$res); }
def Lang_TensorFromF64Op  : Lang_Op<"tensor_from_f64"> { let arguments = (ins I64:$count, Lang_Ptr:$elems); let results = (outs Lang_Ptr:$res); }
def Lang_TensorConcatOp   : Lang_Op<"tensor_concat"> { let arguments = (ins I64:$count, Lang_Ptr:$rows); let results = (outs Lang_Ptr:$res); }
def Lang_TensorBinopTTOp  : Lang_Op<"tensor_binop_tt"> { let arguments = (ins I32:$kind, Lang_Ptr:$lhs, Lang_Ptr:$rhs); let results = (outs Lang_Ptr:$res); }
def Lang_TensorBinopTSOp  : Lang_Op<"tensor_binop_ts"> { let arguments = (ins I32:$kind, Lang_Ptr:$lhs, I32:$isInt, I64:$si, F64:$sf); let results = (outs Lang_Ptr:$res); }
def Lang_TensorBinopSTOp  : Lang_Op<"tensor_binop_st"> { let arguments = (ins I32:$kind, I32:$isInt, I64:$si, F64:$sf, Lang_Ptr:$rhs); let results = (outs Lang_Ptr:$res); }
def Lang_TensorMatmulOp   : Lang_Op<"tensor_matmul"> { let arguments = (ins Lang_Ptr:$lhs, Lang_Ptr:$rhs); let results = (outs Lang_Ptr:$res); }
def Lang_TensorGetIOp     : Lang_Op<"tensor_get_i"> { let arguments = (ins Lang_Ptr:$tensor, I64:$ndim, Lang_Ptr:$idxs); let results = (outs I64:$res); }
def Lang_TensorGetFOp     : Lang_Op<"tensor_get_f"> { let arguments = (ins Lang_Ptr:$tensor, I64:$ndim, Lang_Ptr:$idxs); let results = (outs F64:$res); }
def Lang_TensorSliceOp    : Lang_Op<"tensor_slice"> { let arguments = (ins Lang_Ptr:$tensor, I64:$ndim, Lang_Ptr:$idxs); let results = (outs Lang_Ptr:$res); }
def Lang_TensorSetIOp     : Lang_Op<"tensor_set_i"> { let arguments = (ins Lang_Ptr:$tensor, I64:$ndim, Lang_Ptr:$idxs, I64:$val); let results = (outs); }
def Lang_TensorSetFOp     : Lang_Op<"tensor_set_f"> { let arguments = (ins Lang_Ptr:$tensor, I64:$ndim, Lang_Ptr:$idxs, F64:$val); let results = (outs); }
def Lang_TensorSumIOp     : Lang_Op<"tensor_sum_i"> { let arguments = (ins Lang_Ptr:$tensor); let results = (outs I64:$res); }
def Lang_TensorSumFOp     : Lang_Op<"tensor_sum_f"> { let arguments = (ins Lang_Ptr:$tensor); let results = (outs F64:$res); }
def Lang_TensorUnOp       : Lang_Op<"tensor_un"> { let arguments = (ins I32:$kind, Lang_Ptr:$tensor); let results = (outs Lang_Ptr:$res); }
def Lang_TensorReduceIOp  : Lang_Op<"tensor_reduce_i"> { let arguments = (ins I32:$kind, Lang_Ptr:$tensor, Lang_Ptr:$name); let results = (outs I64:$res); }
def Lang_TensorReduceFOp  : Lang_Op<"tensor_reduce_f"> { let arguments = (ins I32:$kind, Lang_Ptr:$tensor, Lang_Ptr:$name); let results = (outs F64:$res); }
def Lang_TensorTransposeOp : Lang_Op<"tensor_transpose"> { let arguments = (ins Lang_Ptr:$tensor); let results = (outs Lang_Ptr:$res); }
def Lang_TensorReshapeOp  : Lang_Op<"tensor_reshape"> { let arguments = (ins Lang_Ptr:$tensor, I64:$ndim, Lang_Ptr:$dims); let results = (outs Lang_Ptr:$res); }

// ============================================================================
// Result（错误即值；SgResult 堆句柄为 !llvm.ptr）
// 载荷按位编码：i64 原值 / f64 位型 memcpy / bool 0-1 / str 指针。
// unwrap(缺省)：ok 取载荷、失败态取缺省（与 VM CallMethod 语义逐字一致）。
// ============================================================================
def Lang_ResultOkIOp     : Lang_Op<"result_ok_i"> { let arguments = (ins I64:$val); let results = (outs Lang_Ptr:$res); }
def Lang_ResultOkFOp     : Lang_Op<"result_ok_f"> { let arguments = (ins F64:$val); let results = (outs Lang_Ptr:$res); }
def Lang_ResultOkBOp     : Lang_Op<"result_ok_b"> { let arguments = (ins I1:$val); let results = (outs Lang_Ptr:$res); }
def Lang_ResultOkSOp     : Lang_Op<"result_ok_s"> { let arguments = (ins Lang_Ptr:$val); let results = (outs Lang_Ptr:$res); }
def Lang_ResultOkNOp     : Lang_Op<"result_ok_n"> { let arguments = (ins); let results = (outs Lang_Ptr:$res); }
def Lang_ResultErrOp     : Lang_Op<"result_err"> { let arguments = (ins Lang_Ptr:$msg); let results = (outs Lang_Ptr:$res); }
def Lang_ResultIsOkOp    : Lang_Op<"result_is_ok"> { let arguments = (ins Lang_Ptr:$self); let results = (outs I1:$res); }
def Lang_ResultUnwrapIOp : Lang_Op<"result_unwrap_i"> { let arguments = (ins Lang_Ptr:$self, I64:$def); let results = (outs I64:$res); }
def Lang_ResultUnwrapFOp : Lang_Op<"result_unwrap_f"> { let arguments = (ins Lang_Ptr:$self, F64:$def); let results = (outs F64:$res); }
def Lang_ResultUnwrapBOp : Lang_Op<"result_unwrap_b"> { let arguments = (ins Lang_Ptr:$self, I1:$def); let results = (outs I1:$res); }
def Lang_ResultUnwrapSOp : Lang_Op<"result_unwrap_s"> { let arguments = (ins Lang_Ptr:$self, Lang_Ptr:$def); let results = (outs Lang_Ptr:$res); }

// ============================================================================
// List（SgList 堆句柄；etag 0=i64 1=f64 2=str 3=bool）
// 下标读取走 VM normIndex 语义（负下标归一 + 越界 panic），挂 panic 上下文。
// ============================================================================
def Lang_ListNewIOp  : Lang_Op<"list_new_i"> { let arguments = (ins I64:$count, Lang_Ptr:$elems); let results = (outs Lang_Ptr:$res); }
def Lang_ListNewFOp  : Lang_Op<"list_new_f"> { let arguments = (ins I64:$count, Lang_Ptr:$elems); let results = (outs Lang_Ptr:$res); }
def Lang_ListNewSOp  : Lang_Op<"list_new_s"> { let arguments = (ins I64:$count, Lang_Ptr:$elems); let results = (outs Lang_Ptr:$res); }
def Lang_ListNewBOp  : Lang_Op<"list_new_b"> { let arguments = (ins I64:$count, Lang_Ptr:$elems); let results = (outs Lang_Ptr:$res); }
def Lang_ListEmptyOp : Lang_Op<"list_empty"> { let arguments = (ins); let results = (outs Lang_Ptr:$res); }
def Lang_ListLenOp   : Lang_Op<"list_len"> { let arguments = (ins Lang_Ptr:$list); let results = (outs I64:$res); }
def Lang_ListGetIOp  : Lang_Op<"list_get_i"> { let arguments = (ins Lang_Ptr:$list, I64:$idx); let results = (outs I64:$res); }
def Lang_ListGetFOp  : Lang_Op<"list_get_f"> { let arguments = (ins Lang_Ptr:$list, I64:$idx); let results = (outs F64:$res); }
def Lang_ListGetBOp  : Lang_Op<"list_get_b"> { let arguments = (ins Lang_Ptr:$list, I64:$idx); let results = (outs I1:$res); }
def Lang_ListGetSOp  : Lang_Op<"list_get_s"> { let arguments = (ins Lang_Ptr:$list, I64:$idx); let results = (outs Lang_Ptr:$res); }
