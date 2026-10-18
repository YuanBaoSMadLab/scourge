#!/usr/bin/env bash
# verify_jk.sh - J 段（错误处理）/ K 段（@pure 纯度强制）探针双模一致性回归。
# 移植自 verify_jk.ps1：正例 probe_j_* + probe_k_pure_ok 本地三比（含对称
# panic 探针）；负例 probe_k_pure_* 须被 check 拒绝且诊断命中 E1007。
. "$(dirname "$0")/lib.sh"
suite jk

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

# ---- Part 1：可运行探针，双模字节比对 ----
for scg in "$CB"/probe_j_*.sg; do
    [ -e "$scg" ] || continue
    dual_ps1 "$(basename "$scg" .sg)" "$scg" 1
done
dual_ps1 probe_k_pure_ok "$CB/probe_k_pure_ok.sg" 1

# ---- Part 2：负例必须被 check 拒绝且诊断含 E1007 ----
check_fail_e1007() {
    local stem="$1" scg="$2" rc txt
    "$SCOURGE" check "$scg" >"$WORK/$stem.chk.out" 2>"$WORK/$stem.chk.err"; rc=$?
    txt="$(cat "$WORK/$stem.chk.out" "$WORK/$stem.chk.err")"
    if [ "$rc" -eq 0 ]; then
        echo "$stem EXPECTED-FAIL-BUT-PASSED"; _FAIL=$((_FAIL+1))
    elif ! grep -q 'E1007' <<<"$txt"; then
        echo "$stem WRONG-CODE (expected E1007)"; _FAIL=$((_FAIL+1))
    else
        echo "$stem REJECTED (E1007, exit $rc)"
    fi
}
check_fail_e1007 probe_k_pure_print "$CB/probe_k_pure_print.sg"
check_fail_e1007 probe_k_pure_call  "$CB/probe_k_pure_call.sg"
check_fail_e1007 probe_k_pure_net   "$CB/probe_k_pure_net.sg"
check_fail_e1007 probe_k_unknown    "$CB/probe_k_unknown.sg"

suite_done
