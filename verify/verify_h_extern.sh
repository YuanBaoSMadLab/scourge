#!/usr/bin/env bash
# verify_h_extern.sh - @extern dll 调用（0.9.4 H）双模字节一致性回归。
# 移植自 verify_h_extern.ps1：probe_h_extern（正例 kernel32/msvcrt）与
# probe_h_extern_panic（负例缺 dll，双模对称 panic）——故用本地三比。
. "$(dirname "$0")/lib.sh"
suite h_extern

# 正例与负例探针均显式调用 kernel32/msvcrt，POSIX 宿主 VM 桩拒绝、AOT dlopen
# 失败——双模 panic 消息无对称性可言，本套件语义仅成立于 Windows 宿主。
case "$(uname -s)" in
    MSYS*|MINGW*|CYGWIN*) : ;;
    *)
        echo "h_extern SKIP (Windows-only: kernel32/msvcrt extern_call)"
        exit 0 ;;
esac

# ---- 本地三比：逐字移植 PS1 的 run/build/exec 三通道判定 ----
# 与 lib.sh dual 的差异：PS1 对 run 非零退出不早退（probe_h_extern_panic
# 双模对称 panic 属正判）；本套件 PS1 无 elapsed_ms 过滤。
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
        echo "$stem BUILD-FAIL"; head -20 "$WORK/$stem.build.err"; exit 1
    fi
    "$WORK/$stem$SRCEXT" >"$WORK/$stem.aot.out" 2>"$WORK/$stem.aot.err"; rc3=$?
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
}

PB="$REPO_ROOT/verify/probes"

dual_ps1 probe_h_extern        "$PB/probe_h_extern.sg"
dual_ps1 probe_h_extern_panic  "$PB/probe_h_extern_panic.sg"

suite_done
