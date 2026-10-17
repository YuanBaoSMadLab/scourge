#!/usr/bin/env bash
# verify_list.sh - List.append 标量域 + 列表推导（0.9.8）双模字节一致性回归。
# 移植自 verify_list.ps1：append int/float/bool/str 域、空表首插、别名可见、
# int->float 加宽；推导式 range/List/str 源、过滤、空结果、len 组合。
# Sema 负例（内联写出）双通道 E2001。
. "$(dirname "$0")/../lib.sh"
suite list

PB="$REPO_ROOT/verify/probes"

# ---- 正例：run vs build+aot 三比（字节一致；正例 exit 0，直接用 lib.sh dual）----
dual probe_list_append "$PB/probe_list_append.sg"
dual probe_list_comp   "$PB/probe_list_comp.sg"

# ---- Sema 负例：双通道 E2001（lib.sh reject 语义与 PS1 一致）----
# run stderr 带模式名 abort 尾注，build 不带 —— 诊断块本身才是共享契约。
cat > "$PB/probe_list_neg_type.sg" <<'EOF'
def main() -> int:
    xs: List[int] = [1]
    xs.append("s")
    return 0
EOF
cat > "$PB/probe_list_neg_arity.sg" <<'EOF'
def main() -> int:
    xs: List[int] = [1]
    xs.append(2, 3)
    return 0
EOF
cat > "$PB/probe_list_neg_dictsrc.sg" <<'EOF'
def main() -> int:
    d: Dict[str, int] = {"a": 1}
    xs: List[int] = [v for v in d]
    return 0
EOF
reject probe_list_neg_type    "$PB/probe_list_neg_type.sg"    E2001
reject probe_list_neg_arity   "$PB/probe_list_neg_arity.sg"   E2001
reject probe_list_neg_dictsrc "$PB/probe_list_neg_dictsrc.sg" E2001

suite_done
