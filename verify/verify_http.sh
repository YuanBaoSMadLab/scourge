#!/usr/bin/env bash
# verify_http.sh - HTTP server 探针（net_http_read_request/write_response）回归。
# 移植自 verify_http.ps1：build 探针 -> AOT exe 服务端 + python 客户端 ->
# VM run 服务端 + python 客户端 -> 对拍服务端日志字节一致。
. "$(dirname "$0")/lib.sh"
suite http

# python 校验：找不到 PY 明确报错退出，绝不静默跳过。
# 特判：Windows Store 的 python3 假别名存在但不可用，lib.sh 探测结果须实测，
# 坏则回退 command -v python。
if [ -z "${PY:-}" ]; then
    echo "未找到 python：http 套件需要 python 运行对拍客户端" >&2
    exit 2
fi
if ! "$PY" -c 'pass' >/dev/null 2>&1; then
    PY="$(command -v python || true)"
fi
if [ -z "$PY" ] || ! "$PY" -c 'pass' >/dev/null 2>&1; then
    echo "python 不可用（\$PY=$PY）：http 套件需要 python 运行对拍客户端" >&2
    exit 2
fi

case "$(uname -s)" in
    MSYS*|MINGW*|CYGWIN*) SRCEXT=.exe ;;
    *)                    SRCEXT=.bin ;;
esac

sg="$REPO_ROOT/verify/probes/probe_http_acc_server.sg"
stem="probe_http_acc_server"
client="$REPO_ROOT/verify/http_test_client.py"

# ---- Part 1：build AOT 服务端（失败即 exit 1，与 PS1 一致）----
clean "$stem"
"$SCOURGE" build "$sg" -o "$WORK/$stem$SRCEXT" >"$WORK/$stem.build.out" 2>"$WORK/$stem.build.err"
if [ $? -ne 0 ]; then
    echo "$stem BUILD-FAIL"; cat "$WORK/$stem.build.err"; exit 1
fi

# ---- Part 2：aot / vm 两模式各起服务端 + python 客户端 ----
fail=0
for mode in aot vm; do
    log="$WORK/$stem.$mode.log"
    if [ "$mode" = "aot" ]; then
        "$WORK/$stem$SRCEXT" >"$log" 2>"$WORK/$stem.$mode.err" & srv=$!
    else
        "$SCOURGE" run "$sg" >"$log" 2>"$WORK/$stem.$mode.err" & srv=$!
    fi
    # PS1 为 1.2s；Windows 下 python 启动略慢，超时放宽 2 倍
    sleep 2.4
    "$PY" "$client" >"$WORK/$stem.$mode.client.out" 2>"$WORK/$stem.$mode.client.err"
    crc=$?
    kill "$srv" 2>/dev/null
    wait "$srv" 2>/dev/null
    cliPass=0; [ "$crc" -eq 0 ] && cliPass=1
    srvBye=0;  grep -qx 'bye' "$log" 2>/dev/null && srvBye=1
    if [ "$cliPass" -eq 1 ] && [ "$srvBye" -eq 1 ]; then
        echo "$stem $mode PASS (client exit 0, server bye)"
    else
        echo "$stem $mode FAIL client=$cliPass bye=$srvBye"
        head -3 "$WORK/$stem.$mode.err" 2>/dev/null
        head -5 "$log" 2>/dev/null
        fail=$((fail+1))
    fi
done

# ---- Part 3：双模服务端日志字节一致 ----
if cmp -s "$WORK/$stem.aot.log" "$WORK/$stem.vm.log"; then
    echo "$stem logs IDENTICAL"
else
    echo "$stem logs DIFFER"
    fail=$((fail+1))
fi

[ "$fail" -eq 0 ] || _FAIL=$((_FAIL+fail))
suite_done
