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
// 自拼接原地扩容（perf）：acc 为编译期发射点编号，运行时登记独占缓冲原地
// 追加；别名契约见 kRuntimeC sg_str_mark 注释与 docs/02 内存模型章。
def Lang_StrConcatGrowOp : Lang_Op<"str_concat_grow"> { let arguments = (ins I64:$acc, Lang_Ptr:$lhs, Lang_Ptr:$rhs); let results = (outs Lang_Ptr:$res); }
// str 别名登记：槽位存入非独占值/形参绑定/容器捕获编译端补发点
def Lang_StrMarkOp       : Lang_Op<"str_mark"> { let arguments = (ins Lang_Ptr:$s); let results = (outs); }
def Lang_StrRepeatOp     : Lang_Op<"str_repeat"> { let arguments = (ins Lang_Ptr:$str, I64:$count); let results = (outs Lang_Ptr:$res); }
def Lang_StrCmpOp        : Lang_Op<"str_cmp"> { let arguments = (ins Lang_Ptr:$lhs, Lang_Ptr:$rhs); let results = (outs I64:$res); }
def Lang_StrContainsOp   : Lang_Op<"str_contains"> { let arguments = (ins Lang_Ptr:$hay, Lang_Ptr:$needle); let results = (outs I32:$res); }
def Lang_StrLenOp        : Lang_Op<"str_len"> { let arguments = (ins Lang_Ptr:$str); let results = (outs I64:$res); }
def Lang_StrFromI64Op    : Lang_Op<"str_from_i64"> { let arguments = (ins I64:$val); let results = (outs Lang_Ptr:$res); }
def Lang_StrFromF64Op    : Lang_Op<"str_from_f64"> { let arguments = (ins F64:$val); let results = (outs Lang_Ptr:$res); }
def Lang_StrFromBoolOp   : Lang_Op<"str_from_bool"> { let arguments = (ins I1:$val); let results = (outs Lang_Ptr:$res); }
def Lang_StrFromTensorOp : Lang_Op<"str_from_tensor"> { let arguments = (ins Lang_Ptr:$val); let results = (outs Lang_Ptr:$res); }
// 0.9.6 元组 repr：带引号转义（repr 模式），实现复用 sg_list_escape 逐字节写出
def Lang_StrEscapeOp     : Lang_Op<"str_escape"> { let arguments = (ins Lang_Ptr:$str); let results = (outs Lang_Ptr:$res); }

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

// sorted(List) → 新列表：运行时按 SgList etag 泛型稳定升序（0=i64 1=f64
// 2=str 3=bool；int/float 混合仅 VM 可能，AOT 列表恒同质）。
def Lang_SortedOp : Lang_Op<"sorted"> { let arguments = (ins Lang_Ptr:$list); let results = (outs Lang_Ptr:$res); }

// ============================================================================
// IO 内建（0.9.4）：input/read_text/write_text/append_text/exists/list_dir。
// 路径与内容均为 NUL 结尾窄字节串（ANSI 代码页语义，与 VM 一致）；失败
// panic 语义下沉 sg_* 运行时（消息与 VM 逐字一致）；exists 纯查询不 panic。
// ============================================================================
def Lang_InputOp      : Lang_Op<"input">       { let arguments = (ins); let results = (outs Lang_Ptr:$res); }
def Lang_ReadTextOp   : Lang_Op<"read_text">   { let arguments = (ins Lang_Ptr:$path); let results = (outs Lang_Ptr:$res); }
def Lang_WriteTextOp  : Lang_Op<"write_text">  { let arguments = (ins Lang_Ptr:$path, Lang_Ptr:$data); let results = (outs I64:$res); }
def Lang_AppendTextOp : Lang_Op<"append_text"> { let arguments = (ins Lang_Ptr:$path, Lang_Ptr:$data); let results = (outs I64:$res); }
def Lang_ExistsOp     : Lang_Op<"exists">      { let arguments = (ins Lang_Ptr:$path); let results = (outs I1:$res); }
def Lang_ListDirOp    : Lang_Op<"list_dir">    { let arguments = (ins Lang_Ptr:$path); let results = (outs Lang_Ptr:$res); }

// ============================================================================
// 网络/睡眠内建（0.9.4 F/G 专项）：TCP/UDP 客户端 + HTTP GET + 阻塞睡眠。
// 句柄为不透明 I64（1 起始表下标，双模各自编号、行为一致）；失败 panic
// 语义下沉 sg_net_* 运行时（消息与 VM 逐字一致）；仅 http:// 明文。
// ============================================================================
def Lang_NetConnectOp : Lang_Op<"net_connect"> { let arguments = (ins Lang_Ptr:$host, I64:$port); let results = (outs I64:$res); }
def Lang_NetSendOp    : Lang_Op<"net_send">    { let arguments = (ins I64:$sock, Lang_Ptr:$data); let results = (outs I64:$res); }
def Lang_NetRecvOp    : Lang_Op<"net_recv">    { let arguments = (ins I64:$sock, I64:$maxb); let results = (outs Lang_Ptr:$res); }
def Lang_NetCloseOp   : Lang_Op<"net_close">   { let arguments = (ins I64:$sock); let results = (outs); }
def Lang_NetUdpSendOp : Lang_Op<"net_udp_send"> { let arguments = (ins Lang_Ptr:$host, I64:$port, Lang_Ptr:$data); let results = (outs I64:$res); }
def Lang_NetHttpGetOp : Lang_Op<"net_http_get"> { let arguments = (ins Lang_Ptr:$url); let results = (outs Lang_Ptr:$res); }
def Lang_NetHttpGetJsonOp : Lang_Op<"net_http_get_json"> { let arguments = (ins Lang_Ptr:$url); let results = (outs Lang_Ptr:$res); }
def Lang_SleepOp      : Lang_Op<"sleep">       { let arguments = (ins I64:$ms); let results = (outs); }
// F/G 迭代 2：TCP 服务端与收发超时。
def Lang_NetBindOp    : Lang_Op<"net_bind">    { let arguments = (ins Lang_Ptr:$host, I64:$port); let results = (outs I64:$res); }
def Lang_NetAcceptOp  : Lang_Op<"net_accept">  { let arguments = (ins I64:$sock); let results = (outs I64:$res); }
def Lang_NetSetTimeoutOp : Lang_Op<"net_set_timeout"> { let arguments = (ins I64:$sock, I64:$ms); let results = (outs); }
// 0.9.8 HTTP 服务端：读请求 → Json 动态值（EOF 零字节 → null）；写响应发
// 全部字节。解析/组包 panic 语义下沉 sg_*（消息与 VMNet.cpp 逐字一致）。
def Lang_NetHttpReadReqOp : Lang_Op<"net_http_read_request">  { let arguments = (ins I64:$sock); let results = (outs Lang_Ptr:$res); }
def Lang_NetHttpWriteRespOp : Lang_Op<"net_http_write_response"> { let arguments = (ins I64:$sock, I64:$status, Lang_Ptr:$ctype, Lang_Ptr:$body); let results = (outs I64:$res); }

// ============================================================================
// 真并发 M1（thread 域）：线程/锁/通道。句柄为不透明 I64（1 起始表下标，
// 双模各自编号）；失败 panic 语义下沉 sg_* 运行时（消息与 VMThread.cpp 逐字
// 一致）。thread_spawn 独带 sg_target（FlatSymbolRefAttr 指向当前模块的
// 0 参目标函数符号 @sg_<module>__<fn>）——降级层经 func.constant 取函数
// 地址传给 sg_thread_spawn（M1 处方：函数不作头等值，仅字面量符号）。
// ============================================================================
def Lang_ThreadSpawnOp : Lang_Op<"thread_spawn"> {
    let arguments = (ins Lang_Ptr:$name, FlatSymbolRefAttr:$sg_target);
    let results = (outs I64:$res);
}
def Lang_ThreadJoinOp : Lang_Op<"thread_join"> { let arguments = (ins I64:$h); let results = (outs); }
def Lang_LockNewOp : Lang_Op<"lock_new"> { let arguments = (ins); let results = (outs I64:$res); }
def Lang_LockAcquireOp : Lang_Op<"lock_acquire"> { let arguments = (ins I64:$lk); let results = (outs); }
def Lang_LockReleaseOp : Lang_Op<"lock_release"> { let arguments = (ins I64:$lk); let results = (outs); }
def Lang_ChannelNewOp : Lang_Op<"channel_new"> { let arguments = (ins); let results = (outs I64:$res); }
def Lang_ChanSendOp : Lang_Op<"chan_send"> { let arguments = (ins I64:$ch, Lang_Ptr:$msg); let results = (outs); }
def Lang_ChanRecvOp : Lang_Op<"chan_recv"> { let arguments = (ins I64:$ch); let results = (outs Lang_Ptr:$res); }
def Lang_ChanCloseOp : Lang_Op<"chan_close"> { let arguments = (ins I64:$ch); let results = (outs); }

// ============================================================================
// GUI M1+M2（gui 域，仅 AOT/Windows）：轮询式窗口内建。句柄为不透明 I64
// （1 起始静态表下标，窗口与控件同表，语义见 BuilderRTGui.hpp——事件契约
// 单一事实源：gui_poll 返回 Dict[str,str]，恒含 tag/x/y/key/text 五键，
// 空 Dict = 无事件）。VM 侧不发射（gui_* 运行到即固定 panic，见
// VMBuiltin.cpp）；失败 panic 语义下沉 sg_gui_*（消息与 VM 侧句柄校验
// 风格一致，非双模——本域无 VM 对端）。
// ============================================================================
def Lang_GuiWindowOp  : Lang_Op<"gui_window">  { let arguments = (ins Lang_Ptr:$title, I64:$w, I64:$h); let results = (outs I64:$res); }
def Lang_GuiCloseOp   : Lang_Op<"gui_close">   { let arguments = (ins I64:$win); let results = (outs); }
def Lang_GuiPollOp    : Lang_Op<"gui_poll">    { let arguments = (ins I64:$win); let results = (outs Lang_Ptr:$res); }
def Lang_GuiLabelOp   : Lang_Op<"gui_label">   { let arguments = (ins I64:$win, Lang_Ptr:$text, I64:$x, I64:$y, I64:$w, I64:$h); let results = (outs I64:$res); }
def Lang_GuiButtonOp  : Lang_Op<"gui_button">  { let arguments = (ins I64:$win, Lang_Ptr:$text, I64:$x, I64:$y, I64:$w, I64:$h); let results = (outs I64:$res); }
def Lang_GuiInputOp   : Lang_Op<"gui_input">   { let arguments = (ins I64:$win, I64:$x, I64:$y, I64:$w, I64:$h); let results = (outs I64:$res); }
def Lang_GuiSetTextOp : Lang_Op<"gui_set_text">{ let arguments = (ins I64:$h, Lang_Ptr:$text); let results = (outs); }
def Lang_GuiGetTextOp : Lang_Op<"gui_get_text">{ let arguments = (ins I64:$h); let results = (outs Lang_Ptr:$res); }
def Lang_GuiShowOp    : Lang_Op<"gui_show">    { let arguments = (ins I64:$h); let results = (outs); }
def Lang_GuiHideOp    : Lang_Op<"gui_hide">    { let arguments = (ins I64:$h); let results = (outs); }
def Lang_GuiTitleOp   : Lang_Op<"gui_title">   { let arguments = (ins I64:$win, Lang_Ptr:$title); let results = (outs); }

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
// variadic（0.9.6 O 节）：调用方是 ... 变参声明——降级层据此为间接 call
// 挂 var_callee_type，LLVM 后端按 MS x64 varargs 发射（浮点位模式进
// GP 位置槽 + xmm 位置槽 + AL），与 VM 位置式装载双模一致。
// ============================================================================
def Lang_ExternCallOp : Lang_Op<"extern_call"> {
    let arguments = (ins Variadic<AnyType>:$args, UnitAttr:$variadic);
    let results = (outs Optional<AnyType>:$res);
}

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
// data_ptr（0.9.6 O 节 FFI 宽类型域）：张量路径取连续原生缓冲首地址。
// str 路径不走此 op——MLIRGen 对 str 就地发射 llvm.ptrtoint（全局常量
// 进程期存活，与 VM intern 池同契约：地址只读透传）。空张量 panic 在
// 运行时 sg_data_ptr（消息与 VM doBuiltin 逐字一致）。
// ============================================================================
def Lang_DataPtrOp        : Lang_Op<"data_ptr"> { let arguments = (ins Lang_Ptr:$tensor); let results = (outs I64:$res); }

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
def Lang_ListInIOp   : Lang_Op<"list_in_i"> { let arguments = (ins Lang_Ptr:$list, I64:$key); let results = (outs I1:$res); }
def Lang_ListInFOp   : Lang_Op<"list_in_f"> { let arguments = (ins Lang_Ptr:$list, F64:$key); let results = (outs I1:$res); }
def Lang_ListInBOp   : Lang_Op<"list_in_b"> { let arguments = (ins Lang_Ptr:$list, I1:$key); let results = (outs I1:$res); }
def Lang_ListInSOp   : Lang_Op<"list_in_s"> { let arguments = (ins Lang_Ptr:$list, Lang_Ptr:$key); let results = (outs I1:$res); }

// ============================================================================
// Dict（SgDict 堆句柄；插入序迭代。键/值 ABI 统一 64 位槽：int 直通、
// float bitcast、bool zext、str/句柄 ptrtoint。ktag/vtag 0=i64 1=f64 2=str
// 3=bool 4=句柄。get 缺键 / set·new NaN 键 panic 挂上下文）
// ============================================================================
def Lang_DictNewOp   : Lang_Op<"dict_new"> { let arguments = (ins I32:$ktag, I32:$vtag, I64:$count, Lang_Ptr:$keys, Lang_Ptr:$vals); let results = (outs Lang_Ptr:$res); }
def Lang_DictLenOp   : Lang_Op<"dict_len"> { let arguments = (ins Lang_Ptr:$d); let results = (outs I64:$res); }
def Lang_DictGetOp   : Lang_Op<"dict_get"> { let arguments = (ins Lang_Ptr:$d, I64:$key); let results = (outs I64:$res); }
def Lang_DictSetOp   : Lang_Op<"dict_set"> { let arguments = (ins Lang_Ptr:$d, I64:$key, I64:$val); let results = (outs); }
def Lang_DictInOp    : Lang_Op<"dict_in"> { let arguments = (ins Lang_Ptr:$d, I64:$key); let results = (outs I1:$res); }
def Lang_DictKeyAtOp : Lang_Op<"dict_key_at"> { let arguments = (ins Lang_Ptr:$d, I64:$idx); let results = (outs I64:$res); }

// ============================================================================
// Set（v0.9.3；复用 SgDict 堆句柄，v 槽恒 0、vtag=4 哨兵。语义 = Dict 键：
// 数值跨类型同键、NaN 拒绝、插入序去重。tag 0=i64 1=f64 2=str 3=bool 4=句柄。
// in / new NaN 元素 panic 挂上下文）
// ============================================================================
def Lang_SetNewOp    : Lang_Op<"set_new"> { let arguments = (ins I32:$tag, I64:$count, Lang_Ptr:$elems); let results = (outs Lang_Ptr:$res); }
def Lang_SetLenOp    : Lang_Op<"set_len"> { let arguments = (ins Lang_Ptr:$s); let results = (outs I64:$res); }
def Lang_SetInOp     : Lang_Op<"set_in"> { let arguments = (ins Lang_Ptr:$s, I64:$key); let results = (outs I1:$res); }
def Lang_SetElemAtOp : Lang_Op<"set_elem_at"> { let arguments = (ins Lang_Ptr:$s, I64:$idx); let results = (outs I64:$res); }

// ============================================================================
// str / List 成员方法（0.9.4；纯函数。panic 消息与 VM callValueMethod 逐字
// 一致；upper/lower 仅 ASCII，下标/查找按 UTF-8 字节；List 键 ABI = dict
// 键 64 位槽：tag 0=i64 1=f64 2=str 3=bool）
// contains 不设新 op：str 复用 str_contains，List 复用 list_in_*。
// ============================================================================
def Lang_StrUpperOp      : Lang_Op<"str_upper"> { let arguments = (ins Lang_Ptr:$str); let results = (outs Lang_Ptr:$res); }
def Lang_StrLowerOp      : Lang_Op<"str_lower"> { let arguments = (ins Lang_Ptr:$str); let results = (outs Lang_Ptr:$res); }
def Lang_StrTrimOp       : Lang_Op<"str_trim"> { let arguments = (ins Lang_Ptr:$str); let results = (outs Lang_Ptr:$res); }
def Lang_StrTrimStartOp  : Lang_Op<"str_trim_start"> { let arguments = (ins Lang_Ptr:$str); let results = (outs Lang_Ptr:$res); }
def Lang_StrTrimEndOp    : Lang_Op<"str_trim_end"> { let arguments = (ins Lang_Ptr:$str); let results = (outs Lang_Ptr:$res); }
def Lang_StrSplitOp      : Lang_Op<"str_split"> { let arguments = (ins Lang_Ptr:$str, Lang_Ptr:$sep); let results = (outs Lang_Ptr:$res); }
def Lang_StrJoinOp       : Lang_Op<"str_join"> { let arguments = (ins Lang_Ptr:$sep, Lang_Ptr:$parts); let results = (outs Lang_Ptr:$res); }
// List.append：原地尾插，返回 ()。共享语义所有别名可见（与 VM
// shared_ptr push_back 对齐）；非纯（修改状态）。str 版 + 标量域
// i/f/b 版（0.9.8）：空列表首插定型 etag，检查器保证元素一致。
def Lang_ListAppendOp    : Lang_Op<"list_append"> { let arguments = (ins Lang_Ptr:$list, Lang_Ptr:$val); let results = (outs); }
def Lang_ListAppendIOp   : Lang_Op<"list_append_i"> { let arguments = (ins Lang_Ptr:$list, I64:$val); let results = (outs); }
def Lang_ListAppendFOp   : Lang_Op<"list_append_f"> { let arguments = (ins Lang_Ptr:$list, F64:$val); let results = (outs); }
def Lang_ListAppendBOp   : Lang_Op<"list_append_b"> { let arguments = (ins Lang_Ptr:$list, I1:$val); let results = (outs); }
// 下标写（批 7 前置）：List 标量域原地覆盖；与 VM StoreIndex list 分支
// 对齐（单下标/负下标归一/越界 panic 逐字一致）。str 版槽为 sg_dup 副本。
def Lang_ListStoreIOp    : Lang_Op<"list_store_i"> { let arguments = (ins Lang_Ptr:$list, I64:$idx, I64:$val); let results = (outs); }
def Lang_ListStoreFOp    : Lang_Op<"list_store_f"> { let arguments = (ins Lang_Ptr:$list, I64:$idx, F64:$val); let results = (outs); }
def Lang_ListStoreBOp    : Lang_Op<"list_store_b"> { let arguments = (ins Lang_Ptr:$list, I64:$idx, I1:$val); let results = (outs); }
def Lang_ListStoreSOp    : Lang_Op<"list_store_s"> { let arguments = (ins Lang_Ptr:$list, I64:$idx, Lang_Ptr:$val); let results = (outs); }
def Lang_StrReplaceOp    : Lang_Op<"str_replace"> { let arguments = (ins Lang_Ptr:$str, Lang_Ptr:$old, Lang_Ptr:$repl); let results = (outs Lang_Ptr:$res); }
def Lang_StrFindOp       : Lang_Op<"str_find"> { let arguments = (ins Lang_Ptr:$hay, Lang_Ptr:$needle); let results = (outs I64:$res); }
def Lang_StrSliceOp      : Lang_Op<"str_slice"> { let arguments = (ins Lang_Ptr:$str, I64:$begin, I64:$end); let results = (outs Lang_Ptr:$res); }
def Lang_StrIndexOp      : Lang_Op<"str_index"> { let arguments = (ins Lang_Ptr:$hay, Lang_Ptr:$needle); let results = (outs I64:$res); }
def Lang_StrStartsWithOp : Lang_Op<"str_startswith"> { let arguments = (ins Lang_Ptr:$str, Lang_Ptr:$pre); let results = (outs I32:$res); }
def Lang_StrEndsWithOp   : Lang_Op<"str_endswith"> { let arguments = (ins Lang_Ptr:$str, Lang_Ptr:$suf); let results = (outs I32:$res); }
def Lang_ListIndexOp     : Lang_Op<"list_index"> { let arguments = (ins Lang_Ptr:$list, I32:$tag, I64:$key); let results = (outs I64:$res); }
def Lang_ListCountOp     : Lang_Op<"list_count"> { let arguments = (ins Lang_Ptr:$list, I32:$tag, I64:$key); let results = (outs I64:$res); }

// list repr 格式化与打印（0.9.4）：与 VM toString(list, repr=true) 逐字一致
// （"[" + 元素 ", " 连接 + "]"；str 带引号转义；bool True/False；浮点最短
// 往返补 ".0"），print(List) 与 len 同为 AOT 基础设施。
def Lang_ListFormatOp : Lang_Op<"list_format"> { let arguments = (ins Lang_Ptr:$list); let results = (outs Lang_Ptr:$res); }
def Lang_PrintListOp  : Lang_Op<"print_list"> { let arguments = (ins Lang_Ptr:$val); let results = (outs); }
def Lang_PrintDictOp  : Lang_Op<"print_dict"> { let arguments = (ins Lang_Ptr:$val); let results = (outs); }
def Lang_StrChrAtOp   : Lang_Op<"str_chr_at"> { let arguments = (ins Lang_Ptr:$s, I64:$i); let results = (outs Lang_Ptr:$res); }

// 标准库广度（0.9.4 M 专项）：统计（总体方差 ÷N，恒 f64）/ 倒序 / 哈希。
// 张量与列表各一套统计入口（运行时结构不同）；空序列 panic 双模一致。
def Lang_TensorMeanOp : Lang_Op<"tensor_mean"> { let arguments = (ins Lang_Ptr:$t); let results = (outs F64:$res); }
def Lang_TensorVarOp  : Lang_Op<"tensor_var">  { let arguments = (ins Lang_Ptr:$t); let results = (outs F64:$res); }
def Lang_TensorStdOp  : Lang_Op<"tensor_std">  { let arguments = (ins Lang_Ptr:$t); let results = (outs F64:$res); }
def Lang_TensorMedianOp : Lang_Op<"tensor_median"> { let arguments = (ins Lang_Ptr:$t); let results = (outs F64:$res); }
def Lang_ListMeanOp   : Lang_Op<"list_mean">   { let arguments = (ins Lang_Ptr:$l); let results = (outs F64:$res); }
def Lang_ListVarOp    : Lang_Op<"list_var">    { let arguments = (ins Lang_Ptr:$l); let results = (outs F64:$res); }
def Lang_ListStdOp    : Lang_Op<"list_std">    { let arguments = (ins Lang_Ptr:$l); let results = (outs F64:$res); }
def Lang_ListMedianOp : Lang_Op<"list_median"> { let arguments = (ins Lang_Ptr:$l); let results = (outs F64:$res); }
def Lang_ListReversedOp : Lang_Op<"list_reversed"> { let arguments = (ins Lang_Ptr:$l); let results = (outs Lang_Ptr:$res); }
def Lang_StrReversedOp  : Lang_Op<"str_reversed">  { let arguments = (ins Lang_Ptr:$s); let results = (outs Lang_Ptr:$res); }
def Lang_Sha256Op : Lang_Op<"sha256"> { let arguments = (ins Lang_Ptr:$s); let results = (outs Lang_Ptr:$res); }
def Lang_Crc32Op  : Lang_Op<"crc32">  { let arguments = (ins Lang_Ptr:$s); let results = (outs I64:$res); }
// ---- JSON（0.9.4 M 专项：动态 Json 值；tag 序 0=null 1=bool 2=int 3=float 4=str 5=arr 6=obj） ----
def Lang_JsonDecodeOp  : Lang_Op<"json_decode">   { let arguments = (ins Lang_Ptr:$s); let results = (outs Lang_Ptr:$res); }
def Lang_JsonIsOp      : Lang_Op<"json_is">       { let arguments = (ins Lang_Ptr:$j, I32:$tag); let results = (outs I1:$res); }
def Lang_JsonAsIntOp   : Lang_Op<"json_as_int">   { let arguments = (ins Lang_Ptr:$j); let results = (outs I64:$res); }
def Lang_JsonAsFloatOp : Lang_Op<"json_as_float"> { let arguments = (ins Lang_Ptr:$j); let results = (outs F64:$res); }
def Lang_JsonAsBoolOp  : Lang_Op<"json_as_bool">  { let arguments = (ins Lang_Ptr:$j); let results = (outs I1:$res); }
def Lang_JsonAsStrOp   : Lang_Op<"json_as_str">   { let arguments = (ins Lang_Ptr:$j); let results = (outs Lang_Ptr:$res); }
def Lang_JsonGetIdxOp  : Lang_Op<"json_get_idx">  { let arguments = (ins Lang_Ptr:$j, I64:$idx); let results = (outs Lang_Ptr:$res); }
def Lang_JsonGetKeyOp  : Lang_Op<"json_get_key">  { let arguments = (ins Lang_Ptr:$j, Lang_Ptr:$k); let results = (outs Lang_Ptr:$res); }
def Lang_JsonLenOp     : Lang_Op<"json_len">      { let arguments = (ins Lang_Ptr:$j); let results = (outs I64:$res); }
def Lang_JsonKeysOp    : Lang_Op<"json_keys">     { let arguments = (ins Lang_Ptr:$j); let results = (outs Lang_Ptr:$res); }
def Lang_JsonDumpOp    : Lang_Op<"json_dump">     { let arguments = (ins Lang_Ptr:$j); let results = (outs Lang_Ptr:$res); }
def Lang_PrintJsonOp   : Lang_Op<"print_json">    { let arguments = (ins Lang_Ptr:$val); let results = (outs); }

// ============================================================================
// 类型转换 / 码点 / List 归约 / 容器视图（0.9.9）：
// int/float/ord/chr panic 语义下沉运行时（消息与 VM doBuiltin 逐字一致，
// float 不判 errno——strtod 上溢 → ±inf 与 Python 同语义，仅全消费校验）；
// list_reduce_i/f 双入口（op 0=sum 1=min 2=max，name 供空序列 panic 消息）；
// dict 视图（keys/values/items）/ get 带缺省 / set add；dict/set 的 str
// 格式化复用 print 系（0.9.8）的 sg_dict_format / 新增 sg_set_format。
// ============================================================================
def Lang_StrToIOp     : Lang_Op<"str_to_i64">   { let arguments = (ins Lang_Ptr:$s); let results = (outs I64:$res); }
def Lang_StrToFOp     : Lang_Op<"str_to_f64">   { let arguments = (ins Lang_Ptr:$s); let results = (outs F64:$res); }
def Lang_IntFromF64Op : Lang_Op<"int_from_f64"> { let arguments = (ins F64:$val); let results = (outs I64:$res); }
def Lang_OrdOp        : Lang_Op<"ord">          { let arguments = (ins Lang_Ptr:$s); let results = (outs I64:$res); }
def Lang_ChrOp        : Lang_Op<"chr">          { let arguments = (ins I64:$cp); let results = (outs Lang_Ptr:$res); }
def Lang_ListReduceIOp : Lang_Op<"list_reduce_i"> { let arguments = (ins I32:$op, Lang_Ptr:$list, Lang_Ptr:$name); let results = (outs I64:$res); }
def Lang_ListReduceFOp : Lang_Op<"list_reduce_f"> { let arguments = (ins I32:$op, Lang_Ptr:$list, Lang_Ptr:$name); let results = (outs F64:$res); }
def Lang_StrCountOp   : Lang_Op<"str_count">    { let arguments = (ins Lang_Ptr:$hay, Lang_Ptr:$needle); let results = (outs I64:$res); }
def Lang_DictFormatOp : Lang_Op<"dict_format"> { let arguments = (ins Lang_Ptr:$d); let results = (outs Lang_Ptr:$res); }
def Lang_SetFormatOp  : Lang_Op<"set_format">  { let arguments = (ins Lang_Ptr:$s); let results = (outs Lang_Ptr:$res); }
def Lang_DictKeysOp   : Lang_Op<"dict_keys">   { let arguments = (ins Lang_Ptr:$d); let results = (outs Lang_Ptr:$res); }
def Lang_DictValuesOp : Lang_Op<"dict_values"> { let arguments = (ins Lang_Ptr:$d); let results = (outs Lang_Ptr:$res); }
def Lang_DictItemsOp  : Lang_Op<"dict_items">  { let arguments = (ins Lang_Ptr:$d); let results = (outs Lang_Ptr:$res); }
def Lang_DictGetOrOp  : Lang_Op<"dict_get_or"> { let arguments = (ins Lang_Ptr:$d, I64:$key, I64:$def); let results = (outs I64:$res); }
def Lang_SetAddOp     : Lang_Op<"set_add">     { let arguments = (ins Lang_Ptr:$s, I64:$val); let results = (outs); }
// ---- 切片 s[a:b]（0.9.9）：ABI 5 参 [base,a,b,hasA,hasB]，归一公式与
// VM Slice2 逐字一致（负下标+len 钳 0、越界钳 len、b<a 空切片）；纯函数
// 无 panic 路径，List 浅拷贝保留原 etag，str 字节切片。 ----
def Lang_StrNsliceOp  : Lang_Op<"str_nslice">  { let arguments = (ins Lang_Ptr:$s, I64:$a, I64:$b, I64:$ha, I64:$hb); let results = (outs Lang_Ptr:$res); }
def Lang_ListNsliceOp : Lang_Op<"list_nslice"> { let arguments = (ins Lang_Ptr:$l, I64:$a, I64:$b, I64:$ha, I64:$hb); let results = (outs Lang_Ptr:$res); }
// ---- fmt 格式化（0.9.9）：f-string :spec 降糖目标，四层按值类型分派；
// 纯函数（spec 静态校验在 Sema），实现与 VM doBuiltin "fmt" 逐字同构。 ----
def Lang_FmtI64Op  : Lang_Op<"fmt_i64">  { let arguments = (ins I64:$v, Lang_Ptr:$spec); let results = (outs Lang_Ptr:$res); }
def Lang_FmtF64Op  : Lang_Op<"fmt_f64">  { let arguments = (ins F64:$v, Lang_Ptr:$spec); let results = (outs Lang_Ptr:$res); }
def Lang_FmtStrOp  : Lang_Op<"fmt_str">  { let arguments = (ins Lang_Ptr:$v, Lang_Ptr:$spec); let results = (outs Lang_Ptr:$res); }
def Lang_FmtBoolOp : Lang_Op<"fmt_bool"> { let arguments = (ins I1:$v, Lang_Ptr:$spec); let results = (outs Lang_Ptr:$res); }
