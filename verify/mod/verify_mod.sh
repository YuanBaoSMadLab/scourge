#!/usr/bin/env bash
# verify_mod.sh - 0.9.6 L 段（模块与包）双模验证。
# 移植自 verify_mod.ps1：点号模块 a.b -> a/b.sg；本地包缓存 <root>/<pkg>@<ver>
# 数值感知选版（1.10 > 1.9）。正例 run vs AOT build 字节一致；负例 run vs
# build 诊断行一致（E5001/E5003）；外加 interface 命令的 @private 隐藏契约。
. "$(dirname "$0")/../lib.sh"
suite mod

case "$(uname -s)" in
    MSYS*|MINGW*|CYGWIN*) SRCEXT=.exe ;;
    *)                    SRCEXT=.bin ;;
esac

SRC="$REPO_ROOT/verify/mod/probes"
PKGSRC="$REPO_ROOT/verify/mod/packages"

# 工作目录整体重建（PS1：Test-Path -> Remove-Item -Recurse）
rm -rf "$WORK"
mkdir -p "$WORK"
cp -r "$SRC"/. "$WORK"/
mkdir -p "$WORK/packages"
cp -r "$PKGSRC"/. "$WORK/packages"/
export SCOURGE_PACKAGES="$WORK/packages"

# 单模式执行：run/build/exec 全部钉在 $WORK（PS1 -WorkingDirectory）；
# exec 产物沿用 PS1 命名 <tag>.aot.out / <tag>.aot.err
run_mode() {  # $1=mode $2=file $3=tag -> 回显退出码
    local mode="$1" file="$2" tag="$3"
    if [ "$mode" = "build" ]; then
        ( cd "$WORK" && "$SCOURGE" build "$file" -o "$WORK/$tag$SRCEXT" ) \
            >"$WORK/$tag.$mode.out" 2>"$WORK/$tag.$mode.err"
    elif [ "$mode" = "run" ]; then
        ( cd "$WORK" && "$SCOURGE" run "$file" ) \
            >"$WORK/$tag.$mode.out" 2>"$WORK/$tag.$mode.err"
    else
        ( cd "$WORK" && "$WORK/$tag$SRCEXT" ) \
            >"$WORK/$tag.aot.out" 2>"$WORK/$tag.aot.err"
    fi
}

# 诊断行过滤：驱动会附加模式相关尾注，error[ 行才是编译器诊断本体
diag_of() { grep -F 'error[' "$1" 2>/dev/null; }

# Check-Dual 移植：expect_ok=1 走三比，否则走 run vs build 负例比对
check_dual() {  # $1=file $2=tag $3=expect_ok(1/0)
    local file="$1" tag="$2" ok="$3" rc_r rc_b rc_a d1 d2
    run_mode run "$file" "$tag"; rc_r=$?
    run_mode build "$file" "$tag"; rc_b=$?
    if [ "$ok" = "1" ]; then
        if [ "$rc_b" -ne 0 ]; then
            echo "$tag BUILD-FAIL (run exit $rc_r)"
            head -20 "$WORK/$tag.build.err"
            _FAIL=$((_FAIL+1))
            return
        fi
        run_mode exec "$file" "$tag"; rc_a=$?
        if cmp -s "$WORK/$tag.run.out" "$WORK/$tag.aot.out" && \
           cmp -s "$WORK/$tag.run.err" "$WORK/$tag.aot.err" && [ "$rc_r" -eq "$rc_a" ]; then
            echo "$tag IDENTICAL (exit $rc_r)"
        else
            echo "$tag DIFFER exit=$rc_r/$rc_a"
            echo "--- run stdout:"; cat "$WORK/$tag.run.out"
            echo "--- aot stdout:"; cat "$WORK/$tag.aot.out"
            echo "--- run stderr:"; cat "$WORK/$tag.run.err"
            echo "--- aot stderr:"; cat "$WORK/$tag.aot.err"
            _FAIL=$((_FAIL+1))
        fi
    else
        if [ "$rc_r" -eq 0 ] || [ "$rc_b" -eq 0 ]; then
            echo "$tag UNEXPECTED-PASS (run=$rc_r build=$rc_b)"
            _FAIL=$((_FAIL+1))
            return
        fi
        d1="$(diag_of "$WORK/$tag.run.err")"
        d2="$(diag_of "$WORK/$tag.build.err")"
        if cmp -s "$WORK/$tag.run.out" "$WORK/$tag.build.out" && \
           [ "$d1" = "$d2" ] && [ "$rc_r" -eq "$rc_b" ]; then
            echo "$tag NEG-IDENTICAL (exit $rc_r)"
        else
            echo "$tag NEG-DIFFER exit=$rc_r/$rc_b"
            echo "--- run stderr:"; cat "$WORK/$tag.run.err"
            echo "--- build stderr:"; cat "$WORK/$tag.build.err"
            _FAIL=$((_FAIL+1))
        fi
    fi
}

# ---- 双模字节一致检查 ----
check_dual "$WORK/main.sg"           'main'           1
check_dual "$WORK/pkgmain.sg"        'pkgmain'        1
check_dual "$WORK/vermain.sg"        'vermain'        1
check_dual "$WORK/badprivate.sg"     'badprivate'     0
check_dual "$WORK/badfromprivate.sg" 'badfromprivate' 0
check_dual "$WORK/badinternal.sg"    'badinternal'    0
check_dual "$WORK/badmissing.sg"     'badmissing'     0

# ---- 正例精确期望 stdout（PS1 以 \r 剥离后逐行比）----
expect_main='3
5
12
100
10
10
30
100
11
42
100
sub-c
tag:sub-c
tag:sub-c'
got_main="$(tr -d '\r' < "$WORK/main.run.out")"
if [ "$got_main" = "$expect_main" ]; then
    echo "main EXPECTED-STDOUT OK"
else
    echo "main EXPECTED-STDOUT MISMATCH"
    echo "--- expected:"; echo "$expect_main"
    echo "--- got:"; echo "$got_main"
    _FAIL=$((_FAIL+1))
fi

got_pkg="$(tr -d '\r' < "$WORK/pkgmain.run.out")"
if [ "$got_pkg" = "15" ]; then
    echo "pkgmain EXPECTED-STDOUT OK"
else
    echo "pkgmain EXPECTED-STDOUT MISMATCH"
    _FAIL=$((_FAIL+1))
fi

got_ver="$(tr -d '\r' < "$WORK/vermain.run.out")"
if [ "$got_ver" = "1.10" ]; then
    echo "vermain EXPECTED-STDOUT OK (1.10 picked)"
else
    echo "vermain EXPECTED-STDOUT MISMATCH (package version pick)"
    echo "$got_ver"
    _FAIL=$((_FAIL+1))
fi

# ---- 负例期望诊断码（stderr）----
for t in badprivate badfromprivate badinternal; do
    if grep -q 'E5003' "$WORK/$t.run.err"; then
        echo "$t E5003 OK"
    else
        echo "$t missing E5003"
        _FAIL=$((_FAIL+1))
    fi
done
if grep -q 'E5001' "$WORK/badmissing.run.err"; then
    echo "badmissing E5001 OK"
else
    echo "badmissing missing E5001"
    _FAIL=$((_FAIL+1))
fi

# ---- 接口契约：@private 不得导出（L7）----
( cd "$WORK" && "$SCOURGE" interface --out - a/b.sg ) >"$WORK/sgi.out" 2>"$WORK/sgi.err"; rc_sgi=$?
sgi="$(cat "$WORK/sgi.out")"
if [ "$rc_sgi" -ne 0 ]; then
    echo "interface a/b.sg FAIL"
    cat "$WORK/sgi.err"
    _FAIL=$((_FAIL+1))
elif grep -q 'secret' <<<"$sgi"; then
    echo "interface LEAKS @private secret"
    _FAIL=$((_FAIL+1))
elif ! grep -q 'def add' <<<"$sgi" || ! grep -q 'def mul' <<<"$sgi"; then
    echo "interface missing public funcs"
    _FAIL=$((_FAIL+1))
else
    echo "interface PRIVATE-HIDDEN OK"
fi

unset SCOURGE_PACKAGES
suite_done
