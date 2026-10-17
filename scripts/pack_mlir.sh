#!/usr/bin/env bash
# Copyright (c) 2026 Scourge Project. All rights reserved.
# ============================================================================
#  pack_mlir.sh —— LLVM/MLIR 库包打包（Linux，维护者用，容器/本机皆可）
#
#  方案：cmake --graphviz 依赖图 BFS 出 MLIR*/LLVM* 闭包（种子=项目 target
#  直接依赖，与 CMakeLists 零重复）；手工组装包——不用 cmake --install，
#  因 install 要求全树构建完整，而我们的构建树是按需部分构建：
#    include/  <- 源树 llvm/mlir 头 + 构建树生成头（cp -r 合并）
#    lib/      <- 仅闭包静态库（逐 target 校验映射真实文件）
#    lib/cmake <- 拷构建树 cmake 配置 + 绝对路径归一为包相对 ${_SCOURGE_PKG}
#                 （build 树 targets 文件是硬编码绝对路径，须重写才可搬运）
#    bin/      <- 仅留 mlir-tblgen
#  产出：dist/scourge-mlir-libs-linux-<arch>.tar.zst
#
#  用法：scripts/pack_mlir.sh [构建树，默认 /build]（SCOURGE_SRC_DIR=/src 可覆盖）
#  前置：构建树已完成闭包内 target 的构建；REPO_ROOT/build-rel 已构建（图源）。
# ============================================================================
set -euo pipefail

BUILD="${1:-/build}"
SRC="${SCOURGE_SRC_DIR:-/src}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="$(uname -m)"
OUT="scourge-mlir-libs-linux-${ARCH}"

WORK="$(mktemp -d)"
PKG="$WORK/llvm-mlir"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$PKG"

# ---- 闭包 ------------------------------------------------------------------
echo "[pack] cmake --graphviz"
DOT="$WORK/dep.dot"
cmake --graphviz "$DOT" -S "$REPO_ROOT" -B "$REPO_ROOT/build-rel" >/dev/null

# awk：直读边注释（行尾形式 `... -> ... [style] // A -> B`），BFS 收集
# ^(MLIR|LLVM) 闭包。纯 POSIX awk（Ubuntu 默认 mawk 无 gawk 的 match 三参
# 捕获；split("",arr) 是可移植清空）。
# 种子 = From 匹配 ^scourge 的 To（scourge 可执行/库 target 的直接依赖）
CLOSURE="$(awk '
    {
        p = index($0, "//")
        if (p > 0) {
            s = substr($0, p + 2)
            q = index(s, "->")
            if (q > 0) {
                a = substr(s, 1, q - 1); b = substr(s, q + 2)
                # 注释段形如 " A -> B"：a/b 两侧空格都须裁净（a 带前导空格
                # 会使 ^scourge 锚定失配 → seeds 恒空 → 闭包恒空）
                gsub(/^[ \t]+/, "", a); gsub(/[ \t]+$/, "", a)
                gsub(/^[ \t]+/, "", b); gsub(/[ \t]+$/, "", b)
                if (a != "" && b != "") { edges[a] = edges[a] " " b; all[++n] = a "->" b }
            }
        }
    }
    END {
        for (i = 1; i <= n; i++) {
            split(all[i], kv, "->"); if (kv[1] ~ /^scourge/) seeds[">" kv[2]] = 1
        }
        for (k in seeds) { f = substr(k, 2); frontier[f] = 1 }
        moved = 1
        while (moved) {
            moved = 0; split("", nf)
            for (x in frontier) {
                if (x in reach) continue
                reach[x] = 1; moved = 1
                if (x in edges) {
                    cnt = split(edges[x], tos, " ")
                    for (j = 1; j <= cnt; j++) nf[tos[j]] = 1
                }
            }
            for (y in nf) frontier[y] = 1
        }
        for (r in reach) if (r ~ /^(MLIR|LLVM)/ && r !~ /^obj\./) print r
    }' "$DOT" | sort)"
COUNT="$(printf '%s\n' "$CLOSURE" | grep -c . || true)"
echo "[pack] closure targets: $COUNT"
[ "$COUNT" -gt 0 ] || { echo "[pack] ERROR: empty closure"; exit 1; }

# ---- include（合并拷贝）----------------------------------------------------
echo "[pack] copy includes"
for inc in "$SRC/llvm/include" "$SRC/mlir/include" "$BUILD/include" "$BUILD/tools/mlir/include"; do
    if [ -d "$inc" ]; then
        cp -r "$inc/." "$PKG/include/"
    else
        echo "[pack] WARN include dir missing: $inc"
    fi
done

# ---- lib（仅闭包静态库，逐 target 校验）------------------------------------
echo "[pack] copy closure libs"
mkdir -p "$PKG/lib"
# INTERFACE IMPORTED target（纯头依赖，如 MLIRSparseTensorEnums）无 .a 产物，
# 从构建树 targets 文件识别并排除
TGT_TEXT="$(cat "$BUILD/lib/cmake/mlir/MLIRTargets.cmake" \
                 "$BUILD/lib/cmake/llvm/LLVMTargets.cmake" 2>/dev/null || true)"
MISSING=""
while IFS= read -r t; do
    if [ -f "$BUILD/lib/lib${t}.a" ]; then
        cp "$BUILD/lib/lib${t}.a" "$PKG/lib/"
    # 用 herestring 而非 printf|grep 管线：grep -q 首匹配即提前退出，大文本下
    # printf 未写完即被 SIGPIPE 杀掉，pipefail 会把 rc141 误判为"非 INTERFACE"
    elif grep -Eq "add_library\(\"?${t}\"? INTERFACE" <<< "$TGT_TEXT"; then
        :  # INTERFACE library，无产物
    elif [ -x "$BUILD/bin/${t}" ]; then
        :  # 工具类 target（llvm-tblgen 等），无 .a，跳过
    else
        MISSING="$MISSING $t"
    fi
done <<< "$CLOSURE"
[ -z "$MISSING" ] || { echo "[pack] ERROR: closure libs missing in build tree:$MISSING"; exit 1; }
echo "[pack] libs copied: $(ls "$PKG/lib" | grep -c '\.a$')"

# ---- lib/cmake（拷配置 + 绝对路径归一）--------------------------------------
# 替换规则（先具体后泛化）：
#   $BUILD/./ 与 $BUILD/ -> ${_SCOURGE_PKG}/（Config 模板含 /./ 中缀）
#   $SRC/llvm/cmake/modules -> ${_SCOURGE_PKG}/lib/cmake/llvm
#   $SRC/{llvm,mlir,third-party,libc,cmake} -> ${_SCOURGE_PKG}（源树整前缀归一；
#   非 include 目录解析到包内不存在路径无害——scourge 不编上游 tablegen 工具）
echo "[pack] copy cmake config (relocatable rewrite)"
for c in llvm mlir; do
    SRC_C="$BUILD/lib/cmake/$c"
    DST_C="$PKG/lib/cmake/$c"
    [ -d "$SRC_C" ] || { echo "[pack] ERROR: cmake config missing: $SRC_C"; exit 1; }
    mkdir -p "$DST_C"
    for f in "$SRC_C"/*; do
        {
            printf 'set(_SCOURGE_PKG "${CMAKE_CURRENT_LIST_DIR}/../../..")\n'
            sed -e "s|$BUILD/\./|\${_SCOURGE_PKG}/|g" \
                -e "s|$BUILD/|\${_SCOURGE_PKG}/|g" \
                -e "s|$BUILD|\${_SCOURGE_PKG}|g" \
                -e "s|$SRC/llvm/cmake/modules|\${_SCOURGE_PKG}/lib/cmake/llvm|g" \
                -e "s|$SRC/llvm|\${_SCOURGE_PKG}|g" \
                -e "s|$SRC/mlir|\${_SCOURGE_PKG}|g" \
                -e "s|$SRC/third-party|\${_SCOURGE_PKG}|g" \
                -e "s|$SRC/libc|\${_SCOURGE_PKG}|g" \
                -e "s|$SRC/cmake|\${_SCOURGE_PKG}|g" \
                -e "s|$SRC|\${_SCOURGE_PKG}|g" \
                "$f"
        } > "$DST_C/$(basename "$f")"
    done
done

# ---- residue check（兜底：包内不得残留打包机专属路径）----------------------
echo "[pack] residue check (machine-specific paths)"
BAD_FILES=""
for f in "$PKG"/lib/cmake/*/*.cmake; do
    if grep -q -e "$BUILD" -e "$SRC" "$f"; then
        BAD_FILES="$BAD_FILES $(basename "$f")"
    fi
done
[ -z "$BAD_FILES" ] || { echo "[pack] ERROR: machine paths remain in:$BAD_FILES"; exit 1; }

# ---- bin + LICENSE ----------------------------------------------------------
mkdir -p "$PKG/bin"
[ -x "$BUILD/bin/mlir-tblgen" ] || { echo "[pack] ERROR: mlir-tblgen not found"; exit 1; }
cp "$BUILD/bin/mlir-tblgen" "$PKG/bin/"

[ -f "$SRC/llvm/LICENSE.TXT" ] && cp "$SRC/llvm/LICENSE.TXT" "$PKG/LICENSE.TXT" \
    || echo "[pack] WARN LICENSE.TXT missing: $SRC/llvm/LICENSE.TXT"

# 记录 LLVM 版本：bootstrap 读它做 AOT clang 版本门卫（MLIR 产出的 IR 带
# 该版本属性，旧 clang 解析会报 unterminated attribute group）。
# 注意 LLVMConfig.cmake 该行不带引号：set(LLVM_PACKAGE_VERSION 23.1.3)
LLVM_VER="$(sed -n 's/.*LLVM_PACKAGE_VERSION ["[:space:]]*\([0-9][0-9.]*\).*/\1/p' \
    "$PKG/lib/cmake/llvm/LLVMConfig.cmake" | head -n1)"
if [ -n "$LLVM_VER" ]; then
    printf '%s\n' "$LLVM_VER" > "$PKG/LLVM_VERSION.txt"
else
    echo "[pack] WARN LLVM_PACKAGE_VERSION not found in LLVMConfig.cmake"
fi

# ---- 打包 -------------------------------------------------------------------
echo "[pack] tar.zst"
mkdir -p "$REPO_ROOT/dist"
TAROUT="$REPO_ROOT/dist/$OUT.tar.zst"
rm -f "$TAROUT"
tar --zstd -cf "$TAROUT" -C "$WORK" llvm-mlir
SHA="$(sha256sum "$TAROUT" | cut -d' ' -f1)"
echo "[pack] done: dist/$OUT.tar.zst  size=$(du -h "$TAROUT" | cut -f1)  sha256=$SHA"
