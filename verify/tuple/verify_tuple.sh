#!/usr/bin/env bash
# verify_tuple.sh - Tuple（0.9.6）双模字节一致性回归。
# 移植自 verify_tuple.ps1：probe_tuple 单探针三比（保留 elapsed_ms 过滤特判）；
# 编译期拒绝场景以 VM 诊断为唯一门：tuple_err/tuple_err3 须 E2001，
# tuple_err2 须 E2004。负例探针自包含于本目录（PS1 原版依赖本机 build-rel
# 遗留文件，跨环境不可复现，此处改为套件内探针——单一事实源）。
. "$(dirname "$0")/../lib.sh"
suite tuple

# ---- 本地三比：逐字移植 PS1 的 run/build/exec 三通道判定 ----
# 与 lib.sh dual 的差异：PS1 对 run 非零退出不早退；stdout 比对前滤除
# elapsed_ms 行（PS1 的 -notmatch 特判）。
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
        _FAIL=$((_FAIL+1))
    fi
}

# ---- 1. 探针双模字节比对 ----
dual_ps1 tuple_probe "$REPO_ROOT/verify/tuple/probe_tuple.sg"

# ---- 2. 编译期拒绝场景（VM 诊断为唯一门；run 通道单比）----
run_reject() {
    local stem="$1" sgf="$2" code="$3" rc
    "$SCOURGE" run "$sgf" >"$WORK/$stem.out" 2>"$WORK/$stem.err"; rc=$?
    if [ "$rc" -ne 0 ] && grep -q "$code" "$WORK/$stem.err"; then
        echo "$stem REJECTED ($code, exit $rc)"
    else
        echo "$stem NOT-REJECTED (exit $rc)"
        head -4 "$WORK/$stem.err" 2>/dev/null
        _FAIL=$((_FAIL+1))
    fi
}
# 下标写：tuple 不可变
run_reject tuple_err  "$REPO_ROOT/verify/tuple/tuple_err.sg"  E2001
# == 元数不匹配
run_reject tuple_err3 "$REPO_ROOT/verify/tuple/tuple_err3.sg" E2001
# 索引越界报 E2004（消息文本不在此匹配，与 PS1 同）
run_reject tuple_err2 "$REPO_ROOT/verify/tuple/tuple_err2.sg" E2004

suite_done
