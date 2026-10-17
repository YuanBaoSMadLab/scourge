#!/usr/bin/env bash
# verify_net_time.sh - net_* / sleep 内建（0.9.4 F/G）双模字节一致性回归。
# 移植自 verify_net_time.ps1：需本地测试服务器（python net_test_server.py，
# TCP echo 23456 / 固定 HTTP 23458 / 立即关闭 23460）；probe_f_server 单独
# 处理（服务端探针，python 客户端中途对拍 23462）。
. "$(dirname "$0")/lib.sh"
suite net_time

# python 校验：找不到 PY 明确报错退出，绝不静默跳过。
# 特判：Windows Store 的 python3 假别名存在但不可用，lib.sh 探测结果须实测，
# 坏则回退 command -v python。
if [ -z "${PY:-}" ]; then
    echo "未找到 python：net/time 套件需要 python 运行对拍服务器/客户端" >&2
    exit 2
fi
if ! "$PY" -c 'pass' >/dev/null 2>&1; then
    PY="$(command -v python || true)"
fi
if [ -z "$PY" ] || ! "$PY" -c 'pass' >/dev/null 2>&1; then
    echo "python 不可用（\$PY=$PY）：net/time 套件需要 python 运行对拍服务器/客户端" >&2
    exit 2
fi

# ---- 本地三比：逐字移植 PS1 的 run/build/exec 三通道判定 ----
# 与 lib.sh dual 的差异：PS1 对 run 非零退出不早退（清单含对称 panic 探针）；
# filter=1 时 stdout 比对前滤除 elapsed_ms 行（PS1 的 -notmatch 特判）。
case "$(uname -s)" in
    MSYS*|MINGW*|CYGWIN*) SRCEXT=.exe ;;
    *)                    SRCEXT=.bin ;;
esac
dual_ps1() {
    local stem="$1" src="$2" filt="$3" rc1 rc2 rc3 r1 r3
    clean "$stem"
    "$SCOURGE" run "$src" >"$WORK/$stem.run.out" 2>"$WORK/$stem.run.err"; rc1=$?
    "$SCOURGE" build "$src" -o "$WORK/$stem$SRCEXT" >"$WORK/$stem.build.out" 2>"$WORK/$stem.build.err"; rc2=$?
    if [ "$rc2" -ne 0 ]; then
        echo "$stem BUILD-FAIL"; head -20 "$WORK/$stem.build.err"; _FAIL=$((_FAIL+1)); return
    fi
    "$WORK/$stem$SRCEXT" >"$WORK/$stem.aot.out" 2>"$WORK/$stem.aot.err"; rc3=$?
    if [ "$filt" -eq 1 ]; then
        r1="$(grep -v 'elapsed_ms' "$WORK/$stem.run.out")"
        r3="$(grep -v 'elapsed_ms' "$WORK/$stem.aot.out")"
    else
        r1="$(cat "$WORK/$stem.run.out")"; r3="$(cat "$WORK/$stem.aot.out")"
    fi
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

PB="$REPO_ROOT/verify/probes"

# ---- 助手服务器：echo 端口未监听时才启动（PS1 用 TcpClient 探测，
#      此处用 python 等价实现，保持"已监听则不重复起服"语义）----
srvProc=""
if ! "$PY" -c "import socket; socket.create_connection(('127.0.0.1', 23456), 1).close()" >/dev/null 2>&1; then
    "$PY" "$PB/net_test_server.py" >/dev/null 2>&1 & srvProc=$!
    # PS1 为 1.2s；Windows 下 python 启动略慢，超时放宽 2 倍
    sleep 2.4
fi

# ---- probe_f_*（排除 probe_f_server）+ probe_g_sleep：双模字节比对 ----
for sg in "$PB"/probe_f_*.sg; do
    [ -e "$sg" ] || continue
    stem="$(basename "$sg" .sg)"
    case "$stem" in probe_f_server) continue ;; esac
    dual_ps1 "$stem" "$sg" 1
done
dual_ps1 probe_g_sleep "$PB/probe_g_sleep.sg" 1

if [ -n "$srvProc" ]; then
    kill "$srvProc" 2>/dev/null
    wait "$srvProc" 2>/dev/null
fi

# ---- probe_f_server：先 build AOT，再 VM/AOT 各起服务端 + python 客户端 ----
sg="$PB/probe_f_server.sg"
stem="probe_f_server"
client="$PB/net_test_client.py"
clean "$stem"
"$SCOURGE" build "$sg" -o "$WORK/$stem$SRCEXT" >"$WORK/$stem.build.out" 2>"$WORK/$stem.build.err"
if [ $? -ne 0 ]; then
    echo "$stem BUILD-FAIL"; _FAIL=$((_FAIL+1))
else
    # Run 1: VM（scourge run），Run 2: AOT exe 直启（PS1 0.9s，放宽 2 倍）
    "$SCOURGE" run "$sg" >"$WORK/$stem.run.out" 2>"$WORK/$stem.run.err" & p1=$!
    sleep 1.8
    "$PY" "$client" >"$WORK/$stem.client.out" 2>"$WORK/$stem.client.err"
    wait "$p1"
    "$WORK/$stem$SRCEXT" >"$WORK/$stem.aot.out" 2>"$WORK/$stem.aot.err" & p2=$!
    sleep 1.8
    "$PY" "$client" >"$WORK/$stem.client.out" 2>"$WORK/$stem.client.err"
    wait "$p2"
    if [ "$(cat "$WORK/$stem.run.out")" = "$(cat "$WORK/$stem.aot.out")" ] && \
       cmp -s "$WORK/$stem.run.err" "$WORK/$stem.aot.err"; then
        echo "$stem IDENTICAL"
    else
        echo "$stem DIFFER"
        echo "--- run:"; head -5 "$WORK/$stem.run.out" 2>/dev/null
        echo "--- aot:"; head -5 "$WORK/$stem.aot.out" 2>/dev/null
        _FAIL=$((_FAIL+1))
    fi
fi

suite_done
