#!/usr/bin/env bash
# verify_o_wide.sh - O 段 FFI 宽类型域双模字节一致性回归。
# 移植自 verify_o_wide.ps1：正例 10 个 variadic @extern / data_ptr 探针走
# lib.sh dual；Sema 负例双通道 exit 1 且 stderr 命中错误码，走 lib.sh reject。
. "$(dirname "$0")/lib.sh"
suite o_wide

PB="$REPO_ROOT/verify/probes"

# ---- 正例：run vs build+aot 三比（字节一致）----
# 正例全部经 extern_call 走 kernel32/msvcrt，POSIX 宿主 VM 桩拒绝、AOT dlopen
# 失败，双模输出天然不对称——正例仅 Windows 宿主跑；Sema 负例与平台无关照跑。
case "$(uname -s)" in
    MSYS*|MINGW*|CYGWIN*)
        for n in probe_o_e probe_o_a2 probe_o_a3 probe_o_a \
                 probe_o_smoke probe_o_b probe_o_c probe_o_d \
                 probe_o_min probe_o_max20; do
            dual "$n" "$PB/$n.sg"
        done ;;
    *) echo "o_wide dual probes SKIP (Windows-only extern_call); Sema rejects still run" ;;
esac

# ---- Sema 负例：双通道 exit 1，stderr 命中错误码（lib.sh reject 语义与
#      PS1 完全一致：run/build 退出码须为 1 且错误码出现在两侧 stderr）----
reject probe_o_neg_var  "$PB/probe_o_neg_var.sg"  E1007
reject probe_o_neg_list "$PB/probe_o_neg_list.sg" E2001
reject probe_o_neg21    "$PB/probe_o_neg21.sg"    E2001

suite_done
