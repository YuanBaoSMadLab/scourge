#!/usr/bin/env bash
# verify_multi.sh - 参数化双模字节一致验证（try/result/list 探针族）。
# 移植自 verify_multi.ps1：探针源在 verify/ 根目录（git 跟踪），产物落
# build-rel/verify（git 忽略）；清单含 p_try_uncaught（对称 panic 探针），
# 故用本地三比（不预检 run 退出码，原始哈希比，无 elapsed_ms 过滤）。
. "$(dirname "$0")/lib.sh"
suite multi

case "$(uname -s)" in
    MSYS*|MINGW*|CYGWIN*) SRCEXT=.exe ;;
    *)                    SRCEXT=.bin ;;
esac
dual_ps1() {
    local stem="$1" src="$2" rc1 rc2 rc3
    clean "$stem"
    "$SCOURGE" run "$src" >"$WORK/$stem.run.out" 2>"$WORK/$stem.run.err"; rc1=$?
    "$SCOURGE" build "$src" -o "$WORK/$stem$SRCEXT" >"$WORK/$stem.build.out" 2>"$WORK/$stem.build.err"; rc2=$?
    if [ "$rc2" -ne 0 ]; then
        echo "$stem BUILD-FAIL (run exit $rc1)"; head -20 "$WORK/$stem.build.err"; _FAIL=$((_FAIL+1)); return
    fi
    "$WORK/$stem$SRCEXT" >"$WORK/$stem.aot.out" 2>"$WORK/$stem.aot.err"; rc3=$?
    if cmp -s "$WORK/$stem.run.out" "$WORK/$stem.aot.out" && \
       cmp -s "$WORK/$stem.run.err" "$WORK/$stem.aot.err" && [ "$rc1" -eq "$rc3" ]; then
        echo "$stem IDENTICAL (exit $rc1)"
    else
        echo "$stem DIFFER exit=$rc1/$rc3"
        echo "--- run stdout:"; cat "$WORK/$stem.run.out"
        echo "--- aot stdout:"; cat "$WORK/$stem.aot.out"
        echo "--- run stderr:"; cat "$WORK/$stem.run.err"
        echo "--- aot stderr:"; cat "$WORK/$stem.aot.err"
        _FAIL=$((_FAIL+1))
    fi
}

SRC="$REPO_ROOT/verify"
for f in p_try_min.sg p_try_cross.sg p_try_nested.sg p_try_uncaught.sg p_result.sg p_list.sg; do
    dual_ps1 "$(basename "$f" .sg)" "$SRC/$f"
done

suite_done
