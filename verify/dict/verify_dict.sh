#!/usr/bin/env bash
# verify_dict.sh - Dict 运行时双模字节一致性回归（0.9.7，缺陷 Q5）。
# 移植自 verify_dict.ps1：survey 三比 + 缺键 panic stderr 字节一致 + 容器值
# Dict 不对称契约（VM 打印 / AOT 编译期 E1007）。探针 .sg 由本脚本内联写出。
. "$(dirname "$0")/../lib.sh"
suite dict

PB="$REPO_ROOT/verify/probes"

case "$(uname -s)" in
    MSYS*|MINGW*|CYGWIN*) SRCEXT=.exe ;;
    *)                    SRCEXT=.bin ;;
esac

# ---- 正例：survey 三比（字节一致；正例 exit 0，直接用 lib.sh dual）----
dual probe_dict_survey "$PB/probe_dict_survey.sg"

# ---- panic：缺键 stderr 必须字节一致（lib.sh panic 语义与 PS1 一致：
#      双模退出码相等且非零、stderr 全等、以 panic: 开头）----
cat > "$PB/probe_dict_panic.sg" <<'EOF'
def main() -> int:
    d: Dict[str, int] = {"a": 1}
    print(d["zz"])
    return 0
EOF
panic probe_dict_panic "$PB/probe_dict_panic.sg"

# ---- 不对称契约：容器值 Dict 打印 ----
# VM 打印它；AOT 须在编译期以 E1007 拒绝（非双模违约：E1007 是既定的显式
# 拒绝通道）。lib.sh 无此组合判定，按 PS1 低层特判实现。
cat > "$PB/probe_dict_cval.sg" <<'EOF'
def main() -> int:
    x: List[int] = [1, 2]
    d: Dict[str, List[int]] = {"a": x}
    print(d)
    return 0
EOF
clean probe_dict_cval
"$SCOURGE" run "$PB/probe_dict_cval.sg" >"$WORK/probe_dict_cval.run.out" 2>"$WORK/probe_dict_cval.run.err"; rc1=$?
"$SCOURGE" build "$PB/probe_dict_cval.sg" -o "$WORK/probe_dict_cval$SRCEXT" >"$WORK/probe_dict_cval.build.out" 2>"$WORK/probe_dict_cval.build.err"; rc2=$?
vmout="$(cat "$WORK/probe_dict_cval.run.out")"
if [ "$rc1" -eq 0 ] && [ "$vmout" = '{"a": [1, 2]}' ] && \
   [ "$rc2" -eq 1 ] && grep -q 'E1007' "$WORK/probe_dict_cval.build.err"; then
    echo "probe_dict_cval ASYMMETRY-OK (vm prints, aot E1007)"
else
    echo "probe_dict_cval MISMATCH vm=$rc1 build=$rc2"
    echo "--- vm out:"; cat "$WORK/probe_dict_cval.run.out"
    echo "--- build err:"; head -20 "$WORK/probe_dict_cval.build.err"
    _FAIL=$((_FAIL+1))
fi

suite_done
