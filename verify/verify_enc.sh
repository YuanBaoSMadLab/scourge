#!/usr/bin/env bash
# verify_enc.sh - 编码自适应验证（Task B）。
# 移植自 verify_enc.ps1：T1（GBK 可编码中文）在 6 种编码/换行变体下 stdout
# 字节全等；T2（非 BMP emoji）3 种编码全等；AOT build 产物输出与 run 全等。
. "$(dirname "$0")/lib.sh"
suite enc

rm -rf "$WORK"
mkdir -p "$WORK"

case "$(uname -s)" in
    MSYS*|MINGW*|CYGWIN*) SRCEXT=.exe ;;
    *)                    SRCEXT=.bin ;;
esac

t1='你好，世界ABC'
t2='你好🎉!'

# mk_probe <名> <文本> <编码> <crlf>：先按 UTF-8 生成源码字节流，再 iconv
# 转码并按需补 BOM（与 PS1 的 .NET Encoding 语义逐一对齐：UTF-16LE/BE、
# UTF-8 BOM 均带 BOM，GBK/UTF-8 无 BOM）
mk_probe() {
    local name="$1" text="$2" enc="$3" crlf="$4"
    local p="$WORK/$name.sg" tmp="$WORK/.tmp.sg"
    if [ "$crlf" = "1" ]; then
        printf 'def main() -> int:\r\n    print("%s")\r\n    return 0\r\n' "$text" > "$tmp"
    else
        printf 'def main() -> int:\n    print("%s")\n    return 0\n' "$text" > "$tmp"
    fi
    case "$enc" in
        utf8nb)  cp "$tmp" "$p" ;;
        utf8bom) printf '\xEF\xBB\xBF' > "$p"; cat "$tmp" >> "$p" ;;
        u16le)   printf '\xFF\xFE' > "$p"; iconv -f UTF-8 -t UTF-16LE "$tmp" >> "$p" ;;
        u16be)   printf '\xFE\xFF' > "$p"; iconv -f UTF-8 -t UTF-16BE "$tmp" >> "$p" ;;
        gbk)     iconv -f UTF-8 -t GBK "$tmp" > "$p" ;;
    esac
    rm -f "$tmp"
}

# run_probe <scg> <tag>：run --no-cache，失败打印诊断并 exit 1（与 PS1 一致）
run_probe() {
    local sgf="$1" tag="$2" rc
    "$SCOURGE" run --no-cache "$sgf" >"$WORK/$tag.out" 2>"$WORK/$tag.err"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "FAIL run exit $tag code=$rc"
        cat "$WORK/$tag.err"
        exit 1
    fi
    echo "$WORK/$tag.out"
}

hash_of() { sha256sum "$1" | cut -d' ' -f1; }

# compare_group <组名> <文件...>：组内所有输出 SHA256 全等
compare_group() {
    local g="$1"; shift
    local base
    base="$(hash_of "$1")"
    shift
    for f in "$@"; do
        if [ "$(hash_of "$f")" != "$base" ]; then
            echo "FAIL $g: output bytes differ"
            exit 1
        fi
    done
    echo "PASS $g (all $(( $# + 1 )) identical)"
}

# ---- 组 1：同一逻辑文本（GBK 可编码）跨编码/换行 ----
mk_probe enc_utf8nb      "$t1" utf8nb  0
mk_probe enc_utf8bom     "$t1" utf8bom 0
mk_probe enc_u16le       "$t1" u16le   0
mk_probe enc_u16be       "$t1" u16be   0
mk_probe enc_gbk         "$t1" gbk     0
mk_probe enc_utf8nb_crlf "$t1" utf8nb  1
f1="$(run_probe "$WORK/enc_utf8nb.sg"      g1_utf8nb)"
f2="$(run_probe "$WORK/enc_utf8bom.sg"    g1_utf8bom)"
f3="$(run_probe "$WORK/enc_u16le.sg"      g1_u16le)"
f4="$(run_probe "$WORK/enc_u16be.sg"      g1_u16be)"
f5="$(run_probe "$WORK/enc_gbk.sg"        g1_gbk)"
f6="$(run_probe "$WORK/enc_utf8nb_crlf.sg" g1_crlf)"
compare_group 'T1: utf8nb/utf8bom/u16le/u16be/gbk/crlf' "$f1" "$f2" "$f3" "$f4" "$f5" "$f6"

# ---- 组 2：非 BMP emoji（surrogate 解码路径）----
mk_probe enc2_utf8nb  "$t2" utf8nb  0
mk_probe enc2_utf8bom "$t2" utf8bom 0
mk_probe enc2_u16le   "$t2" u16le   0
h1="$(run_probe "$WORK/enc2_utf8nb.sg"  g2_utf8nb)"
h2="$(run_probe "$WORK/enc2_utf8bom.sg" g2_utf8bom)"
h3="$(run_probe "$WORK/enc2_u16le.sg"   g2_u16le)"
compare_group 'T2: utf8nb/utf8bom/u16le (emoji U+1F389)' "$h1" "$h2" "$h3"

# ---- AOT：build + 产物输出与 run 模式字节全等 ----
exe="$WORK/enc_aot$SRCEXT"
rm -f "$exe" "$WORK/aot.out" "$WORK/aot.err"
"$SCOURGE" build "$WORK/enc_utf8bom.sg" -o "$exe" >"$WORK/aot_build.out" 2>"$WORK/aot_build.err"
if [ $? -ne 0 ]; then
    echo "FAIL build exit"
    cat "$WORK/aot_build.err"
    exit 1
fi
"$exe" >"$WORK/aot.out" 2>"$WORK/aot.err"
if [ $? -ne 0 ]; then
    echo "FAIL aot exe exit"
    exit 1
fi
if [ "$(hash_of "$WORK/aot.out")" != "$(hash_of "$f1")" ]; then
    echo "FAIL AOT vs run: output bytes differ"
    exit 1
fi
echo "PASS AOT exe output identical to run mode"

# 人工核对用：打印 T1 输出实际解码字节
echo "--- stdout content (T1) ---"
echo "bytes: $(od -An -v -tx1 "$f1" | tr -s ' \n' ' ' | sed 's/^ //; s/ $//')"

suite_done
