#!/usr/bin/env bash
# verify_str_grow.sh - str 自拼接就地增长（str_concat_grow）回归 + 性能报告。
# 移植自 verify_str_grow.ps1：+= 循环、s = s + b 循环、本地别名、自追加
# （memmove 重叠）、别名标记通道、if-else 双发射点、RHS panic 不污染 lhs。
# 三比（VM run vs AOT exe）字节一致；bench 报告 VM/AOT 墙钟时间（仅报告）。
. "$(dirname "$0")/../lib.sh"
suite strgrow

PB="$REPO_ROOT/verify/probes"

# ---- 语义：run vs build+aot 三比（字节一致；正例 exit 0，直接用 lib.sh dual）----
dual probe_grow_basic "$PB/probe_grow_basic.sg"
dual probe_grow_alias "$PB/probe_grow_alias.sg"
dual probe_grow_flow  "$PB/probe_grow_flow.sg"
dual probe_grow_loop  "$PB/probe_grow_loop.sg"

# ---- 性能：VM run vs AOT exe 墙钟（仅报告，不断言；探针内联写出）----
cat > "$PB/probe_grow_bench.sg" <<'EOF'
def main() -> int:
    s = ""
    for r in range(20):
        s = ""
        for i in range(40000):
            s += "x"
    print(len(s))
    return 0
EOF
clean bench
case "$(uname -s)" in
    MSYS*|MINGW*|CYGWIN*) benchext=.exe ;;
    *)                    benchext=.bin ;;
esac
"$SCOURGE" build "$PB/probe_grow_bench.sg" -o "$WORK/bench$benchext" >"$WORK/bench.build.out" 2>"$WORK/bench.build.err"
if [ $? -ne 0 ]; then
    echo "bench BUILD-FAIL"; head -10 "$WORK/bench.build.err"; _FAIL=$((_FAIL+1))
else
    # 20 x 40000 次累加（=80 万次 append），让循环耗时压过 ~13ms 进程启动地板
    tv0=$(date +%s%3N)
    o1="$("$SCOURGE" run "$PB/probe_grow_bench.sg" 2>&1)"
    tv=$(( $(date +%s%3N) - tv0 ))
    ta0=$(date +%s%3N)
    o3="$("$WORK/bench$benchext" 2>&1)"
    ta=$(( $(date +%s%3N) - ta0 ))
    if [ "$o1" = "$o3" ]; then
        ratio="$(awk "BEGIN{printf \"%.2f\", $tv/($ta>1?$ta:1)}")"
        echo "bench IDENTICAL  vm=${tv}ms aot=${ta}ms  vm/aot=x$ratio"
    else
        echo "bench OUTPUT-DIFF vm=[$o1] aot=[$o3]"
        _FAIL=$((_FAIL+1))
    fi
fi

suite_done
