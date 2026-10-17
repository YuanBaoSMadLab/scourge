#!/usr/bin/env bash
# verify_json.sh - JSON（json_decode + Json 值方法）探针双模字节一致性回归。
# 移植自 verify_json.ps1：正例清单含多个 json panic 探针，故用本地三比
# （不预检 run 退出码）；负例 neg_json_type 须被 check 静态拒绝。
. "$(dirname "$0")/lib.sh"
suite json

# ---- 本地三比：逐字移植 PS1 的 run/build/exec 三通道判定 ----
# 与 lib.sh dual 的差异：PS1 对 run 非零退出不早退（正例清单含对称 panic
# 探针）；本套件 PS1 无 elapsed_ms 过滤，恒为原始字节比。
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
    r1="$(cat "$WORK/$stem.run.out")"; r3="$(cat "$WORK/$stem.aot.out")"
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

# ---- Part 1：可运行探针，双模字节比对（无 elapsed_ms 过滤，逐字保留 PS1 语义）----
for sg in "$CB"/probe_json_*.sg; do
    [ -e "$sg" ] || continue
    dual_ps1 "$(basename "$sg" .sg)" "$sg" 0
done

# ---- Part 2：负例必须被 check 拒绝（退出码非零）----
check_fail() {
    local stem="$1" sg="$2" rc
    "$SCOURGE" check "$sg" >"$WORK/$stem.chk.out" 2>"$WORK/$stem.chk.err"; rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "$stem EXPECTED-FAIL-BUT-PASSED"; _FAIL=$((_FAIL+1))
    else
        echo "$stem REJECTED (exit $rc)"
    fi
}
check_fail neg_json_type "$CB/neg_json_type.sg"

suite_done
