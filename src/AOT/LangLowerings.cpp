// ============================================================================
//  Scourge 编译器 — AOT Lang 降级表
// ----------------------------------------------------------------------------
//  职责边界：kLowerings 唯一事实源——
//  lang.* → @sg_* 运行时符号与 ABI 类型码。
//  LangToLLVM.cpp 只查表消费；新增 lang op 先在此登记。
// ============================================================================
#include "scourge/AOT/LangLowerings.hpp"

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
    // 自拼接原地扩容（acc=发射点编号）与别名登记（见 kRuntimeC mark 契约）
    {"lang.str_concat_grow", "sg_str_concat_grow", "ipp", "ipp", 'p', false},
    {"lang.str_mark", "sg_str_mark", "p", "p", 'v', false},
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
    {"lang.net_http_get_json", "sg_net_http_get_json", "p", "pppi", 'p', true,
     "net"},
    {"lang.sleep", "sg_sleep", "i", "ippi", 'v', true, "time"},
    // ---- F/G 迭代 2：TCP 服务端与收发超时 ----
    {"lang.net_bind", "sg_net_bind", "pi", "pippi", 'i', true, "net"},
    {"lang.net_accept", "sg_net_accept", "i", "ippi", 'i', true, "net"},
    {"lang.net_set_timeout", "sg_net_set_timeout", "ii", "iippi", 'v', true,
     "net"},
    // ---- 0.9.8 HTTP 服务端 ----
    {"lang.net_http_read_request", "sg_net_http_read_request", "i", "ippi",
     'p', true, "net"},
    {"lang.net_http_write_response", "sg_net_http_write_response", "iipp",
     "iippppi", 'i', true, "net"},
    // ---- 真并发 M1（thread 域）：句柄 I64 不透明；全部挂 panic 上下文。
    // thread_spawn 不走通用表——独带 sg_target 符号属性，由特写 pattern
    // 经 func.constant 取函数地址（见 LangThreadSpawnRewrite）。
    {"lang.thread_join", "sg_thread_join", "i", "ippi", 'v', true, "thread"},
    {"lang.lock_new", "sg_lock_new", "", "ppi", 'i', true, "thread"},
    {"lang.lock_acquire", "sg_lock_acquire", "i", "ippi", 'v', true,
     "thread"},
    {"lang.lock_release", "sg_lock_release", "i", "ippi", 'v', true,
     "thread"},
    {"lang.channel_new", "sg_channel_new", "", "ppi", 'i', true, "thread"},
    {"lang.chan_send", "sg_chan_send", "ip", "ipppi", 'v', true, "thread"},
    {"lang.chan_recv", "sg_chan_recv", "i", "ippi", 'p', true, "thread"},
    {"lang.chan_close", "sg_chan_close", "i", "ippi", 'v', true, "thread"},
    // ---- GUI M1+M2（gui 域，仅 AOT/Windows）：句柄 I64 不透明；全部挂
    // panic 上下文 + sg_cap_check 查位。事件契约见 BuilderRTGui.hpp。
    {"lang.gui_window", "sg_gui_window", "pii", "piippi", 'i', true, "gui"},
    {"lang.gui_label", "sg_gui_label", "ipiiii", "ipiiiippi", 'i', true,
     "gui"},
    {"lang.gui_button", "sg_gui_button", "ipiiii", "ipiiiippi", 'i', true,
     "gui"},
    {"lang.gui_input", "sg_gui_input", "iiiii", "iiiiippi", 'i', true,
     "gui"},
    {"lang.gui_poll", "sg_gui_poll", "i", "ippi", 'p', true, "gui"},
    {"lang.gui_set_text", "sg_gui_set_text", "ip", "ipppi", 'v', true,
     "gui"},
    {"lang.gui_get_text", "sg_gui_get_text", "i", "ippi", 'p', true, "gui"},
    {"lang.gui_show", "sg_gui_show", "i", "ippi", 'v', true, "gui"},
    {"lang.gui_hide", "sg_gui_hide", "i", "ippi", 'v', true, "gui"},
    {"lang.gui_title", "sg_gui_title", "ip", "ipppi", 'v', true, "gui"},
    {"lang.gui_close", "sg_gui_close", "i", "ippi", 'v', true, "gui"},
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
    {"lang.list_append", "sg_list_append", "pp", "ppppi", 'v', true},
    {"lang.list_append_i", "sg_list_append_i", "pi", "pippi", 'v', true},
    {"lang.list_append_f", "sg_list_append_f", "pf", "pfppi", 'v', true},
    {"lang.list_append_b", "sg_list_append_b", "pb", "pbppi", 'v', true},
    // 下标写（批 7 前置）：单下标 + 值，ctx 追加；str 槽 sg_dup 副本
    {"lang.list_store_i", "sg_list_store_i", "pii", "piippi", 'v', true},
    {"lang.list_store_f", "sg_list_store_f", "pif", "pifppi", 'v', true},
    {"lang.list_store_b", "sg_list_store_b", "pib", "pibppi", 'v', true},
    {"lang.list_store_s", "sg_list_store_s", "pip", "pipppi", 'v', true},
    {"lang.str_replace", "sg_str_replace", "ppp", "pppppi", 'p', true},
    {"lang.str_find", "sg_str_find", "pp", "pp", 'i', false},
    {"lang.str_slice", "sg_str_slice", "pii", "piippi", 'p', true},
    {"lang.str_index", "sg_str_index", "pp", "ppppi", 'i', true},
    {"lang.str_startswith", "sg_str_startswith", "pp", "pp", 'I', false},
    {"lang.str_endswith", "sg_str_endswith", "pp", "pp", 'I', false},
    {"lang.list_index", "sg_list_index", "pIi", "pIippi", 'i', true},
    {"lang.list_count", "sg_list_count", "pIi", "pIi", 'i', false},
    {"lang.list_format", "sg_list_format", "p", "p", 'p', false},
    // F-22：List[Tuple] repr（Dict.items() 打印闭环）——元组槽按静态
    // ktag/vtag 还原（tag 编码同 etag 0-3），与 VM stringify 逐字一致。
    {"lang.list_format_tuples", "sg_list_format_tuples", "pii", "pii",
     'p', false},
    {"lang.print_list", "sg_print_list", "p", "p", 'v', false},
    {"lang.print_dict", "sg_print_dict", "p", "p", 'v', false},
    {"lang.str_chr_at", "sg_str_chr_at", "pi", "pippi", 'p', true},
    // ---- 类型转换 / 码点 / List 归约 / 容器视图（0.9.9）：panic 消息与
    // VM doBuiltin / callValueMethod 逐字一致；float 不判 errno（strtod
    // 上溢 → ±inf 与 Python 同语义），int 判 ERANGE ----
    {"lang.str_to_i64", "sg_str_to_i64", "p", "pppi", 'i', true},
    {"lang.str_to_f64", "sg_str_to_f64", "p", "pppi", 'f', true},
    {"lang.int_from_f64", "sg_int_from_f64", "f", "fppi", 'i', true},
    {"lang.ord", "sg_ord", "p", "pppi", 'i', true},
    {"lang.chr", "sg_chr", "i", "ippi", 'p', true},
    {"lang.list_reduce_i", "sg_list_reduce_i", "Ipp", "Ippppi", 'i', true},
    {"lang.list_reduce_f", "sg_list_reduce_f", "Ipp", "Ippppi", 'f', true},
    {"lang.str_count", "sg_str_count", "pp", "pp", 'i', false},
    {"lang.dict_format", "sg_dict_format", "p", "p", 'p', false},
    {"lang.set_format", "sg_set_format", "p", "p", 'p', false},
    {"lang.dict_keys", "sg_dict_keys", "p", "pppi", 'p', true},
    {"lang.dict_values", "sg_dict_values", "p", "pppi", 'p', true},
    {"lang.dict_items", "sg_dict_items", "p", "pppi", 'p', true},
    {"lang.dict_get_or", "sg_dict_get_or", "pii", "pii", 'i', false},
    {"lang.set_add", "sg_set_add", "pi", "pippi", 'v', true},
    // ---- 切片 s[a:b]（0.9.9）：ABI 5 参 [base,a,b,hasA,hasB]，归一公式
    // 与 VM Slice2 逐字一致；纯函数无 panic 上下文 ----
    {"lang.str_nslice", "sg_str_nslice", "piiii", "piiii", 'p', false},
    {"lang.list_nslice", "sg_list_nslice", "piiii", "piiii", 'p', false},
    // ---- fmt 格式化（0.9.9）：f-string :spec 降糖，四层按值类型分派；
    // 纯函数无 panic 上下文，'b' 参数 C 侧 int（sg_str_from_bool 先例） ----
    {"lang.fmt_i64", "sg_format_i64", "ip", "ip", 'p', false},
    {"lang.fmt_f64", "sg_format_f64", "fp", "fp", 'p', false},
    {"lang.fmt_str", "sg_format_str", "pp", "pp", 'p', false},
    {"lang.fmt_bool", "sg_format_bool", "bp", "bp", 'p', false},
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
    {"lang.tensor_median", "sg_tensor_median", "p", "pppi", 'f', true},
    {"lang.list_mean", "sg_list_mean", "p", "pppi", 'f', true},
    {"lang.list_var", "sg_list_var", "p", "pppi", 'f', true},
    {"lang.list_std", "sg_list_std", "p", "pppi", 'f', true},
    {"lang.list_median", "sg_list_median", "p", "pppi", 'f', true},
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

} // namespace

llvm::ArrayRef<OpLower> langLowerings() { return kLowerings; }

const OpLower* findLowering(llvm::StringRef name) {
    for (const OpLower& l : kLowerings)
        if (name == l.lang) return &l;
    return nullptr;
}

} // namespace aot
} // namespace scourge
