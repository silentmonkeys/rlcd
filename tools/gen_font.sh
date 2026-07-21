#!/usr/bin/env bash
# 用 lv_font_conv 从系统 TTF 生成 3 份 LVGL 字模。
#
# 输出：**LVGL binfont 二进制**（.bin），落到 partitions/fonts/。
# 通过 CMake 里的 spiffs_create_partition_image() 打包成 SPIFFS 镜像烧进
# fonts 分区（2 MB）。运行时 ui_font.c 用 lv_binfont_create("A:/spiffs/xxx.bin")
# 读取。加字只需：改本脚本 → 重跑 → `idf.py flash fonts`（只烧 2MB 分区，不动 app）。
#
# 依赖：node + `npm install -g lv_font_conv`
#
# 字符集：
#   - ASCII 0x20-0x7F + °(0xB0) + ×(0xD7) + ℃(0x2103) + 箭头(0x2190-0x2193)
#   - CJK：GB2312 一级 3755 汉字（区位 16-55）+ 常用标点符号（0x3000-0x301F 等）
set -e
cd "$(dirname "$0")/.."

FONT_CJK="/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"
FONT_ASCII="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONT_BOLD="/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
for f in "$FONT_CJK" "$FONT_ASCII" "$FONT_BOLD"; do
    if [ ! -f "$f" ]; then
        echo "缺 $f —— 请先 sudo apt install fonts-droid-fallback fonts-dejavu"
        exit 1
    fi
done

OUT_DIR="partitions/fonts"
mkdir -p "$OUT_DIR"

# ---------- 字符集范围 ----------
# GB2312 一级汉字：区位 16-55（每区 94 字，共 3755 字），Unicode 分布：
#   最简做法：直接指定连续的 Unicode 区间 0x4E00-0x9FA5（CJK Unified Ideographs
#   基本区）—— 但那有 20902 字，> 2MB 分区放不下（约 2.4MB）。
#   所以走 GB2312 精确列表：从 python 生成一次范围表，写死在这里。
#
# 常用标点（拆到 CJK 字体 vs 拉丁字体 —— 拉丁标点走 DejaVu，CJK 全角标点走 Droid）：
#   拉丁标点（ASCII 之外的常见）—— DejaVu 有
#     0x00B7           ·
#     0x2010-0x2015    ‐-‒–—―
#     0x2018-0x201F    ''‚‛""„‟
#     0x2022-0x2026    •‣․‥…
#     0x2030-0x2033    ‰′″‴
#     0x2039-0x203A    ‹›
#   CJK 全角标点 —— Droid 有
#     0x3000-0x301F    ideographic space、。〃〄〆〇〈〉《》「」『』【】等
#     0xFF00-0xFFEF    全角 ASCII + 全角标点（！？：；，。等）
LATIN_PUNCT_RANGES="\
-r 0x00B7-0x00B7 \
-r 0x2010-0x2015 \
-r 0x2018-0x201F \
-r 0x2022-0x2026 \
-r 0x2030-0x2033 \
-r 0x2039-0x203A \
"
CJK_PUNCT_RANGES="\
-r 0x3000-0x301F \
-r 0xFF00-0xFFEF \
"

# GB2312 一级汉字 Unicode 精确列表（16-55 区，共 3755 字）
# 生成方式（参考）：
#   python -c "for r in range(0x10,0x38):
#     for c in range(0x21,0x7F):
#       gb=bytes([r+0xA0,c+0xA0]); print(hex(int.from_bytes(gb.decode('gb2312').encode('utf-16-be'),'big')))"
# —— 但一级字实际范围就是 0xB0A1..0xD7F9（GB2312 编码），对应 Unicode 的分布不连续。
# 为了让脚本自包含，这里直接内联 Python，生成一次时映射写到临时文件。
GB1_TMP=$(mktemp)
python3 - "$GB1_TMP" <<'PY'
import sys, os
out = sys.argv[1]
# GB2312 一级字：编码从 0xB0A1 到 0xD7F9，共 3755 个（跳过 0xxx7F 保留）
codepoints = set()
for row in range(0xB0, 0xD8):        # 高字节
    for col in range(0xA1, 0xFF):    # 低字节
        gb = bytes([row, col])
        try:
            ch = gb.decode('gb2312')
        except UnicodeDecodeError:
            continue
        codepoints.add(ord(ch))
        if len(codepoints) >= 3755:
            break
# 用 lv_font_conv 的 -r 0xNNNN 逐字符列出，能被 shell 一行传下去
with open(out, 'w') as f:
    f.write(' '.join(f'-r 0x{cp:04X}' for cp in sorted(codepoints)))
PY
GB1_RANGES=$(cat "$GB1_TMP")
rm -f "$GB1_TMP"

# ---------- 1. CJK 16px（主字库）----------
echo "生成 CJK 16px 字库（GB2312 一级 3755 + 标点，可能需要 10 秒左右）…"
lv_font_conv \
    --font "$FONT_ASCII" -r 0x20-0x7F -r 0xB0-0xB0 -r 0xD7-0xD7 -r 0x2103-0x2103 -r 0x2190-0x2193 $LATIN_PUNCT_RANGES \
    --font "$FONT_CJK" $CJK_PUNCT_RANGES $GB1_RANGES \
    --size 16 --bpp 1 --format bin \
    --no-compress \
    -o "$OUT_DIR/ui_font_cjk_16.bin"
echo "生成 $OUT_DIR/ui_font_cjk_16.bin ($(stat -c%s $OUT_DIR/ui_font_cjk_16.bin) bytes)"

# ---------- 2. 大数字 96px（时钟 HH:MM）----------
lv_font_conv \
    --font "$FONT_BOLD" -r 0x20-0x20 -r 0x30-0x3A \
    --size 96 --bpp 1 --format bin \
    --no-compress \
    -o "$OUT_DIR/ui_font_digit_big.bin"
echo "生成 $OUT_DIR/ui_font_digit_big.bin ($(stat -c%s $OUT_DIR/ui_font_digit_big.bin) bytes)"

# ---------- 3. 中数字 28px（卡片数值）----------
lv_font_conv \
    --font "$FONT_BOLD" -r 0x25-0x25 -r 0x30-0x39 -r 0xB0-0xB0 -r 0x2103-0x2103 \
    --size 28 --bpp 1 --format bin \
    --no-compress \
    -o "$OUT_DIR/ui_font_digit_mid.bin"
echo "生成 $OUT_DIR/ui_font_digit_mid.bin ($(stat -c%s $OUT_DIR/ui_font_digit_mid.bin) bytes)"

TOTAL=$(du -sb "$OUT_DIR" | cut -f1)
echo ""
echo "字库目录总大小：$TOTAL bytes（分区上限 2 MiB = 2097152 bytes）"
echo "下一步：cd 到项目根 → idf.py build → idf.py flash（或 idf.py -p /dev/ttyUSB0 flash）"
echo "以后只想更新字库不动 app：idf.py -p /dev/ttyUSB0 -a flash-fonts"
