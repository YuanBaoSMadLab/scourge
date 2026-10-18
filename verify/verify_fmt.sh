#!/usr/bin/env bash
# verify_fmt.sh - fmt 探针（f-string 插值 + format() 脱糖）双模字节一致性回归。
# 移植自 verify_fmt.ps1：正例本地三比（保留 PS1 的 elapsed_ms 过滤特判）；
# 四个负例 probe_fmt_neg_* 须被 check 静态拒绝（parse/Sema 拒绝）。
. "$(dirname "$0")/lib.sh"
suite fmt

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
for scg in "$CB"/probe_fmt_acc_*.sg; do
    [ -e "$scg" ] || continue
    dual_ps1 "$(basename "$scg" .sg)" "$scg" 1
done

# ---- Part 2：负例必须被 check 拒绝（退出码非零）----
check_fail() {
    local stem="$1" scg="$2" rc
    "$SCOURGE" check "$scg" >"$WORK/$stem.chk.out" 2>"$WORK/$stem.chk.err"; rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "$stem EXPECTED-FAIL-BUT-PASSED"; _FAIL=$((_FAIL+1))
    else
        echo "$stem REJECTED (exit $rc)"
    fi
}
check_fail probe_fmt_neg_spec   "$CB/probe_fmt_neg_spec.sg"
check_fail probe_fmt_neg_fmtlit "$CB/probe_fmt_neg_fmtlit.sg"
check_fail probe_fmt_neg_count  "$CB/probe_fmt_neg_count.sg"
check_fail probe_fmt_neg_empty  "$CB/probe_fmt_neg_empty.sg"

suite_done
