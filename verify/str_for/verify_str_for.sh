#!/usr/bin/env bash
# verify_str_for.sh - str 可迭代性双模字节一致性回归（0.9.7，缺陷 Q15）。
# 移植自 verify_str_for.ps1：for-over-str 字节迭代、逆序拼接、空串零趟循环、
# str 索引（越界经 except 捕获）、未捕获 panic 尾注 stderr 字节一致。
# Sema 负例 for-over-int 与 float 索引双通道 E2001。探针 .sg 内联写出。
. "$(dirname "$0")/../lib.sh"
suite str_for

PB="$REPO_ROOT/verify/probes"

# ---- 正例：run vs build+aot 三比（字节一致；正例 exit 0，直接用 lib.sh dual）----
dual probe_str_for "$PB/probe_str_for.sg"
dual probe_str_idx "$PB/probe_str_idx.sg"

# ---- 未捕获 panic：索引越界尾注必须字节一致（lib.sh panic 语义与 PS1 一致）----
cat > "$PB/probe_str_panic.sg" <<'EOF'
def main() -> int:
    s = "abc"
    print(s[5])
    return 0
EOF
panic probe_str_panic "$PB/probe_str_panic.sg"

# ---- Sema 负例：双通道静态拒绝 E2001 ----
cat > "$PB/neg_for_int.sg" <<'EOF'
def main() -> int:
    for x in 123:
        print(x)
    return 0
EOF
cat > "$PB/neg_idx_flt.sg" <<'EOF'
def main() -> int:
    s = "abc"
    print(s[1.5])
    return 0
EOF
reject neg_for_int "$PB/neg_for_int.sg" E2001
reject neg_idx_flt "$PB/neg_idx_flt.sg" E2001

suite_done
