#!/usr/bin/env bash
# verify_str_list.sh - str / List 成员方法（0.9.4）探针双模字节一致性回归。
# 移植自 verify_str_list.ps1：probe_c_acc_* / probe_d_acc_* 清单含对称 panic
# 探针，故用本地三比（不预检 run 退出码），保留 elapsed_ms 过滤特判。
. "$(dirname "$0")/lib.sh"
suite str_list

# ---- 本地三比：逐字移植 PS1 的 run/build/exec 三通道判定 ----
# 与 lib.sh dual 的差异：PS1 对 run 非零退出不早退；filter=1 时 stdout 比对前
# 滤除 elapsed_ms 行（PS1 的 -notmatch 'elapsed_ms' 特判）。
case "$(uname -s)" in
    MSYS*|MINGW*|CYGWIN*) SRCEXT=.exe ;;
    *)                    SRCEXT=.bin ;;
esac
dual_ps1() {
    local stem="$1" src="$2" filt="$3" rc1 rc2 rc3 r1 r3
    clean "$stem"
    "$SCOURGE" run "$src" >"$WORK/$stem.run.out" 2>"$WORK/$stem.run.err"; rc1=$?
    "$SCOURGE" build "$src" -o "$WORK/$stem$SRCEXT" >"$WORK/$stem.build.out" 2>"$WORK/$stem.build.err"; rc2=$?
    if [ "$rc2" -ne 0 ]; then
        echo "$stem BUILD-FAIL"; head -20 "$WORK/$stem.build.err"; _FAIL=$((_FAIL+1)); return
    fi
    "$WORK/$stem$SRCEXT" >"$WORK/$stem.aot.out" 2>"$WORK/$stem.aot.err"; rc3=$?
    if [ "$filt" -eq 1 ]; then
        r1="$(grep -v 'elapsed_ms' "$WORK/$stem.run.out")"
        r3="$(grep -v 'elapsed_ms' "$WORK/$stem.aot.out")"
    else
        r1="$(cat "$WORK/$stem.run.out")"; r3="$(cat "$WORK/$stem.aot.out")"
    fi
    if [ "$r1" = "$r3" ] && cmp -s "$WORK/$stem.run.err" "$WORK/$stem.aot.err" && [ "$rc1" -eq "$rc3" ]; then
        echo "$stem IDENTICAL (exit $rc1)"
    else
        echo "$stem DIFFER exit=$rc1/$rc3"
        echo "--- run:"; head -5 "$WORK/$stem.run.out"
        echo "--- aot:"; head -5 "$WORK/$stem.aot.out"
        echo "--- run err:"; head -3 "$WORK/$stem.run.err"
        echo "--- aot err:"; head -3 "$WORK/$stem.aot.err"
        _FAIL=$((_FAIL+1))
    fi
}

CB="$REPO_ROOT/verify/probes"

for scg in "$CB"/probe_c_acc_*.sg "$CB"/probe_d_acc_*.sg; do
    [ -e "$scg" ] || continue
    dual_ps1 "$(basename "$scg" .sg)" "$scg" 1
done

suite_done
