#!/usr/bin/env bash
# verify_reg.sh - train_linreg / attention_softmax 双模字节一致性回归 +
# ovf_* 标量溢出探针（AOT inline-lowered with.overflow 须与 VM 字节一致地
# panic，含 add/sub/mul/neg + try-catch）。移植自 verify_reg.ps1。
. "$(dirname "$0")/lib.sh"
suite reg

# ---- 本地三比：逐字移植 PS1 的 run/build/exec 三通道判定 ----
# 与 lib.sh dual 的差异：PS1 对 run 非零退出不早退（ovf 清单含对称 panic
# 探针）；stdout 比对前滤除 elapsed_ms 行（PS1 的 -notmatch 特判）。
case "$(uname -s)" in
    MSYS*|MINGW*|CYGWIN*) SRCEXT=.exe ;;
    *)                    SRCEXT=.bin ;;
esac
dual_ps1() {
    local stem="$1" src="$2" rc1 rc2 rc3 r1 r3
    clean "$stem"
    "$SCOURGE" run "$src" >"$WORK/$stem.run.out" 2>"$WORK/$stem.run.err"; rc1=$?
    "$SCOURGE" build "$src" -o "$WORK/$stem$SRCEXT" >"$WORK/$stem.build.out" 2>"$WORK/$stem.build.err"; rc2=$?
    if [ "$rc2" -ne 0 ]; then
        echo "$stem BUILD-FAIL"; head -20 "$WORK/$stem.build.err"; _FAIL=$((_FAIL+1)); return
    fi
    "$WORK/$stem$SRCEXT" >"$WORK/$stem.aot.out" 2>"$WORK/$stem.aot.err"; rc3=$?
    r1="$(grep -v 'elapsed_ms' "$WORK/$stem.run.out")"
    r3="$(grep -v 'elapsed_ms' "$WORK/$stem.aot.out")"
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

for scg in "$REPO_ROOT/release/examples/train_linreg.sg" \
          "$REPO_ROOT/release/examples/attention_softmax.sg" \
          "$REPO_ROOT"/verify/ovf_*.sg \
          "$REPO_ROOT"/verify/dvz_*.sg; do
    [ -e "$scg" ] || continue
    dual_ps1 "$(basename "$scg" .sg)" "$scg"
done

suite_done
