#!/bin/bash
# ============================================================================
#  bootstrap.sh - Scourge 一键构建引导（Linux/macOS）
#
#  职责：让用户 clone 后一个脚本拿到可用的 scg——
#    1) 检查构建工具链（cmake/ninja/clang）
#    2) 获取平台预编译 MLIR 库包（默认 GitHub Release 附件；可 --url 覆盖，
#       或 --llvm-root 指向本机已有 LLVM/MLIR 跳过下载）
#    3) cmake 配置 + 构建 → build-rel/scg
#
#  说明：AOT build 命令运行时另需系统 clang 与链接器（发行版 clang 即可，
#  apt/dnf 装 clang；这是 scg 给 .sg 程序做最终链接的外部驱动，与编译
#  scg 本身无关）。
#  用法：
#    scripts/bootstrap.sh                 # 全自动（下载库包→构建）
#    scripts/bootstrap.sh --url <文件>    # 用本地/自定库包 zip|tar.zst
#    scripts/bootstrap.sh --llvm-root <目录>  # 用本机 LLVM/MLIR（含 lib/cmake/mlir）
#    scripts/bootstrap.sh --jobs 8        # 构建并行度（默认 nproc）
# ============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="$REPO_ROOT/.deps"
MLIR_ROOT="$DEPS_DIR/llvm-mlir"
OWNER_REPO="${SCOURGE_RELEASE_REPO:-}"   # 例：your-name/scourge
TAG="${SCOURGE_MLIR_TAG:-mlir-libs-v1}"  # 库包 release tag
ASSET_PFX="scourge-mlir-libs"

URL="" ; LLVM_ROOT="" ; JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
while [ $# -gt 0 ]; do
    case "$1" in
        --url)       URL="$2"; shift 2 ;;
        --llvm-root) LLVM_ROOT="$2"; shift 2 ;;
        --jobs)      JOBS="$2"; shift 2 ;;
        *) echo "未知参数: $1" >&2; exit 2 ;;
    esac
done

echo "[bootstrap] 工具链检查"
for t in cmake ninja clang; do
    command -v "$t" >/dev/null || { echo "缺少 $t：apt install cmake ninja-build clang（或发行版等价命令）" >&2; exit 1; }
done

if [ -n "$LLVM_ROOT" ]; then
    MLIR_ROOT="$LLVM_ROOT"
    echo "[bootstrap] 使用本机 LLVM/MLIR: $MLIR_ROOT"
else
        ARCH="$(uname -m)"; OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
        if [ -d "$MLIR_ROOT/lib/cmake/mlir" ]; then
            echo "[bootstrap] 库包已就位: $MLIR_ROOT（删除该目录可强制重新下载）"
        else
            # 零参数一键路径：无包源时自动探测本机 LLVM/MLIR——Debian/Ubuntu
            # 的 llvm-dev 自带 lib/cmake/mlir（官方 LLVM 预编译包不含 MLIR，
            # 这是发行版装 llvm-dev 即可一键的原因）。
            if [ -z "$URL" ] && [ -z "$OWNER_REPO" ]; then
                for cand in /usr/lib/llvm /usr/lib/llvm-20 /usr/lib/llvm-19 \
                            /usr/lib/llvm-18 /usr/lib/llvm-17 /usr/local/llvm; do
                    if [ -d "$cand/lib/cmake/mlir" ]; then
                        MLIR_ROOT="$cand"
                        echo "[bootstrap] 自动探测到本机 LLVM/MLIR: $MLIR_ROOT"
                        break
                    fi
                done
            fi
            if [ -d "$MLIR_ROOT/lib/cmake/mlir" ]; then
                echo "[bootstrap] 使用本机 LLVM/MLIR: $MLIR_ROOT"
            else
                ASSET="${ASSET_PFX}-${OS}-${ARCH}.tar.zst"
                if [ -z "$URL" ]; then
                    if [ -z "$OWNER_REPO" ]; then
                        echo "未找到可用 MLIR 库源。三选一：" >&2
                        echo "  a) 发行版安装：apt install llvm-dev clang（含 MLIR，零配置一键）" >&2
                        echo "  b) export SCOURGE_RELEASE_REPO=<owner/repo> 后重跑（自动拼 Release 附件 URL）" >&2
                        echo "  c) --url <本地或远程库包文件> / --llvm-root <本机 LLVM/MLIR 前缀>" >&2
                        exit 1
                    fi
                    URL="https://github.com/${OWNER_REPO}/releases/download/${TAG}/${ASSET}"
                fi
                echo "[bootstrap] 获取库包: $URL"
                mkdir -p "$DEPS_DIR"
                case "$URL" in
                    http*|ftp*)
                        if command -v curl >/dev/null; then curl -fL "$URL" -o "$DEPS_DIR/pkg.d"
                        else wget -O "$DEPS_DIR/pkg.d" "$URL"; fi ;;
                    *) cp "$URL" "$DEPS_DIR/pkg.d" ;;
                esac
                rm -rf "$MLIR_ROOT"
                case "$URL" in
                    *.tar.zst|*.zst) tar --zstd -xf "$DEPS_DIR/pkg.d" -C "$DEPS_DIR" ;;
                    *.tar.gz|*.tgz)  tar -xzf "$DEPS_DIR/pkg.d" -C "$DEPS_DIR" ;;
                    *.zip)           unzip -q "$DEPS_DIR/pkg.d" -d "$DEPS_DIR" ;;
                    *) echo "无法识别的库包格式: $URL" >&2; exit 1 ;;
                esac
                rm -f "$DEPS_DIR/pkg.d"
                [ -d "$MLIR_ROOT/lib/cmake/mlir" ] || { echo "库包结构异常（缺 lib/cmake/mlir）" >&2; exit 1; }
                echo "[bootstrap] 库包解压至 $MLIR_ROOT（含上游 LICENSE.TXT）"
            fi
        fi
    fi

# ---- AOT clang 版本门卫 ----------------------------------------------------
# MLIR 产出的 IR 带库包版本的属性（如 nocreateundeforpoison），旧系统 clang 在
# scg build 时会报 unterminated attribute group。包内自带 clang 则直接
# 导出 SCOURGE_LLVM_BIN；否则按 LLVM_VERSION.txt 比对大版本并警告。
if [ -x "$MLIR_ROOT/bin/clang" ]; then
    export SCOURGE_LLVM_BIN="$MLIR_ROOT/bin"
    echo "[bootstrap] 使用包内 clang: $MLIR_ROOT/bin"
elif [ -f "$MLIR_ROOT/LLVM_VERSION.txt" ]; then
    PKG_VER="$(cat "$MLIR_ROOT/LLVM_VERSION.txt")"
    PKG_MAJOR="${PKG_VER%%.*}"
    CLANG_MAJOR="$(clang --version 2>/dev/null | head -n1 \
        | sed -n 's/.*version \([0-9]*\).*/\1/p')"
    if [ -n "$PKG_MAJOR" ] && [ -n "$CLANG_MAJOR" ] && [ "$CLANG_MAJOR" -lt "$PKG_MAJOR" ]; then
        echo "[bootstrap] 警告: clang $CLANG_MAJOR 低于库包 LLVM $PKG_VER，AOT build 将失败。" >&2
        echo "  先安装匹配版本 clang：deb http://apt.llvm.org/<发行版>/ llvm-toolchain-<发行版>-$PKG_MAJOR main" >&2
    fi
fi

echo "[bootstrap] cmake 配置（build-rel）"
# 编译器选择：优先真 g++（--version 自报 g++ 才算；macOS 的 g++ 是 clang
# 别名自然回落）。原因：clang 对 LLVM 18.1.x AliasAnalysis.h 的内联构造
# 函数做急切实例化——计算异常规范要求完整 AAResults::Concept，而发行版
# 头文件只有前向声明（LLVM 23 才把构造函数移出线外），Ubuntu 24.04 的
# llvm-18 + clang 组合编译 scourge_aot 必挂；GCC 13 惰性处理无此问题，
# 且 LLVM 上游 CI 本就覆盖 GCC 13。
C_COMPILER=clang; CXX_COMPILER=clang++
if command -v g++ >/dev/null && g++ --version 2>/dev/null | head -n1 | grep -q 'g++'; then
    C_COMPILER=gcc; CXX_COMPILER=g++
    echo "[bootstrap] 编译器: gcc/g++（clang 对发行版 LLVM 18 头有异常规范急切实例化问题）"
fi
cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build-rel" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$C_COMPILER" -DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
    -DSCOURGE_LLVM_ROOT="$MLIR_ROOT" \
    -G Ninja

echo "[bootstrap] 构建（-j$JOBS）"
# 三件套：scg（主编译器）+ scourge-mcp（MCP 独立入口）+ sget（包管理器自举，
# 其 AOT 全链成功本身就是 scg build 能力的冒烟；目标名 sget_bootstrap 避开
# Linux 下目标名与产物文件名同名冲突）；scourge_tests 为 CTest 单测。
cmake --build "$REPO_ROOT/build-rel" -j"$JOBS" --target scourge scourge-mcp sget_bootstrap scourge_tests

BIN="$REPO_ROOT/build-rel"
echo "[bootstrap] 冒烟"
echo "[bootstrap]   scg:    $($BIN/scg run "$REPO_ROOT/examples/hello.sg" | tail -1)"
echo "[bootstrap]   sget:  $($BIN/sget list 2>/dev/null | head -1)"
MCP_INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
if printf '%s\n' "$MCP_INIT" | "$BIN/scourge-mcp" 2>/dev/null | grep -q 'serverInfo'; then
    echo "[bootstrap]   scourge-mcp: initialize OK"
else
    echo "[bootstrap] 警告: scourge-mcp initialize 冒烟无 serverInfo 响应" >&2
fi
echo "[bootstrap] 完成：$BIN/scg（+ scourge-mcp、sget）—— 立即可用"
echo "[bootstrap]   $BIN/scg run <file.sg>      # VM 直跑"
echo "[bootstrap]   $BIN/scg build <file.sg>    # AOT 产出独立可执行"
