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
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "scourge/AOT/LangOps.h"

#include "scourge/AOT/GpuCover.hpp"
#include "scourge/Common/Capability.hpp"

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
    const char* cap = nullptr; ///< 能力域名（Capability.hpp；需授权行非空，
                               ///< LangCallRewrite 前置 sg_cap_check 查位）
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
    {"lang.str_escape", "sg_str_escape", "p", "p", 'p', false}, // 0.9.6 元组 repr
    // ---- 数学内建 ----
    {"lang.abs_i", "sg_abs_i64", "i", "i", 'i', false},
    {"lang.abs_f", "sg_abs_f64", "f", "f", 'f', false},
    {"lang.sqrt", "sg_sqrt_f64", "f", "f", 'f', false},
    {"lang.exp", "sg_exp_f64", "f", "f", 'f', false},
    {"lang.log", "sg_log_f64", "f", "f", 'f', false},
    // v0.9.3 标量数学/随机/排序（三角/双曲不 panic；取整族越界 panic 挂上下文）
    {"lang.sin", "sg_sin_f64", "f", "f", 'f', false},
    {"lang.cos", "sg_cos_f64", "f", "f", 'f', false},
    {"lang.tan", "sg_tan_f64", "f", "f", 'f', false},
    {"lang.asin", "sg_asin_f64", "f", "f", 'f', false},
    {"lang.acos", "sg_acos_f64", "f", "f", 'f', false},
    {"lang.atan", "sg_atan_f64", "f", "f", 'f', false},
    {"lang.sinh", "sg_sinh_f64", "f", "f", 'f', false},
    {"lang.cosh", "sg_cosh_f64", "f", "f", 'f', false},
    {"lang.tanh", "sg_tanh_f64", "f", "f", 'f', false},
    {"lang.atan2", "sg_atan2_f64", "ff", "ff", 'f', false},
    {"lang.floor", "sg_floor_f64", "f", "fppi", 'i', true},
    {"lang.ceil", "sg_ceil_f64", "f", "fppi", 'i', true},
    {"lang.round", "sg_round_f64", "f", "fppi", 'i', true},
    {"lang.trunc", "sg_trunc_f64", "f", "fppi", 'i', true},
    {"lang.random", "sg_random", "", "", 'f', false},
    {"lang.seed", "sg_seed", "i", "i", 'v', false},
    {"lang.sorted", "sg_sorted", "p", "pppi", 'p', true},
    // ---- IO 内建（0.9.4）：失败 panic 的五个挂上下文；exists 纯查询不挂 ----
    {"lang.input", "sg_input", "", "ppi", 'p', true, "io"},
    {"lang.read_text", "sg_read_text", "p", "pppi", 'p', true, "io"},
    {"lang.write_text", "sg_write_text", "pp", "ppppi", 'i', true, "io"},
    {"lang.append_text", "sg_append_text", "pp", "ppppi", 'i', true, "io"},
    {"lang.exists", "sg_exists", "p", "p", 'b', false, "io"},
    {"lang.list_dir", "sg_list_dir", "p", "pppi", 'p', true, "io"},
    // ---- 网络/睡眠（0.9.4 F/G 专项）：句柄 I64 不透明；全部挂 panic 上下文 ----
    {"lang.net_connect", "sg_net_connect", "pi", "pippi", 'i', true, "net"},
    {"lang.net_send", "sg_net_send", "ip", "ipppi", 'i', true, "net"},
    {"lang.net_recv", "sg_net_recv", "ii", "iippi", 'p', true, "net"},
    {"lang.net_close", "sg_net_close", "i", "ippi", 'v', true, "net"},
    {"lang.net_udp_send", "sg_net_udp_send", "pip", "pipppi", 'i', true, "net"},
    {"lang.net_http_get", "sg_net_http_get", "p", "pppi", 'p', true, "net"},
    {"lang.sleep", "sg_sleep", "i", "ippi", 'v', true, "time"},
    // ---- F/G 迭代 2：TCP 服务端与收发超时 ----
    {"lang.net_bind", "sg_net_bind", "pi", "pippi", 'i', true, "net"},
    {"lang.net_accept", "sg_net_accept", "i", "ippi", 'i', true, "net"},
    {"lang.net_set_timeout", "sg_net_set_timeout", "ii", "iippi", 'v', true,
     "net"},
    {"lang.now", "sg_now", "", "", 'f', false, "time"},
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
    // ---- data_ptr（0.9.6 O 节 FFI 宽类型域；str 路径在 MLIRGen 就地
    // ptrtoint 不落 op；空张量 panic 消息与 VM doBuiltin 逐字一致） ----
    {"lang.data_ptr", "sg_data_ptr", "p", "pppi", 'i', true},
    // ---- Result（错误即值；载荷按位编码，unwrap 携带缺省值） ----
    {"lang.result_ok_i", "sg_result_ok_i", "i", "i", 'p', false},
    {"lang.result_ok_f", "sg_result_ok_f", "f", "f", 'p', false},
    {"lang.result_ok_b", "sg_result_ok_b", "b", "b", 'p', false},
    {"lang.result_ok_s", "sg_result_ok_s", "p", "p", 'p', false},
    {"lang.result_ok_n", "sg_result_ok_n", "", "", 'p', false},
    {"lang.result_err", "sg_result_err", "p", "p", 'p', false},
    {"lang.result_is_ok", "sg_result_is_ok", "p", "p", 'b', false},
    {"lang.result_unwrap_i", "sg_result_unwrap_i", "pi", "pi", 'i', false},
    {"lang.result_unwrap_f", "sg_result_unwrap_f", "pf", "pf", 'f', false},
    {"lang.result_unwrap_b", "sg_result_unwrap_b", "pb", "pb", 'b', false},
    {"lang.result_unwrap_s", "sg_result_unwrap_s", "pp", "pp", 'p', false},
    // ---- List（etag 0=i64 1=f64 2=str 3=bool；get 越界 panic 挂上下文） ----
    {"lang.list_new_i", "sg_list_new_i", "ip", "ip", 'p', false},
    {"lang.list_new_f", "sg_list_new_f", "ip", "ip", 'p', false},
    {"lang.list_new_s", "sg_list_new_s", "ip", "ip", 'p', false},
    {"lang.list_new_b", "sg_list_new_b", "ip", "ip", 'p', false},
    {"lang.list_empty", "sg_list_empty", "", "", 'p', false},
    {"lang.list_len", "sg_list_len", "p", "p", 'i', false},
    {"lang.list_get_i", "sg_list_get_i", "pi", "pippi", 'i', true},
    {"lang.list_get_f", "sg_list_get_f", "pi", "pippi", 'f', true},
    {"lang.list_get_b", "sg_list_get_b", "pi", "pippi", 'b', true},
    {"lang.list_get_s", "sg_list_get_s", "pi", "pippi", 'p', true},
    {"lang.list_in_i", "sg_list_in_i", "pi", "pi", 'b', false},
    {"lang.list_in_f", "sg_list_in_f", "pf", "pf", 'b', false},
    {"lang.list_in_b", "sg_list_in_b", "pb", "pb", 'b', false},
    {"lang.list_in_s", "sg_list_in_s", "pp", "pp", 'b', false},
    // ---- Dict（键/值统一 64 位槽 ABI；插入序迭代。new/set NaN 键、
    // get 缺键、key_at 越界 panic 挂上下文） ----
    {"lang.dict_new", "sg_dict_new", "IIipp", "IIippppi", 'p', true},
    {"lang.dict_len", "sg_dict_len", "p", "p", 'i', false},
    {"lang.dict_get", "sg_dict_get", "pi", "pippi", 'i', true},
    {"lang.dict_set", "sg_dict_set", "pii", "piippi", 'v', true},
    {"lang.dict_in", "sg_dict_in", "pi", "pi", 'b', false},
    {"lang.dict_key_at", "sg_dict_key_at", "pi", "pippi", 'i', true},
    // ---- Set（0.9.3：仅键表，复用 SgDict 链式哈希；元素语义 = Dict 键。
    // new NaN 元素、elem_at 越界 panic 挂上下文） ----
    {"lang.set_new", "sg_set_new", "Iip", "Iipppi", 'p', true},
    {"lang.set_len", "sg_set_len", "p", "p", 'i', false},
    {"lang.set_in", "sg_set_in", "pi", "pi", 'b', false},
    {"lang.set_elem_at", "sg_set_elem_at", "pi", "pippi", 'i', true},
    // ---- str / List 成员方法（0.9.4；panic 消息与 VM callValueMethod 逐字
    // 一致；contains 不设新 op——str 用 str_contains，List 用 list_in_*） ----
    {"lang.str_upper", "sg_str_upper", "p", "p", 'p', false},
    {"lang.str_lower", "sg_str_lower", "p", "p", 'p', false},
    {"lang.str_trim", "sg_str_trim", "p", "p", 'p', false},
    {"lang.str_trim_start", "sg_str_trim_start", "p", "p", 'p', false},
    {"lang.str_trim_end", "sg_str_trim_end", "p", "p", 'p', false},
    {"lang.str_split", "sg_str_split", "pp", "ppppi", 'p', true},
    {"lang.str_join", "sg_str_join", "pp", "ppppi", 'p', true},
    {"lang.str_replace", "sg_str_replace", "ppp", "pppppi", 'p', true},
    {"lang.str_find", "sg_str_find", "pp", "pp", 'i', false},
    {"lang.str_index", "sg_str_index", "pp", "ppppi", 'i', true},
    {"lang.str_startswith", "sg_str_startswith", "pp", "pp", 'I', false},
    {"lang.str_endswith", "sg_str_endswith", "pp", "pp", 'I', false},
    {"lang.list_index", "sg_list_index", "pIi", "pIippi", 'i', true},
    {"lang.list_count", "sg_list_count", "pIi", "pIi", 'i', false},
    {"lang.list_format", "sg_list_format", "p", "p", 'p', false},
    {"lang.print_list", "sg_print_list", "p", "p", 'v', false},
    // ---- 系统编程内建（0.9.4 H 专项）：panic 的三个挂上下文，其余纯查询 ----
    {"lang.args", "sg_args", "", "", 'p', false, "env"},
    {"lang.env", "sg_env", "p", "p", 'p', false, "env"},
    {"lang.has_env", "sg_has_env", "p", "p", 'b', false, "env"},
    {"lang.set_env", "sg_set_env", "pp", "pp", 'b', false, "env"},
    {"lang.unset_env", "sg_unset_env", "p", "p", 'v', false, "env"},
    {"lang.epoch", "sg_epoch", "", "", 'i', false, "time"},
    {"lang.epoch_ms", "sg_epoch_ms", "", "", 'i', false, "time"},
    {"lang.localtime", "sg_localtime", "i", "ippi", 'p', true, "time"},
    {"lang.gmtime", "sg_gmtime", "i", "ippi", 'p', true, "time"},
    {"lang.system", "sg_system", "p", "p", 'i', false, "proc"},
    {"lang.spawn_wait", "sg_spawn_wait", "pp", "ppppi", 'i', true, "proc"},
    {"lang.signal_caught", "sg_signal_caught", "", "", 'b', false, "signal"},
    {"lang.signal_reset", "sg_signal_reset", "", "", 'v', false, "signal"},
    // 0.9.4 M 专项：标准库广度（统计/倒序/哈希）
    {"lang.tensor_mean", "sg_tensor_mean", "p", "pppi", 'f', true},
    {"lang.tensor_var", "sg_tensor_var", "p", "pppi", 'f', true},
    {"lang.tensor_std", "sg_tensor_std", "p", "pppi", 'f', true},
    {"lang.list_mean", "sg_list_mean", "p", "pppi", 'f', true},
    {"lang.list_var", "sg_list_var", "p", "pppi", 'f', true},
    {"lang.list_std", "sg_list_std", "p", "pppi", 'f', true},
    {"lang.list_reversed", "sg_list_reversed", "p", "p", 'p', false},
    {"lang.str_reversed", "sg_str_reversed", "p", "p", 'p', false},
    {"lang.sha256", "sg_sha256", "p", "p", 'p', false},
    {"lang.crc32", "sg_crc32", "p", "p", 'i', false},
    // ---- JSON（0.9.4 M 专项：动态 Json 值；tag 序 0=null 1=bool 2=int
    //      3=float 4=str 5=arr 6=obj，与 JsonData::JK / SgJson 同序） ----
    {"lang.json_decode", "sg_json_decode", "p", "pppi", 'p', true},
    {"lang.json_is", "sg_json_is", "pI", "pI", 'b', false},
    {"lang.json_as_int", "sg_json_as_int", "p", "pppi", 'i', true},
    {"lang.json_as_float", "sg_json_as_float", "p", "pppi", 'f', true},
    {"lang.json_as_bool", "sg_json_as_bool", "p", "pppi", 'b', true},
    {"lang.json_as_str", "sg_json_as_str", "p", "pppi", 'p', true},
    {"lang.json_get_idx", "sg_json_get_idx", "pi", "pippi", 'p', true},
    {"lang.json_get_key", "sg_json_get_key", "pp", "pp", 'p', false},
    {"lang.json_len", "sg_json_len", "p", "pppi", 'i', true},
    {"lang.json_keys", "sg_json_keys", "p", "pppi", 'p', true},
    {"lang.json_dump", "sg_json_dump", "p", "p", 'p', false},
    {"lang.print_json", "sg_print_json", "p", "p", 'v', false},
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
        // failure 自动让位）；@extern 间接调用次之（benefit 5，抢在查表
        // LangCallRewrite 之前——kLowerings 无此 op，落到它会硬失败）；
        // 其余 lang op 走 @sg_* 运行时调用。
        patterns.add<LangInlineIntRewrite>(ctx);
        patterns.add<LangExternCallRewrite>(ctx);
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
        for (const OpLower& l : kLowerings) {
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
        for (const OpLower& l : kLowerings) {
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
