# ============================================================================
#  lib.sh - Scourge verify 公共引擎（bash 跨平台版，配 *.sh 驱动脚本）
#
#  职责边界：只提供"VM run vs AOT build+exec 双模字节一致"与"负例拒绝"
#  两类核心判定 + 产物落盘管理；套件各自的探针清单与特判留在各 .sh。
#  与 verify/*.ps1（Windows 专用家族）语义对齐：判定契约逐字节一致。
#
#  用法（驱动脚本内）：
#    . "$(dirname "$0")/../lib.sh"    # 子目录脚本
#    suite <名称>                      # 设定套件名与产物目录
#    dual|panic|reject ...             # 判定
#    suite_done                        # 输出 <名> PASS / exit 1
#
#  环境变量：
#    SCOURGE  scg 可执行路径；默认优先 PATH 里的 `scg`，回退
#             build-rel/scg（Linux）或 build-rel/scg.exe（Windows/WSL）。
#  产物约定：一律落 $REPO_ROOT/build-rel/verify/<套件名>/，严禁写回 verify/。
# ============================================================================
# 代理环境去重：小写代理变量与大写并存时丢弃小写（Python 子进程网络测试需要）
for _v in http_proxy https_proxy no_proxy; do
    _up=$(echo "$_v" | tr '[:lower:]' '[:upper:]')
    if [ -n "$(printenv "$_v")" ] && [ -n "$(printenv "$_up")" ]; then
        unset "$_v"
    fi
done
unset _v _up

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Python 解释器探测（网络对拍助手用；Linux 为 python3，Windows 侧两看）
if [ -z "${PY:-}" ]; then
    PY="$(command -v python3 || command -v python || true)"
fi

# scg 可执行探测：先 PATH 后本地
if [ -z "${SCOURGE:-}" ]; then
    _c="$(command -v scg 2>/dev/null || true)"
    if [ -z "$_c" ]; then
        for _c in "$REPO_ROOT/build-rel/scg" "$REPO_ROOT/build-rel/scg.exe"; do
            if [ -x "$_c" ]; then break; fi
        done
    fi
    if [ -n "$_c" ] && [ -x "$_c" ]; then SCOURGE="$_c"; fi
fi
if [ -z "${SCOURGE:-}" ]; then
    echo "scg 可执行未找到：请先构建（cmake --build build-rel）或把 scg 加进 PATH，或设 SCOURGE=<路径>" >&2
    exit 2
fi

_FAIL=0
_SUITE=""
WORK=""

# suite <名称>：设定套件名并准备产物目录 build-rel/verify/<名称>
suite() {
    _SUITE="$1"
    WORK="$REPO_ROOT/build-rel/verify/$_SUITE"
    mkdir -p "$WORK"
    _FAIL=0
}

# suite_done：全部通过则输出 "<套件> PASS"，否则 exit 1
suite_done() {
    if [ "$_FAIL" -eq 0 ]; then
        echo "${_SUITE} PASS"
    else
        echo "${_SUITE} FAIL ($_FAIL)" >&2
        exit 1
    fi
}

# clean <stem>：清理该探针在产物目录的遗留（.sgbuild 为 obj 缓存目录，须递归）
clean() { rm -rf "$WORK/$1".* ; }

# _vm <stem> <src>：VM run，重定向到 <stem>.run.{out,err}，退出码存 VM_RC
_vm() {
    "$SCOURGE" run "$2" >"$WORK/$1.run.out" 2>"$WORK/$1.run.err"
    VM_RC=$?
}

# _aot <stem> <src>：AOT build，产物落 <stem>.exe/.bin（按宿主平台），
# 日志 <stem>.build.{out,err}，退出码存 AOT_RC
_aot() {
    case "$(uname -s)" in
        MSYS*|MINGW*|CYGWIN*) _aotext=".exe" ;;
        *)                    _aotext=".bin" ;;
    esac
    "$SCOURGE" build "$2" -o "$WORK/$1$_aotext" >"$WORK/$1.build.out" 2>"$WORK/$1.build.err"
    AOT_RC=$?
    AOT_BIN="$WORK/$1$_aotext"
}

# _aotexec <stem>：执行 AOT 产物，日志 <stem>.aot.{out,err}，退出码存 AOTX_RC
_aotexec() {
    "$AOT_BIN" >"$WORK/$1.aot.out" 2>"$WORK/$1.aot.err"
    AOTX_RC=$?
}

# dual <stem> <src>：正例双模字节一致（stdout/stderr 逐字节 + 退出码）
dual() {
    local stem="$1" src="$2"
    clean "$stem"
    _vm "$stem" "$src"
    if [ $VM_RC -ne 0 ] && ! grep -q . "$WORK/$stem.run.err" 2>/dev/null; then
        echo "$stem RUN-ABORT (exit $VM_RC)"; cat "$WORK/$stem.run.err"; _FAIL=$((_FAIL+1)); return
    fi
    _aot "$stem" "$src"
    if [ $AOT_RC -ne 0 ]; then
        echo "$stem BUILD-FAIL"; head -20 "$WORK/$stem.build.err"; _FAIL=$((_FAIL+1)); return
    fi
    _aotexec "$stem"
    local o=0 e=0 x=0
    cmp -s "$WORK/$stem.run.out" "$WORK/$stem.aot.out" && o=1
    cmp -s "$WORK/$stem.run.err" "$WORK/$stem.aot.err" && e=1
    [ "$VM_RC" -eq "$AOTX_RC" ] && x=1
    if [ $o -eq 1 ] && [ $e -eq 1 ] && [ $x -eq 1 ]; then
        echo "$stem IDENTICAL (exit $VM_RC)"
    else
        echo "$stem DIFFER out=$o err=$e exit=$VM_RC/$AOTX_RC"
        echo "--- run stdout:"; cat "$WORK/$stem.run.out"
        echo "--- aot stdout:"; cat "$WORK/$stem.aot.out"
        echo "--- run stderr:"; cat "$WORK/$stem.run.err"
        echo "--- aot stderr:"; cat "$WORK/$stem.aot.err"
        _FAIL=$((_FAIL+1))
    fi
}

# panic <stem> <src>：panic 负例——双模 exit 1、stderr 逐字节一致且以 panic: 开头
panic() {
    local stem="$1" src="$2"
    clean "$stem"
    _vm "$stem" "$src"
    _aot "$stem" "$src"
    _aotexec "$stem"
    if head -c 6 "$WORK/$stem.run.err" | grep -q "panic:" && \
       cmp -s "$WORK/$stem.run.err" "$WORK/$stem.aot.err" && \
       [ "$VM_RC" -eq "$AOTX_RC" ] && [ "$VM_RC" -ne 0 ]; then
        echo "$stem PANIC-IDENTICAL (exit $VM_RC)"
    else
        echo "$stem PANIC-MISMATCH run=$VM_RC aot=$AOTX_RC"
        echo "--- run stderr:"; cat "$WORK/$stem.run.err"
        echo "--- aot stderr:"; cat "$WORK/$stem.aot.err"
        _FAIL=$((_FAIL+1))
    fi
}

# reject <stem> <src> <code>：静态拒绝负例——双模 exit 1 且 stderr 命中错误码
reject() {
    local stem="$1" src="$2" code="$3"
    clean "$stem"
    _vm "$stem" "$src"
    _aot "$stem" "$src"
    if [ "$VM_RC" -eq 1 ] && [ "$AOT_RC" -eq 1 ] && \
       grep -q "$code" "$WORK/$stem.run.err" && grep -q "$code" "$WORK/$stem.build.err"; then
        echo "$stem REJECTED ($code)"
    else
        echo "$stem NOT-REJECTED ($code) run=$VM_RC build=$AOT_RC"
        echo "--- run stderr:"; cat "$WORK/$stem.run.err"
        echo "--- build stderr:"; cat "$WORK/$stem.build.err"
        _FAIL=$((_FAIL+1))
    fi
}

# vm_stdout_is <stem> <src> <期望全文>：VM stdout 去尾空白后须等于期望（特判用）
vm_stdout_is() {
    local stem="$1" src="$2" want="$3"
    clean "$stem"
    _vm "$stem" "$src"
    local got
    got="$(cat "$WORK/$stem.run.out")"
    if [ "$VM_RC" -eq 0 ] && [ "$got" = "$want" ]; then
        echo "$stem VM-OUT-OK"
    else
        echo "$stem VM-OUT-MISMATCH rc=$VM_RC"
        echo "--- got:"; echo "$got"
        echo "--- want:"; echo "$want"
        _FAIL=$((_FAIL+1))
    fi
}
