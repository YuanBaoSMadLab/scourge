#!/usr/bin/env bash
# verify_stat.sh - 统计内建双模字节一致性回归（0.9.7）。
# 移植自 verify_stat.ps1：median 为新增，mean/var/std 先行（0.9.4 M），四者
# 均在此锁定：tensor/list 输入、奇偶 median、int 提升、NaN 拒绝。Sema 负例
# List[str] 双通道 E2001（探针内联写出）。
. "$(dirname "$0")/../lib.sh"
suite stat

PB="$REPO_ROOT/verify/probes"

# ---- 正例：run vs build+aot 三比（字节一致；正例 exit 0，直接用 lib.sh dual）----
dual probe_stat_median "$PB/probe_stat_median.sg"
dual probe_stat_exist  "$PB/probe_stat_exist.sg"

# ---- Sema 负例：List[str] 双通道 E2001 ----
cat > "$PB/probe_stat_neg.sg" <<'EOF'
def main() -> int:
    print(median(["a", "b"]))
    return 0
EOF
reject probe_stat_neg "$PB/probe_stat_neg.sg" E2001

suite_done
