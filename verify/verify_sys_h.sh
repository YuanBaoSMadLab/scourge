#!/usr/bin/env bash
# verify_sys_h.sh - sys 内建（0.9.4 H）双模字节一致性回归。
# 移植自 verify_sys_h.ps1：probe_h_sys 需固定脚本参数 [x, --flag, y] 与环境
# 变量 PROBE_H_VAR 保证 args()/env() 确定性；time 用固定历元输入。
. "$(dirname "$0")/lib.sh"
suite sys_h

case "$(uname -s)" in
    MSYS*|MINGW*|CYGWIN*) SRCEXT=.exe ;;
    *)                    SRCEXT=.bin ;;
esac

export PROBE_H_VAR=probe-value-123

scg="$REPO_ROOT/verify/probes/probe_h_sys.sg"
stem="probe_h_sys"
clean "$stem"

# VM run 携带脚本参数；lib.sh _vm 不支持参数，此处按 PS1 直调（低层特判）
"$SCOURGE" run "$scg" x --flag y >"$WORK/$stem.run.out" 2>"$WORK/$stem.run.err"; rc1=$?
"$SCOURGE" build "$scg" -o "$WORK/$stem$SRCEXT" >"$WORK/$stem.build.out" 2>"$WORK/$stem.build.err"; rc2=$?
if [ "$rc2" -ne 0 ]; then
    echo "$stem BUILD-FAIL"; cat "$WORK/$stem.build.err"; exit 1
fi
"$WORK/$stem$SRCEXT" x --flag y >"$WORK/$stem.aot.out" 2>"$WORK/$stem.aot.err"; rc3=$?

if [ "$(cat "$WORK/$stem.run.out")" = "$(cat "$WORK/$stem.aot.out")" ] && \
   cmp -s "$WORK/$stem.run.err" "$WORK/$stem.aot.err" && [ "$rc1" -eq "$rc3" ]; then
    echo "$stem IDENTICAL (exit $rc1)"
else
    echo "$stem DIFFER exit=$rc1/$rc3"
    echo "--- run:"; cat "$WORK/$stem.run.out"
    echo "--- aot:"; cat "$WORK/$stem.aot.out"
    echo "--- run err:"; head -3 "$WORK/$stem.run.err"
    echo "--- aot err:"; head -3 "$WORK/$stem.aot.err"
    _FAIL=$((_FAIL+1))
fi

suite_done
