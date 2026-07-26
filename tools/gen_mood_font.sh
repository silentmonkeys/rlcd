#!/usr/bin/env bash
# 生成 ui_font_mood_16.bin —— 主页"心情表情"专用字模
#
# 字符集：
#   - ASCII 0x20-0x7F（表情括号、字母、数字等）
#   - 表情符号：·(U+00B7) ○(U+25CB) ▽(U+25BD) ●(U+25CF)
#   - 中文标签字：干冷寒湿凉微潮舒适热闷暴晒炎蒸笼
#   - 备用符号：℃(U+2103) °(U+0B0)
#
# 注意：CJK 字库(GB2312 一级)里没有 ▽(U+25BD)，所以必须由 mood 字体提供。
# 为了避免 fallback 后显示空白，mood 字体自包含所有表情+标签字符。
#
# 输出：partitions/fonts/ui_font_mood_16.bin
# 运行：bash tools/gen_mood_font.sh
set -e
cd "$(dirname "$0")/.."

FONT_CJK="/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"
FONT_ASCII="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
for f in "$FONT_CJK" "$FONT_ASCII"; do
    if [ ! -f "$f" ]; then
        echo "缺 $f —— 请先 sudo apt install fonts-droid-fallback fonts-dejavu"
        exit 1
    fi
done

OUT_DIR="partitions/fonts"
mkdir -p "$OUT_DIR"

# 文字标签用到的中文字符（去重排序）
MOOD_CN="干冷寒湿凉微潮舒适热闷暴晒炎蒸笼"

# 生成中文字符的 -r 范围（合并连续区间）
CN_RANGES=$(python3 -c "
s = '$MOOD_CN'
cps = sorted(set(ord(c) for c in s))
ranges = []
i = 0
while i < len(cps):
    j = i
    while j + 1 < len(cps) and cps[j+1] == cps[j] + 1:
        j += 1
    ranges.append(f'0x{cps[i]:04X}-0x{cps[j]:04X}')
    i = j + 1
print(' '.join(f'-r {r}' for r in ranges))
")

echo "生成 mood 16px 字库（ASCII + 表情符号 + 中文标签字）…"
lv_font_conv \
    --font "$FONT_ASCII" -r 0x20-0x7F -r 0x00B7 -r 0x25CB -r 0x25BD -r 0x25CF -r 0x2103 -r 0xB0 \
    --font "$FONT_CJK" $CN_RANGES \
    --size 16 --bpp 1 --format bin \
    --no-compress \
    -o "$OUT_DIR/ui_font_mood_16.bin"
echo "生成 $OUT_DIR/ui_font_mood_16.bin ($(stat -c%s $OUT_DIR/ui_font_mood_16.bin) bytes)"
echo ""
echo "下一步：cd 到项目根 → 重新编译模拟器即可看到效果"
