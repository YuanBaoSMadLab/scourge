#!/usr/bin/env bash
# verify_cap.sh - 能力模型（0.9.7）探针回归。
# 移植自 verify_cap.ps1：probe_cap 相对 CWD 写 probe_cap_tmp.txt，两模式钉在
# 同一工作目录保证输出可比；负例 neg_cap_io 须在 run 与 build 两路被 E5004 拒绝。
. "$(dirname "$0")/lib.sh"
suite cap

case "$(uname -s)" in
    MSYS*|MINGW*|CYGWIN*) SRCEXT=.exe ;;
    *)                    SRCEXT=.bin ;;
esac

SRC="$REPO_ROOT/verify/cap"

# ---- Part 1：可运行探针，双模字节比对（CWD 钉死在产物目录 + elapsed_ms 过滤）----
# lib.sh _vm/_aot 不支持钉 CWD，此处按 PS1 直调（低层特判）。
for stem in probe_cap probe_cap_pure; do
    sg="$SRC/$stem.sg"
    clean "$stem"
    rm -f "$WORK/probe_cap_tmp.txt"
    ( cd "$WORK" && "$SCOURGE" run "$sg" ) >"$WORK/$stem.run.out" 2>"$WORK/$stem.run.err"; rc1=$?
    rm -f "$WORK/probe_cap_tmp.txt"
    ( cd "$WORK" && "$SCOURGE" build "$sg" -o "$WORK/$stem$SRCEXT" ) >"$WORK/$stem.build.out" 2>"$WORK/$stem.build.err"; rc2=$?
    if [ "$rc2" -ne 0 ]; then
        echo "$stem BUILD-FAIL"; head -20 "$WORK/$stem.build.err"; _FAIL=$((_FAIL+1)); continue
    fi
    ( cd "$WORK" && "$WORK/$stem$SRCEXT" ) >"$WORK/$stem.aot.out" 2>"$WORK/$stem.aot.err"; rc3=$?
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
    rm -f "$WORK/probe_cap_tmp.txt"
done

# ---- Part 2：负例 -> run 与 build 两路均须 E5004 静态拒绝（lib.sh reject
#      语义与 PS1 一致：双通道 exit 1 且 stderr 命中错误码）----
reject neg_cap_io "$SRC/neg_cap_io.sg" E5004

suite_done
