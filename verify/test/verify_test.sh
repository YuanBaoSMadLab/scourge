#!/usr/bin/env bash
# verify_test.sh - 测试框架验证（sg test，属性测试 + .sgt 运行器）。
# 移植自 verify_test.ps1：mathlib 探针刻意不完美（证明 oracle 有效）——
# 显式用例 pass/fail/panic，属性 pass/fail（fact 反例 n=0）/skip，
# 总退出码 1；ok.sg（全对契约）须退出 0。ok.sg 由本脚本内联写出。
. "$(dirname "$0")/../lib.sh"
suite test

SRC="$REPO_ROOT/verify/test"

# 工作目录整体重建并拷贝探针资产（PS1：Test-Path -> Remove-Item -> Copy）
rm -rf "$WORK"
mkdir -p "$WORK"
cp "$SRC/mathlib.sg" "$WORK/"
cp "$SRC/mathlib.sgt" "$WORK/"

# ok.sg：契约全对 => 整个命令退出 0
cat > "$WORK/ok.sg" <<'EOF'
# ok.sg - all-pass property probe (positive exit code check).

@pure
def ident(x: int) -> int:
    """
    identity function probe.
    requires: x >= -1000000 and x <= 1000000
    ensures: result == x
    """
    return x
EOF

# Run-Test 移植：sg test <args...>，stdout/stderr 落盘后读回
run_test() {  # $1=tag，其余为 test 子命令参数
    local tag="$1"; shift
    "$SCOURGE" test "$@" >"$WORK/$tag.out" 2>"$WORK/$tag.err"
    RC=$?
    OUT="$(cat "$WORK/$tag.out")"
    ERR="$(cat "$WORK/$tag.err")"
}

hash_of() { sha256sum "$1" | cut -d' ' -f1; }

# Check 移植：PASS/FAIL 逐项打印
check() {  # $1=条件(0/1) $2=名称
    if [ "$1" -eq 1 ]; then
        echo "PASS $2"
    else
        _FAIL=$((_FAIL+1))
        echo "FAIL $2"
    fi
}

# 属性对象窗口：自 "func": "X" 起至下一个 "func" 键止（含 key 本身）
json_window() {  # $1=json $2=key $3=终止键
    local j="$1" key="$2" term="$3"
    case "$j" in
        *"$key"*) j="${j#*"$key"}" ;;
        *) return ;;
    esac
    case "$j" in
        *"$term"*) j="${j%%"$term"*}" ;;
    esac
    printf '%s' "$key$j"
}

# 子串包含判定：命中回显 1，否则 0
has() {  # $1=文本 $2=子串
    case "$1" in
        *"$2"*) echo 1 ;;
        *) echo 0 ;;
    esac
}

# ---- T1：完整运行（--json --runs 32）=> exit 1，状态符合期望 ----
run_test t1 --json --runs 32 "$WORK/mathlib.sg"
c=0; [ "$RC" -eq 1 ] && c=1
check "$c" "T1 exit code 1 (got $RC)"
if [ "$RC" -ne 0 ] && [ -n "$ERR" ]; then
    echo "  stderr: $ERR"
fi
json="$OUT"
check "$(has "$json" '"module": "mathlib"')" 'T1 module field'
check "$(has "$json" '"seed": null')" 'T1 default seed is null (derived)'

fw="$(json_window "$json" '"func": "add"' '"func":')"
check "$(has "$fw" '"status": "pass"')" 'T1 prop add pass'
fw="$(json_window "$json" '"func": "div"' '"func":')"
check "$(has "$fw" '"status": "pass"')" 'T1 prop div pass'
fw="$(json_window "$json" '"func": "capped"' '"func":')"
check "$(has "$fw" '"status": "pass"')" 'T1 prop capped pass'
fw="$(json_window "$json" '"func": "scaled"' '"func":')"
check "$(has "$fw" '"status": "pass"')" 'T1 prop scaled pass'

fw="$(json_window "$json" '"func": "fact"' '"func":')"
check "$(has "$fw" '"status": "fail"')" 'T1 prop fact fail'
check "$(has "$fw" '"counterexample"')" 'T1 fact reports counterexample'
check "$(has "$fw" '"args"')" 'T1 fact counterexample has args'
check "$(has "$fw" '"0"')" 'T1 fact counterexample first arg is 0 (boundary pool)'
check "$(has "$fw" '"reason"')" 'T1 fact counterexample has reason'

lw="$(json_window "$json" '"func": "lookup"' '"func":')"
check "$(has "$lw" '"status": "skip"')" 'T1 prop lookup skip'
check "$(has "$lw" '"skipReason"')" 'T1 lookup skipReason present'

tw="$(json_window "$json" '"name": "test_add_basic"' '"name":')"
check "$(has "$tw" '"status": "pass"')" 'T1 test_add_basic pass'
tw="$(json_window "$json" '"name": "test_add_wrong"' '"name":')"
check "$(has "$tw" '"status": "fail"')" 'T1 test_add_wrong fail'
tw="$(json_window "$json" '"name": "test_oob_panic"' '"name":')"
check "$(has "$tw" '"status": "panic"')" 'T1 test_oob_panic panic'

check "$(has "$json" '"tests": 3')" 'T1 summary tests=3'
check "$(has "$json" '"testFailures": 2')" 'T1 summary testFailures=2'
check "$(has "$json" '"properties": 6')" 'T1 summary properties=6'
check "$(has "$json" '"propertyFailures": 1')" 'T1 summary propertyFailures=1'
check "$(has "$json" '"ok": false')" 'T1 summary ok=false'

# ---- T2：确定性 —— 同参数两次 => JSON 字节一致 ----
run_test t2 --json --runs 32 "$WORK/mathlib.sg"
c=0; [ "$(hash_of "$WORK/t1.out")" = "$(hash_of "$WORK/t2.out")" ] && c=1
check "$c" 'T2 deterministic rerun (same bytes)'

# ---- T3：--runs / --seed 生效且保持确定性 ----
run_test t3a --json --seed 7 --runs 4 "$WORK/mathlib.sg"
c=0; [ "$RC" -eq 1 ] && c=1
check "$c" "T3 exit code 1 (got $RC)"
check "$(has "$OUT" '"seed": 7')" 'T3 explicit seed echoed'
check "$(has "$OUT" '"runs": 4,')" 'T3 runs=4 in property results'
run_test t3b --json --seed 7 --runs 4 "$WORK/mathlib.sg"
c=0; [ "$(hash_of "$WORK/t3a.out")" = "$(hash_of "$WORK/t3b.out")" ] && c=1
check "$c" 'T3 explicit-seed rerun identical'

# ---- T4：无 .sgt 旁车 => tests 数组为空，属性仍运行 ----
mkdir -p "$WORK/nosgt"
cp "$WORK/mathlib.sg" "$WORK/nosgt/"
run_test t4 --json --runs 8 "$WORK/nosgt/mathlib.sg"
c=0; [ "$RC" -eq 1 ] && c=1
check "$c" "T4 exit code 1 (got $RC)"
# stdout 为文本模式（LF -> CRLF 可能混入）；剥 CR 后再匹配换行敏感片段
json4="${OUT//$'\r'/}"
empty_tests=$'"tests": [\n  ]'
check "$(has "$json4" "$empty_tests")" 'T4 empty tests array'
check "$(has "$OUT" '"testFailures": 0')" 'T4 testFailures=0'
fw="$(json_window "$OUT" '"func": "fact"' '"func":')"
check "$(has "$fw" '"status": "fail"')" 'T4 properties still verified'

# ---- T5：全对模块（ok.sg）=> exit 0 ----
run_test t5 --json "$WORK/ok.sg"
c=0; [ "$RC" -eq 0 ] && c=1
check "$c" "T5 exit code 0 (got $RC)"
check "$(has "$OUT" '"ok": true')" 'T5 ok=true'

# ---- T6：用法错误 => exit 2 ----
run_test t6 --runs 0 "$WORK/mathlib.sg"
c=0; [ "$RC" -eq 2 ] && c=1
check "$c" "T6 --runs 0 rejected with exit 2 (got $RC)"

# ---- T7：文本模式冒烟（人类可读报告）----
run_test t7 "$WORK/mathlib.sg"
c=0; [ "$RC" -eq 1 ] && c=1
check "$c" "T7 text mode exit 1 (got $RC)"

suite_done
