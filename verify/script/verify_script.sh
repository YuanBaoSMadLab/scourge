#!/usr/bin/env bash
# verify_script.sh - 脚本模式双模回归（顶层可执行语句 → 合成 main）。
# 锁定：单行脚本、def 与顶层语句混排（选择性搬移，def 留模块层）、
# 顶层 return 退出码（7）与裸 return（0）——双模字节一致 + 退出码一致；
# 负例：脚本模式与 def main 互斥（E2007）、import 脚本模块拒绝（E1007）。
. "$(dirname "$0")/../lib.sh"
suite script

D="$(dirname "$0")"

# ---- 正例：run vs build+aot 三比（dual 校验退出码一致性，含非零退出码）----
dual probe_script_hello  "$D/probe_script_hello.sg"
dual probe_script_full   "$D/probe_script_full.sg"
dual probe_script_return "$D/probe_script_return.sg"
dual probe_script_bare   "$D/probe_script_bare.sg"

# ---- 负例：双通道静态拒绝 ----
reject neg_script_main "$D/neg_script_main.sg" E2007
reject negimp_main     "$D/negimp/main.sg"     E1007

suite_done
